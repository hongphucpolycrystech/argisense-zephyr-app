/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ph_measurement.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <argisense/drivers/adc/ads124s08.h>

#include "argisense_settings.h"

LOG_MODULE_REGISTER(argisense_ph_measurement, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define PH_ADC_NODE DT_ALIAS(ph_adc)
#define PH_BIAS_DAC_NODE DT_ALIAS(ph_bias_dac)

#define PH_PT1000_POSITIVE_INPUT DT_PROP(PH_ADC_NODE, pt1000_positive_input)
#define PH_PT1000_NEGATIVE_INPUT DT_PROP(PH_ADC_NODE, pt1000_negative_input)
#define PH_PT1000_IDAC_UA DT_PROP(PH_ADC_NODE, idac_current_microamp)
#define PH_DAC_REFERENCE_MV DT_PROP(PH_BIAS_DAC_NODE, reference_millivolt)
#define PH_DAC_GAIN DT_PROP(PH_BIAS_DAC_NODE, gain)
#define PH_DAC_FULL_SCALE_MV (PH_DAC_REFERENCE_MV * PH_DAC_GAIN)
#define PH_DAC_MAX_RAW UINT16_MAX
#define PH_VDS_TARGET_UV (-500000)
#define PH_VDS_TOLERANCE_UV 200000
#define PH_X1000_NEUTRAL 7000

enum ph_bias_channel {
	PH_DAC_ID_SET_ISFET = 0,
	PH_DAC_VDS_SET_ISFET = 1,
	PH_DAC_ID_SET_REFET = 2,
	PH_DAC_VDS_SET_REFET = 3,
	PH_DAC_BULK_ISFET = 4,
	PH_DAC_SUBS_ISFET = 5,
	PH_DAC_BULK_REFET = 6,
	PH_DAC_SUBS_REFET = 7,
};

enum ph_adc_channel {
	PH_AIN_VS_ISFET = 0,
	PH_AIN_VS_REFET = 1,
	PH_AIN_PT1000_P = 2,
	PH_AIN_PT1000_N = 3,
	PH_AIN_ISFET_BULK = 4,
	PH_AIN_ISFET_SUBS = 5,
	PH_AIN_REFET_BULK = 6,
	PH_AIN_REFET_SUBS = 7,
	PH_AIN_ISFET_DRAIN = 8,
	PH_AIN_REFET_DRAIN = 9,
	PH_AIN_GATE = 10,
};

static const struct device *const ph_adc = DEVICE_DT_GET(PH_ADC_NODE);
static const struct device *const ph_bias_dac = DEVICE_DT_GET(PH_BIAS_DAC_NODE);
static const struct gpio_dt_spec pre_power =
	GPIO_DT_SPEC_GET(USER_NODE, pre_power_gpios);
static const struct gpio_dt_spec analog_power =
	GPIO_DT_SPEC_GET(USER_NODE, analog_power_gpios);
static const struct gpio_dt_spec adc_power =
	GPIO_DT_SPEC_GET(USER_NODE, adc_power_gpios);
static const struct gpio_dt_spec electrode_zero =
	GPIO_DT_SPEC_GET(USER_NODE, electrode_zero_gpios);

static uint32_t dac_raw_from_mv(uint32_t millivolt)
{
	uint64_t raw = (uint64_t)millivolt * PH_DAC_MAX_RAW;

	raw += PH_DAC_FULL_SCALE_MV / 2U;
	raw /= PH_DAC_FULL_SCALE_MV;

	return (uint32_t)MIN(raw, PH_DAC_MAX_RAW);
}

static int set_gpio_if_ready(const struct gpio_dt_spec *gpio, bool enabled)
{
	if (!gpio_is_ready_dt(gpio)) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(gpio, enabled);
}

static int configure_output(const struct gpio_dt_spec *gpio, const char *name)
{
	int ret;

	if (!gpio_is_ready_dt(gpio)) {
		LOG_WRN("%s GPIO controller is not ready", name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("%s GPIO configure failed: %d", name, ret);
	}

	return ret;
}

static void power_on_for_measurement(void)
{
	set_gpio_if_ready(&pre_power, true);
	set_gpio_if_ready(&adc_power, true);
	set_gpio_if_ready(&analog_power, true);
	ph_measurement_set_electrode_zero(false);
	k_msleep(CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS);
}

static int setup_bias_dac_channels(void)
{
	const struct dac_channel_cfg cfg = {
		.resolution = 16U,
	};
	int first_error = 0;

	if (!device_is_ready(ph_bias_dac)) {
		return -ENODEV;
	}

	for (uint8_t channel = 0U; channel < 8U; channel++) {
		struct dac_channel_cfg channel_cfg = cfg;
		int ret;

		channel_cfg.channel_id = channel;
		ret = dac_channel_setup(ph_bias_dac, &channel_cfg);
		if (ret < 0 && first_error == 0) {
			first_error = ret;
		}
	}

	return first_error;
}

static int write_bias_mv(uint8_t channel, uint32_t millivolt)
{
	if (!device_is_ready(ph_bias_dac)) {
		return -ENODEV;
	}

	return dac_write_value(ph_bias_dac, channel, dac_raw_from_mv(millivolt));
}

int ph_measurement_set_bias_mv(uint8_t channel, uint32_t millivolt)
{
	if (channel > PH_DAC_SUBS_REFET || millivolt > PH_DAC_FULL_SCALE_MV) {
		return -EINVAL;
	}

	power_on_for_measurement();

	return write_bias_mv(channel, millivolt);
}

static uint32_t source_tracking_mv(int32_t source_uv)
{
	uint32_t source_mv;

	if (source_uv <= 0) {
		return 0U;
	}

	source_mv = (uint32_t)((source_uv + 500) / 1000);
	return MIN(source_mv, PH_DAC_FULL_SCALE_MV);
}

static int apply_source_tracking_biases(
	const struct argisense_ph_measurement_sample *sample)
{
	const uint32_t isfet_source_mv =
		source_tracking_mv(sample->vs_isfet_uv);
	const uint32_t refet_source_mv =
		source_tracking_mv(sample->vs_refet_uv);
	int first_error = 0;
	int ret;

	ret = write_bias_mv(PH_DAC_BULK_ISFET, isfet_source_mv);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = write_bias_mv(PH_DAC_SUBS_ISFET, isfet_source_mv);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = write_bias_mv(PH_DAC_BULK_REFET, refet_source_mv);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = write_bias_mv(PH_DAC_SUBS_REFET, refet_source_mv);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	if (first_error == 0) {
		LOG_DBG("AD5675 source tracking: ISFET bulk/subs=%u mV REFET bulk/subs=%u mV",
			isfet_source_mv, refet_source_mv);
		k_msleep(1);
	}

	return first_error;
}

int ph_measurement_set_electrode_zero(bool closed)
{
	return set_gpio_if_ready(&electrode_zero, closed);
}

int ph_measurement_read_adc_uv(uint8_t positive, uint8_t negative,
			       bool differential, bool enable_idac,
			       uint8_t idac_pin, int32_t *raw,
			       int32_t *microvolts)
{
	int32_t adc_raw;
	int ret;

	if (raw == NULL || microvolts == NULL ||
	    positive >= ADS124S08_CHANNEL_COUNT ||
	    (differential && negative > ADS124S08_AINCOM) ||
	    (enable_idac && idac_pin > ADS124S08_AINCOM)) {
		return -EINVAL;
	}

	if (!device_is_ready(ph_adc)) {
		return -ENODEV;
	}

	power_on_for_measurement();

	if (differential) {
		ret = ads124s08_read_differential(ph_adc, positive, negative,
						  enable_idac, idac_pin,
						  &adc_raw);
	} else {
		ret = ads124s08_read_single_ended(ph_adc, positive, &adc_raw);
	}

	if (ret < 0) {
		return ret;
	}

	ret = ads124s08_raw_to_microvolts(ph_adc, adc_raw, microvolts);
	if (ret < 0) {
		return ret;
	}

	*raw = adc_raw;
	return 0;
}

static int apply_initial_biases(void)
{
	int ret;
	int first_error = 0;

	ret = write_bias_mv(PH_DAC_ID_SET_ISFET, 448U);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = write_bias_mv(PH_DAC_VDS_SET_ISFET, 500U);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = write_bias_mv(PH_DAC_ID_SET_REFET, 448U);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = write_bias_mv(PH_DAC_VDS_SET_REFET, 500U);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	/* Safe preload; the measurement flow rewrites these from Vs readings. */
	for (uint8_t channel = PH_DAC_BULK_ISFET;
	     channel <= PH_DAC_SUBS_REFET; channel++) {
		ret = write_bias_mv(channel, 0U);
		if (ret < 0 && first_error == 0) {
			first_error = ret;
		}
	}

	return first_error;
}

static int pt1000_temperature_centi_c(int32_t voltage_uv,
				      int32_t *temperature_centi_c)
{
	int64_t resistance_milliohm;
	int64_t temperature;

	if (temperature_centi_c == NULL || PH_PT1000_IDAC_UA == 0) {
		return -EINVAL;
	}

	resistance_milliohm = ((int64_t)voltage_uv * 1000LL) /
			      PH_PT1000_IDAC_UA;

	/*
	 * Bring-up approximation for IEC 60751 PT1000 near 0 degC:
	 * R = R0 * (1 + alpha * T), alpha = 0.00385.
	 */
	temperature = (resistance_milliohm - 1000000LL) * 100LL;
	temperature /= 3850LL;

	if (temperature > INT32_MAX || temperature < INT32_MIN) {
		return -ERANGE;
	}

	*temperature_centi_c = (int32_t)temperature;
	return 0;
}

static int read_single_ended_uv(uint8_t channel, int32_t *microvolts)
{
	int32_t raw;
	int ret;

	ret = ads124s08_read_single_ended(ph_adc, channel, &raw);
	if (ret < 0) {
		return ret;
	}

	return ads124s08_raw_to_microvolts(ph_adc, raw, microvolts);
}

static bool voltage_near(int32_t measured_uv, int32_t target_uv,
			 int32_t tolerance_uv)
{
	int32_t delta = measured_uv - target_uv;

	return delta >= -tolerance_uv && delta <= tolerance_uv;
}

static int calculate_ph_x1000(
	const struct argisense_runtime_config *config,
	struct argisense_ph_measurement_sample *sample)
{
	int64_t slope_uv_per_ph;
	int64_t ph_x1000;
	int64_t temp_multiplier_ppm = 1000000LL;
	int32_t temperature_centi_c = config->ph_temp_reference_centi_c;
	bool use_temp = sample->temperature_valid;

	if (config->ph_calibration_valid == 0U ||
	    config->ph_calibration_mode == ARGISENSE_PH_CAL_MODE_DISABLED) {
		return -ENODATA;
	}

	if (config->ph_calibration_mode == ARGISENSE_PH_CAL_MODE_ISFET_REFET) {
		sample->ph_raw_uv = sample->vgs_isfet_uv -
				    sample->vgs_refet_uv;
	} else if (config->ph_calibration_mode ==
		   ARGISENSE_PH_CAL_MODE_REFERENCE_ELECTRODE) {
		sample->ph_raw_uv = sample->vgs_isfet_uv;
	} else {
		return -EINVAL;
	}

	if (use_temp) {
		temperature_centi_c = sample->temperature_centi_c;
		sample->ph_operating_status |=
			ARGISENSE_PH_OPERATING_STATUS_TEMP_COMP_USED;
	}

	temp_multiplier_ppm +=
		((int64_t)config->ph_temp_coeff_ppm_per_c *
		 (temperature_centi_c - config->ph_temp_reference_centi_c)) /
		100LL;
	slope_uv_per_ph =
		((int64_t)config->ph_slope_uv_per_ph * temp_multiplier_ppm) /
		1000000LL;

	if (slope_uv_per_ph > -1000LL && slope_uv_per_ph < 1000LL) {
		return -ERANGE;
	}

	ph_x1000 = PH_X1000_NEUTRAL +
		   (((int64_t)sample->ph_raw_uv - config->ph7_raw_uv) *
		    1000LL) /
			   slope_uv_per_ph;
	ph_x1000 = (ph_x1000 * config->ph_slope_x1000) / 1000LL +
		   config->ph_offset_x1000;

	if (ph_x1000 > INT32_MAX || ph_x1000 < INT32_MIN) {
		return -ERANGE;
	}

	sample->ph_x1000 = (int32_t)ph_x1000;
	sample->ph_operating_status |=
		ARGISENSE_PH_OPERATING_STATUS_CAL_VALID |
		ARGISENSE_PH_OPERATING_STATUS_RAW_VALID;

	return 0;
}

int ph_measurement_init(void)
{
	int ret;
	int first_error = 0;

	ret = configure_output(&pre_power, "pre power");
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = configure_output(&analog_power, "analog power");
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = configure_output(&adc_power, "ADC power");
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = configure_output(&electrode_zero, "electrode zero");
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	ret = setup_bias_dac_channels();
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	LOG_INF("pH measurement layer: ADS124S08=%s AD5675=%s",
		device_is_ready(ph_adc) ? "ready" : "not-ready",
		device_is_ready(ph_bias_dac) ? "ready" : "not-ready");

	return first_error;
}

void ph_measurement_sample(struct argisense_ph_measurement_sample *sample)
{
	int32_t pt1000_raw = 0;
	int32_t pt1000_uv = 0;
	struct argisense_runtime_config config;
	int ret;
	bool raw_ready = false;
	bool isfet_vds_ok = false;
	bool refet_vds_ok = false;

	if (sample == NULL) {
		return;
	}

	argisense_settings_get_copy(&config);

	*sample = (struct argisense_ph_measurement_sample){
		.dac0_current_ua = CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA,
		.dac1_current_ua = CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA,
		.ph_last_error = -ENODATA,
		.temperature_last_error = -ENODATA,
		.ph_adc_status = device_is_ready(ph_adc) ? BIT(0) : 0U,
		.bias_dac_status = device_is_ready(ph_bias_dac) ? BIT(0) : 0U,
		.sample_ready = true,
	};

	power_on_for_measurement();

	ret = apply_initial_biases();
	if (ret == 0) {
		sample->bias_dac_status |= BIT(1);
	} else {
		sample->bias_dac_status |= BIT(15);
	}

	if (!device_is_ready(ph_adc)) {
		sample->ph_last_error = -ENODEV;
		sample->temperature_last_error = -ENODEV;
		return;
	}

	ret = read_single_ended_uv(PH_AIN_VS_ISFET, &sample->vs_isfet_uv);
	if (ret == 0) {
		ret = read_single_ended_uv(PH_AIN_VS_REFET,
					   &sample->vs_refet_uv);
	}
	if (ret == 0) {
		ret = read_single_ended_uv(PH_AIN_GATE, &sample->gate_uv);
	}
	if (ret == 0) {
		sample->vgs_isfet_uv = sample->gate_uv - sample->vs_isfet_uv;
		sample->vgs_refet_uv = sample->gate_uv - sample->vs_refet_uv;
		sample->ph_raw_uv = sample->vgs_isfet_uv -
				     sample->vgs_refet_uv;
		raw_ready = true;
		sample->ph_adc_status |= BIT(1);
		sample->ph_operating_status |=
			ARGISENSE_PH_OPERATING_STATUS_RAW_VALID;
		ret = apply_source_tracking_biases(sample);
		if (ret == 0) {
			sample->bias_dac_status |= BIT(2);
		} else {
			sample->bias_dac_status |= BIT(15);
		}
	} else {
		sample->ph_last_error = ret;
	}

	if (read_single_ended_uv(PH_AIN_ISFET_DRAIN,
				 &sample->isfet_drain_uv) == 0 &&
	    raw_ready) {
		isfet_vds_ok = voltage_near(sample->isfet_drain_uv -
						    sample->vs_isfet_uv,
					    PH_VDS_TARGET_UV,
					    PH_VDS_TOLERANCE_UV);
		if (isfet_vds_ok) {
			sample->ph_operating_status |=
				ARGISENSE_PH_OPERATING_STATUS_ISFET_VDS_OK;
		}
	}

	if (read_single_ended_uv(PH_AIN_REFET_DRAIN,
				 &sample->refet_drain_uv) == 0 &&
	    raw_ready) {
		refet_vds_ok = voltage_near(sample->refet_drain_uv -
						    sample->vs_refet_uv,
					    PH_VDS_TARGET_UV,
					    PH_VDS_TOLERANCE_UV);
		if (refet_vds_ok) {
			sample->ph_operating_status |=
				ARGISENSE_PH_OPERATING_STATUS_REFET_VDS_OK;
		}
	}

	if (read_single_ended_uv(PH_AIN_ISFET_BULK,
				 &sample->isfet_bulk_uv) == 0 &&
	    read_single_ended_uv(PH_AIN_ISFET_SUBS,
				 &sample->isfet_subs_uv) == 0 &&
	    read_single_ended_uv(PH_AIN_REFET_BULK,
				 &sample->refet_bulk_uv) == 0 &&
	    read_single_ended_uv(PH_AIN_REFET_SUBS,
				 &sample->refet_subs_uv) == 0) {
		sample->ph_adc_status |= BIT(3);
	}

	ret = ads124s08_read_differential(ph_adc, PH_PT1000_POSITIVE_INPUT,
					  PH_PT1000_NEGATIVE_INPUT, true,
					  PH_PT1000_POSITIVE_INPUT,
					  &pt1000_raw);
	if (ret == 0) {
		ret = ads124s08_raw_to_microvolts(ph_adc, pt1000_raw,
						  &pt1000_uv);
	}
	sample->pt1000_uv = pt1000_uv;

	if (ret == 0) {
		ret = pt1000_temperature_centi_c(pt1000_uv,
						 &sample->temperature_centi_c);
	}

	if (ret == 0) {
		sample->ph_adc_status |= BIT(2);
		sample->temperature_last_error = 0;
		sample->temperature_valid = true;
	} else {
		sample->temperature_last_error = ret;
	}

	if (!raw_ready) {
		return;
	}

	if (config.ph_calibration_valid != 0U) {
		sample->ph_adc_status |= BIT(4);
	}

	if (config.ph_calibration_mode ==
		    ARGISENSE_PH_CAL_MODE_ISFET_REFET &&
	    (!isfet_vds_ok || !refet_vds_ok)) {
		sample->ph_last_error = -EAGAIN;
		return;
	}

	if (config.ph_calibration_mode ==
		    ARGISENSE_PH_CAL_MODE_REFERENCE_ELECTRODE &&
	    !isfet_vds_ok) {
		sample->ph_last_error = -EAGAIN;
		return;
	}

	ret = calculate_ph_x1000(&config, sample);
	if (ret == 0) {
		sample->ph_last_error = 0;
		sample->ph_valid = true;
		sample->ph_operating_status |=
			ARGISENSE_PH_OPERATING_STATUS_PH_VALID;
	} else {
		sample->ph_last_error = ret;
	}
}
