/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "current_loop_output.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(argisense_ph_loop, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define DAC0_NODE DT_ALIAS(current_loop_dac0)
#define DAC1_NODE DT_ALIAS(current_loop_dac1)

#define GP8302_CHANNEL 0U
#define GP8302_RESOLUTION_BITS 12U
#define GP8302_RAW_MAX 0x0FFFU

BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, dac_power_gpios),
	     "zephyr,user must define dac-power-gpios");

struct current_loop_channel {
	const struct device *dev;
	const char *name;
	uint32_t max_current_ua;
};

static const struct current_loop_channel loop_channels[] = {
	{
		.dev = DEVICE_DT_GET(DAC0_NODE),
		.name = "DAC0 pH GP8302",
		.max_current_ua = DT_PROP(DAC0_NODE, max_current_microamp),
	},
	{
		.dev = DEVICE_DT_GET(DAC1_NODE),
		.name = "DAC1 temperature GP8302",
		.max_current_ua = DT_PROP(DAC1_NODE, max_current_microamp),
	},
};

static const struct gpio_dt_spec dac_power =
	GPIO_DT_SPEC_GET(USER_NODE, dac_power_gpios);

static int32_t clamp_i32(int32_t value, int32_t low, int32_t high)
{
	if (value < low) {
		return low;
	}

	if (value > high) {
		return high;
	}

	return value;
}

static int set_dac_power(bool enabled)
{
	int ret;

	if (!gpio_is_ready_dt(&dac_power)) {
		LOG_WRN("DAC power GPIO controller is not ready");
		return -ENODEV;
	}

	ret = gpio_pin_set_dt(&dac_power, enabled);
	if (ret < 0) {
		LOG_ERR("DAC power GPIO set failed: %d", ret);
	}

	return ret;
}

static int ensure_dac_power_for_write(void)
{
	int ret;

	if (!IS_ENABLED(CONFIG_ARGISENSE_DAC_POWER_DURING_MEASUREMENT) &&
	    !IS_ENABLED(CONFIG_ARGISENSE_DAC_POWER_IDLE)) {
		LOG_WRN("DAC power is disabled by Kconfig; skip GP8302 writes");
		return -EACCES;
	}

	ret = set_dac_power(true);
	if (ret < 0) {
		return ret;
	}

	k_msleep(CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS);
	return 0;
}

static void apply_dac_idle_policy(void)
{
	if (!IS_ENABLED(CONFIG_ARGISENSE_DAC_POWER_IDLE)) {
		(void)set_dac_power(false);
	}
}

static int setup_channel(const struct current_loop_channel *channel)
{
	const struct dac_channel_cfg cfg = {
		.channel_id = GP8302_CHANNEL,
		.resolution = GP8302_RESOLUTION_BITS,
	};
	int ret;

	if (!device_is_ready(channel->dev)) {
		LOG_WRN("%s device is not ready", channel->name);
		return -ENODEV;
	}

	ret = dac_channel_setup(channel->dev, &cfg);
	if (ret < 0) {
		LOG_ERR("%s setup failed: %d", channel->name, ret);
		return ret;
	}

	return 0;
}

static int32_t scale_current_ua(int32_t value, int32_t range_low,
				int32_t range_high, bool valid,
				const struct argisense_runtime_config *config)
{
	const int64_t range_span = (int64_t)range_high - range_low;
	int64_t value_span;

	if (!valid || range_span <= 0) {
		return config->dac_fault_current_ua;
	}

	value_span = (int64_t)value - range_low;
	if (value_span <= 0) {
		return config->dac_min_current_ua;
	}

	if (value_span >= range_span) {
		return config->dac_max_current_ua;
	}

	return config->dac_min_current_ua +
	       (int32_t)((value_span *
				  (config->dac_max_current_ua -
				   config->dac_min_current_ua) +
			  (range_span / 2)) /
			 range_span);
}

static int32_t apply_trim_ua(int32_t current_ua, int32_t trim_low_ua,
			     int32_t trim_high_ua,
			     const struct argisense_runtime_config *config,
			     uint32_t channel_max_current_ua)
{
	const int64_t output_span =
		(int64_t)config->dac_max_current_ua - config->dac_min_current_ua;
	int64_t current_span;
	int32_t trim_ua;

	if (output_span <= 0) {
		return current_ua;
	}

	current_span = (int64_t)current_ua - config->dac_min_current_ua;
	if (current_span <= 0) {
		trim_ua = trim_low_ua;
	} else if (current_span >= output_span) {
		trim_ua = trim_high_ua;
	} else {
		trim_ua = trim_low_ua +
			  (int32_t)((current_span *
					     (trim_high_ua - trim_low_ua) +
				     (output_span / 2)) /
				    output_span);
	}

	return clamp_i32(current_ua + trim_ua, 0,
			 (int32_t)channel_max_current_ua);
}

static uint32_t raw_from_current_ua(int32_t current_ua, uint32_t max_current_ua)
{
	uint32_t clamped_current_ua;

	if (current_ua <= 0) {
		return 0U;
	}

	if ((uint32_t)current_ua >= max_current_ua) {
		return GP8302_RAW_MAX;
	}

	clamped_current_ua = (uint32_t)current_ua;

	return (uint32_t)(((uint64_t)clamped_current_ua * GP8302_RAW_MAX +
			   (max_current_ua / 2U)) /
			  max_current_ua);
}

static int write_channel_current(const struct current_loop_channel *channel,
				 int32_t current_ua)
{
	uint32_t raw;
	int ret;

	if (!device_is_ready(channel->dev)) {
		return -ENODEV;
	}

	raw = raw_from_current_ua(current_ua, channel->max_current_ua);
	ret = dac_write_value(channel->dev, GP8302_CHANNEL, raw);
	if (ret < 0) {
		LOG_ERR("%s write failed: current=%d uA raw=0x%03x ret=%d",
			channel->name, current_ua, raw, ret);
		return ret;
	}

	LOG_DBG("%s written: current=%d uA raw=0x%03x", channel->name,
		current_ua, raw);
	return 0;
}

int current_loop_output_init(void)
{
	int first_error = 0;
	int ret;

	if (!gpio_is_ready_dt(&dac_power)) {
		LOG_WRN("DAC power GPIO controller is not ready");
		first_error = -ENODEV;
	} else {
		ret = gpio_pin_configure_dt(&dac_power, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("DAC power GPIO configure failed: %d", ret);
			first_error = ret;
		}
	}

	ret = ensure_dac_power_for_write();
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(loop_channels); i++) {
		ret = setup_channel(&loop_channels[i]);
		if (ret < 0 && first_error == 0) {
			first_error = ret;
		}
	}

	apply_dac_idle_policy();

	LOG_INF("pH current-loop mapping ready: DAC0=pH, DAC1=temperature");
	return first_error;
}

int current_loop_output_update(const struct argisense_runtime_config *config,
			       struct argisense_ph_measurement_sample *sample)
{
	int32_t ph_current_ua;
	int32_t temp_current_ua;
	int ret;
	int first_error = 0;

	if (config == NULL || sample == NULL) {
		return -EINVAL;
	}

	ph_current_ua = scale_current_ua(sample->ph_x1000,
					 config->ph_range_low_x1000,
					 config->ph_range_high_x1000,
					 sample->ph_valid, config);
	if (sample->ph_valid) {
		ph_current_ua = apply_trim_ua(ph_current_ua,
					      config->dac0_4ma_trim_ua,
					      config->dac0_20ma_trim_ua,
					      config,
					      loop_channels[0].max_current_ua);
	}

	temp_current_ua = scale_current_ua(
		sample->temperature_centi_c,
		config->temperature_range_low_centi_c,
		config->temperature_range_high_centi_c, sample->temperature_valid,
		config);
	if (sample->temperature_valid) {
		temp_current_ua =
			apply_trim_ua(temp_current_ua, config->dac1_4ma_trim_ua,
				      config->dac1_20ma_trim_ua, config,
				      loop_channels[1].max_current_ua);
	}

	sample->dac0_current_ua = clamp_i32(ph_current_ua, 0,
					    (int32_t)loop_channels[0]
						    .max_current_ua);
	sample->dac1_current_ua = clamp_i32(temp_current_ua, 0,
					    (int32_t)loop_channels[1]
						    .max_current_ua);

	ret = ensure_dac_power_for_write();
	if (ret < 0) {
		return ret;
	}

	ret = write_channel_current(&loop_channels[0], sample->dac0_current_ua);
	if (ret < 0) {
		first_error = ret;
	}

	ret = write_channel_current(&loop_channels[1], sample->dac1_current_ua);
	if (ret < 0 && first_error == 0) {
		first_error = ret;
	}

	apply_dac_idle_policy();

	if (sample->temperature_valid) {
		LOG_INF("DAC1 temperature output: %d.%02d C -> %d uA",
			sample->temperature_centi_c / 100,
			abs(sample->temperature_centi_c % 100),
			sample->dac1_current_ua);
	} else {
		LOG_INF("DAC1 temperature output fault current: %d uA",
			sample->dac1_current_ua);
	}

	return first_error;
}
