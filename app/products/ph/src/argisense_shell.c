/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <argisense/drivers/adc/ads124s08.h>

#include "argisense_dfu.h"
#include "argisense_registers.h"
#include "argisense_settings.h"
#include "current_loop_output.h"
#include "ph_measurement.h"

#define USER_NODE DT_PATH(zephyr_user)
#define PH_ADC_NODE DT_ALIAS(ph_adc)
#define PH_BIAS_DAC_NODE DT_ALIAS(ph_bias_dac)
#define DAC0_NODE DT_ALIAS(current_loop_dac0)
#define DAC1_NODE DT_ALIAS(current_loop_dac1)
#define EEPROM_NODE DT_ALIAS(eeprom0)
#define RS485_NODE DT_ALIAS(rs485_uart)
#define PH_BIAS_DAC_FULL_SCALE_MV \
	(DT_PROP(PH_BIAS_DAC_NODE, reference_millivolt) * \
	 DT_PROP(PH_BIAS_DAC_NODE, gain))

enum argisense_shell_setting_type {
	ARGISENSE_SHELL_SETTING_U32,
	ARGISENSE_SHELL_SETTING_I32,
	ARGISENSE_SHELL_SETTING_U8,
};

struct argisense_shell_setting {
	const char *name;
	const char *alias;
	size_t offset;
	enum argisense_shell_setting_type type;
	int64_t min;
	int64_t max;
	const char *unit;
	bool reboot_required;
};

#define ARGISENSE_SETTING_DESC(_name, _alias, _field, _type, _min, _max, _unit, _reboot) \
	{ \
		.name = _name, \
		.alias = _alias, \
		.offset = offsetof(struct argisense_runtime_config, _field), \
		.type = _type, \
		.min = _min, \
		.max = _max, \
		.unit = _unit, \
		.reboot_required = _reboot, \
	}

static const struct i2c_dt_spec ph_bias_dac_i2c =
	I2C_DT_SPEC_GET(PH_BIAS_DAC_NODE);
static const struct i2c_dt_spec dac0_i2c = I2C_DT_SPEC_GET(DAC0_NODE);
static const struct i2c_dt_spec dac1_i2c = I2C_DT_SPEC_GET(DAC1_NODE);
static const struct i2c_dt_spec eeprom_i2c = I2C_DT_SPEC_GET(EEPROM_NODE);
static const struct device *const ph_adc = DEVICE_DT_GET(PH_ADC_NODE);
static const struct device *const ph_bias_dac = DEVICE_DT_GET(PH_BIAS_DAC_NODE);
static const struct device *const ph_adc_spi = DEVICE_DT_GET(DT_BUS(PH_ADC_NODE));
static const struct device *const ph_bias_i2c =
	DEVICE_DT_GET(DT_BUS(PH_BIAS_DAC_NODE));
static const struct device *const dac0 = DEVICE_DT_GET(DAC0_NODE);
static const struct device *const dac1 = DEVICE_DT_GET(DAC1_NODE);
static const struct device *const eeprom = DEVICE_DT_GET(EEPROM_NODE);
static const struct device *const rs485_uart = DEVICE_DT_GET(RS485_NODE);

static const struct argisense_shell_setting shell_settings[] = {
	ARGISENSE_SETTING_DESC("measurement_period_s", "period",
			       measurement_period_seconds,
			       ARGISENSE_SHELL_SETTING_U32,
			       ARGISENSE_MEASUREMENT_PERIOD_SECONDS_MIN,
			       UINT32_MAX, "s", false),
	ARGISENSE_SETTING_DESC("measurement_window_ms", "window",
			       measurement_window_ms,
			       ARGISENSE_SHELL_SETTING_U32,
			       ARGISENSE_MEASUREMENT_WINDOW_MS_MIN,
			       ARGISENSE_MEASUREMENT_WINDOW_MS_MAX, "ms", false),
	ARGISENSE_SETTING_DESC("ph_range_low_x1000", "ph_low",
			       ph_range_low_x1000,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "pH_x1000", false),
	ARGISENSE_SETTING_DESC("ph_range_high_x1000", "ph_high",
			       ph_range_high_x1000,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "pH_x1000", false),
	ARGISENSE_SETTING_DESC("temperature_range_low_centi_c", "temp_low",
			       temperature_range_low_centi_c,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "centi-C", false),
	ARGISENSE_SETTING_DESC("temperature_range_high_centi_c", "temp_high",
			       temperature_range_high_centi_c,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "centi-C", false),
	ARGISENSE_SETTING_DESC("ph_slope_x1000", "ph_slope",
			       ph_slope_x1000,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "x1000", false),
	ARGISENSE_SETTING_DESC("ph_offset_x1000", "ph_offset",
			       ph_offset_x1000,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "pH_x1000", false),
	ARGISENSE_SETTING_DESC("ph7_raw_uv", "ph7_raw",
			       ph7_raw_uv,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "uV", false),
	ARGISENSE_SETTING_DESC("ph_slope_uv_per_ph", "ph_slope_uv",
			       ph_slope_uv_per_ph,
			       ARGISENSE_SHELL_SETTING_I32,
			       -200000, 200000, "uV/pH", false),
	ARGISENSE_SETTING_DESC("ph_temp_reference_centi_c", "ph_temp_ref",
			       ph_temp_reference_centi_c,
			       ARGISENSE_SHELL_SETTING_I32,
			       -4000, 13500, "centi-C", false),
	ARGISENSE_SETTING_DESC("ph_temp_coeff_ppm_per_c", "ph_temp_coeff",
			       ph_temp_coeff_ppm_per_c,
			       ARGISENSE_SHELL_SETTING_I32,
			       -20000, 20000, "ppm/C", false),
	ARGISENSE_SETTING_DESC("dac_min_ua", "dac_min",
			       dac_min_current_ua,
			       ARGISENSE_SHELL_SETTING_I32,
			       0, 25000, "uA", false),
	ARGISENSE_SETTING_DESC("dac_max_ua", "dac_max",
			       dac_max_current_ua,
			       ARGISENSE_SHELL_SETTING_I32,
			       0, 25000, "uA", false),
	ARGISENSE_SETTING_DESC("dac_fault_ua", "dac_fault",
			       dac_fault_current_ua,
			       ARGISENSE_SHELL_SETTING_I32,
			       0, 25000, "uA", false),
	ARGISENSE_SETTING_DESC("dac0_4ma_trim_ua", "dac0_4ma",
			       dac0_4ma_trim_ua,
			       ARGISENSE_SHELL_SETTING_I32,
			       -2000, 2000, "uA", false),
	ARGISENSE_SETTING_DESC("dac0_20ma_trim_ua", "dac0_20ma",
			       dac0_20ma_trim_ua,
			       ARGISENSE_SHELL_SETTING_I32,
			       -2000, 2000, "uA", false),
	ARGISENSE_SETTING_DESC("dac1_4ma_trim_ua", "dac1_4ma",
			       dac1_4ma_trim_ua,
			       ARGISENSE_SHELL_SETTING_I32,
			       -2000, 2000, "uA", false),
	ARGISENSE_SETTING_DESC("dac1_20ma_trim_ua", "dac1_20ma",
			       dac1_20ma_trim_ua,
			       ARGISENSE_SHELL_SETTING_I32,
			       -2000, 2000, "uA", false),
	ARGISENSE_SETTING_DESC("rs485_baudrate", "baud",
			       rs485_baudrate,
			       ARGISENSE_SHELL_SETTING_U32,
			       1, UINT32_MAX, "baud", true),
	ARGISENSE_SETTING_DESC("rs485_parity", "parity",
			       rs485_parity,
			       ARGISENSE_SHELL_SETTING_U8,
			       ARGISENSE_RS485_PARITY_NONE,
			       ARGISENSE_RS485_PARITY_EVEN, "code", true),
	ARGISENSE_SETTING_DESC("rs485_stop_bits", "stop",
			       rs485_stop_bits,
			       ARGISENSE_SHELL_SETTING_U8,
			       ARGISENSE_RS485_STOP_BITS_1,
			       ARGISENSE_RS485_STOP_BITS_2, "bits", true),
	ARGISENSE_SETTING_DESC("rs485_data_bits", "databits",
			       rs485_data_bits,
			       ARGISENSE_SHELL_SETTING_U8,
			       ARGISENSE_RS485_DATA_BITS_8,
			       ARGISENSE_RS485_DATA_BITS_8, "bits", true),
	ARGISENSE_SETTING_DESC("ph_calibration_mode", "ph_cal_mode",
			       ph_calibration_mode,
			       ARGISENSE_SHELL_SETTING_U8,
			       ARGISENSE_PH_CAL_MODE_DISABLED,
			       ARGISENSE_PH_CAL_MODE_REFERENCE_ELECTRODE,
			       "code", false),
	ARGISENSE_SETTING_DESC("ph_calibration_valid", "ph_cal_valid",
			       ph_calibration_valid,
			       ARGISENSE_SHELL_SETTING_U8,
			       0, 1, "", false),
	ARGISENSE_SETTING_DESC("modbus_address", "address",
			       modbus_address,
			       ARGISENSE_SHELL_SETTING_U8,
			       ARGISENSE_MODBUS_ADDRESS_MIN,
			       ARGISENSE_MODBUS_ADDRESS_MAX, "", true),
	ARGISENSE_SETTING_DESC("rs485_termination", "termination",
			       rs485_termination_enabled,
			       ARGISENSE_SHELL_SETTING_U8,
			       0, 1, "", false),
};

static const char *ready_str(bool ready)
{
	return ready ? "ready" : "not-ready";
}

static int parse_u16_arg(const char *arg, uint16_t *value)
{
	char *endptr;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(arg, &endptr, 0);
	if (errno != 0 || endptr == arg || *endptr != '\0' ||
	    parsed > UINT16_MAX) {
		return -EINVAL;
	}

	*value = (uint16_t)parsed;
	return 0;
}

static int parse_i64_arg(const char *arg, int64_t *value)
{
	char *endptr;
	long long parsed;

	errno = 0;
	parsed = strtoll(arg, &endptr, 0);
	if (errno != 0 || endptr == arg || *endptr != '\0') {
		return -EINVAL;
	}

	*value = (int64_t)parsed;
	return 0;
}

static int i2c_address_probe(const struct i2c_dt_spec *spec)
{
	struct i2c_msg msg = {
		.buf = NULL,
		.len = 0U,
		.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
	};

	if (!i2c_is_ready_dt(spec)) {
		return -ENODEV;
	}

	return i2c_transfer_dt(spec, &msg, 1U);
}

static void print_device_line(const struct shell *shell, const char *label,
			      const char *node_name, const char *driver_state,
			      int probe_ret)
{
	if (probe_ret == 0) {
		shell_print(shell, "%-20s %-22s %-10s present", label,
			    node_name, driver_state);
	} else if (probe_ret == -ENOTSUP) {
		shell_print(shell, "%-20s %-22s %-10s -", label, node_name,
			    driver_state);
	} else {
		shell_print(shell, "%-20s %-22s %-10s not-present(%d)",
			    label, node_name, driver_state, probe_ret);
	}
}

static const struct argisense_shell_setting *find_shell_setting(const char *name)
{
	for (size_t i = 0U; i < ARRAY_SIZE(shell_settings); i++) {
		if (strcmp(name, shell_settings[i].name) == 0 ||
		    (shell_settings[i].alias != NULL &&
		     strcmp(name, shell_settings[i].alias) == 0)) {
			return &shell_settings[i];
		}
	}

	return NULL;
}

static int64_t shell_setting_get_value(
	const struct argisense_runtime_config *config,
	const struct argisense_shell_setting *setting)
{
	const uint8_t *base = (const uint8_t *)config;
	uint32_t u32_value;
	int32_t i32_value;
	uint8_t u8_value;

	switch (setting->type) {
	case ARGISENSE_SHELL_SETTING_U32:
		memcpy(&u32_value, base + setting->offset, sizeof(u32_value));
		return u32_value;
	case ARGISENSE_SHELL_SETTING_I32:
		memcpy(&i32_value, base + setting->offset, sizeof(i32_value));
		return i32_value;
	case ARGISENSE_SHELL_SETTING_U8:
		memcpy(&u8_value, base + setting->offset, sizeof(u8_value));
		return u8_value;
	default:
		return 0;
	}
}

static void shell_setting_set_value(
	struct argisense_runtime_config *config,
	const struct argisense_shell_setting *setting,
	int64_t value)
{
	uint8_t *base = (uint8_t *)config;
	uint32_t u32_value;
	int32_t i32_value;
	uint8_t u8_value;

	switch (setting->type) {
	case ARGISENSE_SHELL_SETTING_U32:
		u32_value = (uint32_t)value;
		memcpy(base + setting->offset, &u32_value, sizeof(u32_value));
		break;
	case ARGISENSE_SHELL_SETTING_I32:
		i32_value = (int32_t)value;
		memcpy(base + setting->offset, &i32_value, sizeof(i32_value));
		break;
	case ARGISENSE_SHELL_SETTING_U8:
		u8_value = (uint8_t)value;
		memcpy(base + setting->offset, &u8_value, sizeof(u8_value));
		break;
	default:
		break;
	}
}

static void print_shell_setting(const struct shell *shell,
				const struct argisense_shell_setting *setting,
				const struct argisense_runtime_config *config)
{
	const int64_t value = shell_setting_get_value(config, setting);

	shell_print(shell, "%-34s %12lld %-8s alias=%-14s range=%lld..%lld%s",
		    setting->name, value, setting->unit,
		    setting->alias != NULL ? setting->alias : "-",
		    setting->min, setting->max,
		    setting->reboot_required ? " reboot-required" : "");
}

static int cmd_argisense_drivers(const struct shell *shell, size_t argc,
				 char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "ArgiSense pH driver readiness and bus/address probe");
	shell_print(shell, "---------------------------------------------------");
	shell_print(shell, "%-20s %-22s %-10s %s", "device", "node/bus",
		    "driver", "probe");
	print_device_line(shell, "pH ADC SPI bus", ph_adc_spi->name,
			  ready_str(device_is_ready(ph_adc_spi)), -ENOTSUP);
	print_device_line(shell, "pH ADC ADS124S08", ph_adc->name,
			  ready_str(device_is_ready(ph_adc)), -ENOTSUP);
	print_device_line(shell, "pH ADC binding", "ph-adc@0", "dts",
			  -ENOTSUP);
	print_device_line(shell, "pH bias I2C bus", ph_bias_i2c->name,
			  ready_str(device_is_ready(ph_bias_i2c)), -ENOTSUP);
	print_device_line(shell, "pH bias AD5675", ph_bias_dac->name,
			  ready_str(device_is_ready(ph_bias_dac)),
			  i2c_address_probe(&ph_bias_dac_i2c));
	print_device_line(shell, "dac0 pH", dac0->name,
			  ready_str(device_is_ready(dac0)),
			  i2c_address_probe(&dac0_i2c));
	print_device_line(shell, "dac1 temperature", dac1->name,
			  ready_str(device_is_ready(dac1)),
			  i2c_address_probe(&dac1_i2c));
	print_device_line(shell, "external EEPROM", eeprom->name,
			  ready_str(device_is_ready(eeprom)),
			  i2c_address_probe(&eeprom_i2c));
	print_device_line(shell, "RS485 UART", rs485_uart->name,
			  ready_str(device_is_ready(rs485_uart)), -ENOTSUP);

	shell_print(shell, "");
	shell_print(shell, "Mappings:");
	shell_print(shell, "  pH ADC: %s on %s CS%u max-frequency=%u",
		    DT_PROP_OR(USER_NODE, ph_adc_part_number, "ADS124S08"),
		    ph_adc_spi->name, (unsigned int)DT_REG_ADDR(PH_ADC_NODE),
		    DT_PROP(PH_ADC_NODE, spi_max_frequency));
	shell_print(shell, "  pH bias DAC: %s on %s addr=0x%02x",
		    DT_PROP_OR(USER_NODE, ph_bias_dac_part_number, "AD5675"),
		    ph_bias_i2c->name, ph_bias_dac_i2c.addr);
	shell_print(shell, "  DAC0 pH: %s addr=0x%02x max-current=%u uA",
		    dac0_i2c.bus->name, dac0_i2c.addr,
		    DT_PROP(DAC0_NODE, max_current_microamp));
	shell_print(shell, "  DAC1 temperature: %s addr=0x%02x max-current=%u uA",
		    dac1_i2c.bus->name, dac1_i2c.addr,
		    DT_PROP(DAC1_NODE, max_current_microamp));
	shell_print(shell, "  EEPROM: %s addr=0x%02x size=%zu bytes page=%u",
		    eeprom_i2c.bus->name, eeprom_i2c.addr,
		    eeprom_get_size(eeprom), DT_PROP(EEPROM_NODE, pagesize));
	shell_print(shell, "");
	shell_print(shell,
		    "note: ADS124S08/AD5675 drivers are loaded; pH value remains invalid until product calibration defines the final ISFET/REFET equation.");

	return 0;
}

static void dump_rs485_range(const struct shell *shell, uint16_t start,
			     uint16_t count)
{
	for (uint16_t offset = 0U; offset < count; offset++) {
		uint16_t addr = start + offset;
		uint16_t reg;
		const char *name = argisense_register_holding_name(addr);
		int ret = argisense_register_read_holding(addr, &reg);

		if (ret < 0) {
			shell_print(shell, "%04u  ----  %-26s ret=%d", addr,
				    name != NULL ? name : "reserved", ret);
			continue;
		}

		shell_print(shell, "%04u  0x%04x %-26s %u", addr, reg,
			    name != NULL ? name : "", reg);
	}
}

static int cmd_argisense_rs485(const struct shell *shell, size_t argc,
			       char **argv)
{
	uint16_t start = 0U;
	uint16_t count = 37U;
	int ret;

	if (argc > 3U) {
		shell_error(shell, "usage: argisense rs485 [start] [count]");
		return -EINVAL;
	}

	if (argc >= 2U) {
		ret = parse_u16_arg(argv[1], &start);
		if (ret < 0) {
			shell_error(shell, "invalid start register: %s", argv[1]);
			return ret;
		}
	}

	if (argc >= 3U) {
		ret = parse_u16_arg(argv[2], &count);
		if (ret < 0 || count == 0U) {
			shell_error(shell, "invalid count: %s", argv[2]);
			return -EINVAL;
		}
	}

	shell_print(shell, "pH RS485 holding registers");
	shell_print(shell, "addr  hex    name                       dec");
	dump_rs485_range(shell, start, count);

	if (argc == 1U) {
		shell_print(shell, "");
		dump_rs485_range(shell, 37U, 31U);
		shell_print(shell, "");
		dump_rs485_range(shell, 70U, 7U);
		shell_print(shell, "");
		dump_rs485_range(shell, 80U, 27U);
#if defined(CONFIG_ARGISENSE_RS485_DFU)
		shell_print(shell, "");
		dump_rs485_range(shell, ARGISENSE_DFU_REG_CONTROL, 36U);
#endif
	}

	return 0;
}

static int cmd_argisense_sensors(const struct shell *shell, size_t argc,
				 char **argv)
{
	uint16_t ph_hi;
	uint16_t ph_lo;
	uint16_t temp_hi;
	uint16_t temp_lo;
	uint16_t raw_hi;
	uint16_t raw_lo;
	uint16_t dac0_ua;
	uint16_t dac1_ua;
	uint16_t op_status;
	uint16_t status;
	uint16_t seq_lo;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	argisense_register_read_holding(2U, &status);
	argisense_register_read_holding(10U, &ph_hi);
	argisense_register_read_holding(11U, &ph_lo);
	argisense_register_read_holding(12U, &temp_hi);
	argisense_register_read_holding(13U, &temp_lo);
	argisense_register_read_holding(14U, &dac0_ua);
	argisense_register_read_holding(15U, &dac1_ua);
	argisense_register_read_holding(17U, &seq_lo);
	argisense_register_read_holding(80U, &raw_hi);
	argisense_register_read_holding(81U, &raw_lo);
	argisense_register_read_holding(106U, &op_status);

	shell_print(shell, "pH latest register snapshot");
	shell_print(shell, "status=0x%04x sequence-lo=%u", status, seq_lo);
	shell_print(shell, "pH_x1000=%d",
		    (int32_t)((((uint32_t)ph_hi) << 16) | ph_lo));
	shell_print(shell, "temperature_centi_c=%d",
		    (int32_t)((((uint32_t)temp_hi) << 16) | temp_lo));
	shell_print(shell, "dac0_ph_current_ua=%u dac1_temp_current_ua=%u",
		    dac0_ua, dac1_ua);
	shell_print(shell, "ph_raw_uv=%d operating_status=0x%04x",
		    (int32_t)((((uint32_t)raw_hi) << 16) | raw_lo),
		    op_status);
	shell_print(shell,
		    "note: pH is valid only when calibration is enabled and the ISFET/REFET operating point is in range.");

	return 0;
}

static int cmd_argisense_ph_adc(const struct shell *shell, size_t argc,
				char **argv)
{
	uint16_t positive;
	uint16_t negative = ADS124S08_AINCOM;
	uint16_t idac_pin = ADS124S08_AINCOM;
	bool differential = false;
	bool enable_idac = false;
	int32_t raw;
	int32_t microvolts;
	int ret;

	if (argc < 2U || argc > 4U) {
		shell_error(shell,
			    "usage: argisense ph adc <positive> [negative] [idac-pin]");
		return -EINVAL;
	}

	ret = parse_u16_arg(argv[1], &positive);
	if (ret < 0 || positive >= ADS124S08_CHANNEL_COUNT) {
		shell_error(shell, "invalid ADS124S08 positive channel: %s",
			    argv[1]);
		return -EINVAL;
	}

	if (argc >= 3U) {
		ret = parse_u16_arg(argv[2], &negative);
		if (ret < 0 || negative > ADS124S08_AINCOM) {
			shell_error(shell,
				    "invalid ADS124S08 negative channel: %s",
				    argv[2]);
			return -EINVAL;
		}
		differential = true;
	}

	if (argc >= 4U) {
		ret = parse_u16_arg(argv[3], &idac_pin);
		if (ret < 0 || idac_pin > ADS124S08_AINCOM) {
			shell_error(shell,
				    "invalid ADS124S08 IDAC channel: %s",
				    argv[3]);
			return -EINVAL;
		}
		enable_idac = true;
	}

	ret = ph_measurement_read_adc_uv((uint8_t)positive,
					 (uint8_t)negative, differential,
					 enable_idac, (uint8_t)idac_pin,
					 &raw, &microvolts);
	if (ret < 0) {
		shell_error(shell, "ADS124S08 read failed: %d", ret);
		return ret;
	}

	if (differential) {
		shell_print(shell,
			    "ADS124S08 diff ch%u-ch%u%s raw=%d microvolts=%d",
			    positive, negative,
			    enable_idac ? " idac=on" : "", raw, microvolts);
	} else {
		shell_print(shell,
			    "ADS124S08 single ch%u raw=%d microvolts=%d",
			    positive, raw, microvolts);
	}
	return 0;
}

static int cmd_argisense_ph_dac(const struct shell *shell, size_t argc,
				char **argv)
{
	uint16_t channel;
	uint16_t millivolt;
	int ret;

	if (argc != 3U) {
		shell_error(shell,
			    "usage: argisense ph dac <channel 0..7> <millivolt>");
		return -EINVAL;
	}

	ret = parse_u16_arg(argv[1], &channel);
	if (ret < 0 || channel > 7U) {
		shell_error(shell, "invalid AD5675 channel: %s", argv[1]);
		return -EINVAL;
	}

	ret = parse_u16_arg(argv[2], &millivolt);
	if (ret < 0 || millivolt > PH_BIAS_DAC_FULL_SCALE_MV) {
		shell_error(shell, "invalid AD5675 millivolt: %s", argv[2]);
		return -EINVAL;
	}

	ret = ph_measurement_set_bias_mv((uint8_t)channel, millivolt);
	if (ret < 0) {
		shell_error(shell, "AD5675 write failed: %d", ret);
		return ret;
	}

	shell_print(shell, "AD5675 channel %u set to %u mV", channel,
		    millivolt);
	return 0;
}

static int cmd_argisense_ph_zero(const struct shell *shell, size_t argc,
				 char **argv)
{
	uint16_t closed;
	int ret;

	if (argc != 2U) {
		shell_error(shell, "usage: argisense ph zero <0=open|1=closed>");
		return -EINVAL;
	}

	ret = parse_u16_arg(argv[1], &closed);
	if (ret < 0 || closed > 1U) {
		shell_error(shell, "invalid electrode-zero state: %s", argv[1]);
		return -EINVAL;
	}

	ret = ph_measurement_set_electrode_zero(closed != 0U);
	if (ret < 0) {
		shell_error(shell, "electrode-zero GPIO set failed: %d", ret);
		return ret;
	}

	shell_print(shell, "electrode-zero switch %s",
		    closed != 0U ? "closed" : "open");
	return 0;
}

static int cmd_argisense_ph_sample(const struct shell *shell, size_t argc,
				   char **argv)
{
	struct argisense_ph_measurement_sample sample;
	struct argisense_runtime_config config;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	argisense_settings_get_copy(&config);
	ph_measurement_sample(&sample);
	ret = current_loop_output_update(&config, &sample);
	if (ret < 0) {
		shell_print(shell, "current-loop output update failed: %d", ret);
	}
	argisense_register_update_sample(&sample);

	shell_print(shell,
		    "status: adc=0x%04x bias=0x%04x operating=0x%04x ph_valid=%u temp_valid=%u",
		    sample.ph_adc_status, sample.bias_dac_status,
		    sample.ph_operating_status, sample.ph_valid,
		    sample.temperature_valid);
	shell_print(shell,
		    "ph_x1000=%d ph_raw_uv=%d vgs_isfet_uv=%d vgs_refet_uv=%d",
		    sample.ph_x1000, sample.ph_raw_uv,
		    sample.vgs_isfet_uv, sample.vgs_refet_uv);
	shell_print(shell,
		    "vs_isfet_uv=%d vs_refet_uv=%d gate_uv=%d temp_centi_c=%d pt1000_uv=%d",
		    sample.vs_isfet_uv, sample.vs_refet_uv, sample.gate_uv,
		    sample.temperature_centi_c, sample.pt1000_uv);
	shell_print(shell, "current_loop: dac0_ph=%d uA dac1_temp=%d uA",
		    sample.dac0_current_ua, sample.dac1_current_ua);
	shell_print(shell,
		    "drain_uv: isfet=%d refet=%d bulk_uv: isfet=%d refet=%d subs_uv: isfet=%d refet=%d",
		    sample.isfet_drain_uv, sample.refet_drain_uv,
		    sample.isfet_bulk_uv, sample.refet_bulk_uv,
		    sample.isfet_subs_uv, sample.refet_subs_uv);
	shell_print(shell, "errors: ph=%d temp=%d",
		    sample.ph_last_error, sample.temperature_last_error);

	return 0;
}

static int cmd_argisense_settings(const struct shell *shell, size_t argc,
				  char **argv)
{
	struct argisense_runtime_config config;
	const struct argisense_shell_setting *setting;
	struct argisense_runtime_config updated;
	int64_t value;
	int ret;

	argisense_settings_get_copy(&config);

	if (argc == 1U || (argc == 2U && strcmp(argv[1], "list") == 0)) {
		shell_print(shell, "schema=%u size=%u", config.schema_version,
			    config.struct_size);
		shell_print(shell, "setting                            value        unit     alias          range");
		shell_print(shell, "--------------------------------------------------------------------------------");
		for (size_t i = 0U; i < ARRAY_SIZE(shell_settings); i++) {
			print_shell_setting(shell, &shell_settings[i], &config);
		}
		shell_print(shell, "");
		shell_print(shell,
			    "parity codes: 0=none, 1=odd, 2=even; stop bits: 1 or 2; data bits: 8 only");
		shell_print(shell,
			    "pH calibration modes: 0=disabled, 1=ISFET-REFET differential, 2=reference-electrode");
		return 0;
	}

	if (argc == 2U && strcmp(argv[1], "reset") == 0) {
		ret = argisense_settings_reset_defaults();
		if (ret < 0) {
			shell_error(shell, "settings reset failed: %d", ret);
			return ret;
		}

		shell_print(shell, "pH settings reset to defaults and saved to NVS");
		shell_print(shell,
			    "note: reboot before relying on changed RS485 transport settings");
		return 0;
	}

	if ((argc == 2U && strcmp(argv[1], "get") != 0) ||
	    (argc == 3U && strcmp(argv[1], "get") == 0)) {
		const char *name = argc == 2U ? argv[1] : argv[2];

		setting = find_shell_setting(name);
		if (setting == NULL) {
			shell_error(shell, "unknown setting: %s", name);
			return -ENOENT;
		}

		print_shell_setting(shell, setting, &config);
		return 0;
	}

	if (argc == 4U && strcmp(argv[1], "set") == 0) {
		setting = find_shell_setting(argv[2]);
		if (setting == NULL) {
			shell_error(shell, "unknown setting: %s", argv[2]);
			return -ENOENT;
		}

		ret = parse_i64_arg(argv[3], &value);
		if (ret < 0) {
			shell_error(shell, "invalid value: %s", argv[3]);
			return ret;
		}

		if (value < setting->min || value > setting->max) {
			shell_error(shell, "%s out of range: %lld not in %lld..%lld",
				    setting->name, value, setting->min,
				    setting->max);
			return -ERANGE;
		}

		updated = config;
		shell_setting_set_value(&updated, setting, value);

		ret = argisense_settings_save(&updated);
		if (ret < 0) {
			shell_error(shell, "settings save rejected %s=%lld: %d",
				    setting->name, value, ret);
			return ret;
		}

		shell_print(shell, "saved %s=%lld %s", setting->name, value,
			    setting->unit);
		if (setting->reboot_required) {
			shell_print(shell,
				    "note: reboot is required before this RS485 transport setting takes effect");
		}
		return 0;
	}

	shell_error(shell, "usage: argisense settings [list]");
	shell_error(shell, "       argisense settings [get] <name|alias>");
	shell_error(shell, "       argisense settings set <name|alias> <value>");
	shell_error(shell, "       argisense settings reset");
	return -EINVAL;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	argisense_ph_cmds,
	SHELL_CMD(adc, NULL,
		  "Read ADS124S08 channel: adc <pos> [neg] [idac-pin].",
		  cmd_argisense_ph_adc),
	SHELL_CMD(dac, NULL, "Set AD5675 bias DAC: dac <channel> <mV>.",
		  cmd_argisense_ph_dac),
	SHELL_CMD(sample, NULL, "Force one pH measurement sample.",
		  cmd_argisense_ph_sample),
	SHELL_CMD(zero, NULL, "Set electrode-zero switch: zero <0|1>.",
		  cmd_argisense_ph_zero),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	argisense_cmds,
	SHELL_CMD(drivers, NULL, "Show pH board driver and bus readiness.",
		  cmd_argisense_drivers),
	SHELL_CMD(ph, &argisense_ph_cmds,
		  "pH ADC/DAC/electrode bring-up commands.", NULL),
	SHELL_CMD(sensors, NULL, "Print latest pH register snapshot.",
		  cmd_argisense_sensors),
	SHELL_CMD(rs485, NULL, "Dump pH RS485 holding registers.",
		  cmd_argisense_rs485),
	SHELL_CMD(settings, NULL, "Read or write pH runtime settings.",
		  cmd_argisense_settings),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(argisense, &argisense_cmds,
		   "ArgiSense pH diagnostic commands.", NULL);
