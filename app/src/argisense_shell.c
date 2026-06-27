/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <argisense/drivers/sensor/dynament_platinum.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "argisense_dfu.h"
#include "argisense_rs485.h"
#include "argisense_registers.h"
#include "argisense_settings.h"
#include "argisense_power.h"

#define DAC0_NODE DT_ALIAS(current_loop_dac0)
#define DAC1_NODE DT_ALIAS(current_loop_dac1)
#define EEPROM_NODE DT_ALIAS(eeprom0)
#define PRESSURE_NODE DT_ALIAS(pressure_sensor)
#define METHANE_NODE DT_ALIAS(methane_sensor)
#define HUMIDITY_NODE DT_ALIAS(humidity_sensor)
#define RS485_NODE DT_ALIAS(rs485_uart)
#define GP8302_RAW_MAX 0x0FFFU

static const struct device *const methane_sensor = DEVICE_DT_GET(METHANE_NODE);
static const struct device *const methane_uart = DEVICE_DT_GET(DT_BUS(METHANE_NODE));
static const struct device *const pressure_sensor = DEVICE_DT_GET(PRESSURE_NODE);
static const struct device *const pressure_spi = DEVICE_DT_GET(DT_BUS(PRESSURE_NODE));
static const struct device *const humidity_sensor = DEVICE_DT_GET(HUMIDITY_NODE);
static const struct device *const humidity_i2c = DEVICE_DT_GET(DT_BUS(HUMIDITY_NODE));
static const struct device *const dac0 = DEVICE_DT_GET(DAC0_NODE);
static const struct device *const dac1 = DEVICE_DT_GET(DAC1_NODE);
static const struct i2c_dt_spec dac0_i2c_spec = I2C_DT_SPEC_GET(DAC0_NODE);
static const struct i2c_dt_spec dac1_i2c_spec = I2C_DT_SPEC_GET(DAC1_NODE);
static const struct i2c_dt_spec humidity_i2c_spec = I2C_DT_SPEC_GET(HUMIDITY_NODE);
static const struct device *const dac0_i2c = DEVICE_DT_GET(DT_BUS(DAC0_NODE));
static const struct device *const dac1_i2c = DEVICE_DT_GET(DT_BUS(DAC1_NODE));
static const struct device *const external_eeprom = DEVICE_DT_GET(EEPROM_NODE);
static const struct i2c_dt_spec external_eeprom_i2c = I2C_DT_SPEC_GET(EEPROM_NODE);
static const struct device *const rs485_uart = DEVICE_DT_GET(RS485_NODE);

struct argisense_shell_dac {
	const struct device *dev;
	struct i2c_dt_spec i2c;
	const char *label;
	uint32_t max_current_ua;
};

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

static const struct argisense_shell_dac shell_dacs[] = {
	{
		.dev = DEVICE_DT_GET(DAC0_NODE),
		.i2c = I2C_DT_SPEC_GET(DAC0_NODE),
		.label = "dac0 methane",
		.max_current_ua = DT_PROP(DAC0_NODE, max_current_microamp),
	},
	{
		.dev = DEVICE_DT_GET(DAC1_NODE),
		.i2c = I2C_DT_SPEC_GET(DAC1_NODE),
		.label = "dac1 pressure",
		.max_current_ua = DT_PROP(DAC1_NODE, max_current_microamp),
	},
};

static const struct dac_channel_cfg gp8302_shell_channel_cfg = {
	.channel_id = 0U,
	.resolution = 12U,
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
	ARGISENSE_SETTING_DESC("methane_warmup_s", "methane_warmup",
			       methane_warmup_seconds,
			       ARGISENSE_SHELL_SETTING_U32,
			       0, 600, "s", false),
	ARGISENSE_SETTING_DESC("methane_read_ms", "methane_read",
			       methane_read_period_ms,
			       ARGISENSE_SHELL_SETTING_U32,
			       100, 60000, "ms", false),
	ARGISENSE_SETTING_DESC("humidity_read_s", "humidity_read",
			       humidity_read_period_seconds,
			       ARGISENSE_SHELL_SETTING_U32,
			       1, 86400, "s", false),
	ARGISENSE_SETTING_DESC("methane_range_low_ppm", "methane_low",
			       methane_dac_range_low_ppm,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "ppm", false),
	ARGISENSE_SETTING_DESC("methane_range_high_ppm", "methane_high",
			       methane_dac_range_high_ppm,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "ppm", false),
	ARGISENSE_SETTING_DESC("pressure_range_low_pa", "pressure_low",
			       pressure_dac_range_low_pa,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "Pa", false),
	ARGISENSE_SETTING_DESC("pressure_range_high_pa", "pressure_high",
			       pressure_dac_range_high_pa,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "Pa", false),
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
	ARGISENSE_SETTING_DESC("methane_zero_offset_ppm_x100", "methane_zero",
			       methane_zero_offset_ppm_x100,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "ppm_x100", false),
	ARGISENSE_SETTING_DESC("pressure_offset_pa", "pressure_offset",
			       pressure_offset_pa,
			       ARGISENSE_SHELL_SETTING_I32,
			       INT32_MIN, INT32_MAX, "Pa", false),
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

static void print_device_probe_line(const struct shell *shell,
				    const char *label,
				    const struct device *dev,
				    int probe_ret)
{
	if (probe_ret == 0) {
		shell_print(shell, "%-18s %-18s %-10s present",
			    label, dev->name, ready_str(device_is_ready(dev)));
	} else {
		shell_print(shell, "%-18s %-18s %-10s not-present(%d)",
			    label, dev->name, ready_str(device_is_ready(dev)),
			    probe_ret);
	}
}

static void print_device_no_probe_line(const struct shell *shell,
				       const char *label,
				       const struct device *dev)
{
	shell_print(shell, "%-18s %-18s %-10s -", label, dev->name,
		    ready_str(device_is_ready(dev)));
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

static int parse_u32_arg(const char *arg, uint32_t *value)
{
	char *endptr;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(arg, &endptr, 0);
	if (errno != 0 || endptr == arg || *endptr != '\0' ||
	    parsed > UINT32_MAX) {
		return -EINVAL;
	}

	*value = (uint32_t)parsed;

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

static int parse_dac_channel_arg(const char *arg, size_t *index)
{
	if (strcmp(arg, "0") == 0 || strcmp(arg, "dac0") == 0 ||
	    strcmp(arg, "methane") == 0) {
		*index = 0U;
		return 0;
	}

	if (strcmp(arg, "1") == 0 || strcmp(arg, "dac1") == 0 ||
	    strcmp(arg, "pressure") == 0) {
		*index = 1U;
		return 0;
	}

	return -EINVAL;
}

static uint32_t gp8302_raw_from_current(uint32_t current_ua,
					uint32_t max_current_ua)
{
	if (max_current_ua == 0U || current_ua == 0U) {
		return 0U;
	}

	if (current_ua >= max_current_ua) {
		return GP8302_RAW_MAX;
	}

	return (uint32_t)(((uint64_t)current_ua * GP8302_RAW_MAX +
			   (max_current_ua / 2U)) /
			  max_current_ua);
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

	shell_print(shell, "%-32s %12lld %-8s alias=%-16s range=%lld..%lld%s",
		    setting->name, value, setting->unit,
		    setting->alias != NULL ? setting->alias : "-",
		    setting->min, setting->max,
		    setting->reboot_required ? " reboot-required" : "");
}

static void print_sensor_value(const struct shell *shell, const char *label,
			       const char *unit, const struct sensor_value *value)
{
	shell_print(shell, "%-18s %d.%06d %s", label, value->val1,
		    value->val2 < 0 ? -value->val2 : value->val2, unit);
}

static int cmd_argisense_drivers(const struct shell *shell, size_t argc,
				 char **argv)
{
	int power_ret;
	int dac0_probe = -ENODEV;
	int dac1_probe = -ENODEV;
	int eeprom_probe = -ENODEV;
	int humidity_probe = -ENODEV;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	argisense_power_measurement_lock();
	power_ret = argisense_power_measurement_on();
	if (power_ret == 0) {
		dac0_probe = i2c_address_probe(&dac0_i2c_spec);
		dac1_probe = i2c_address_probe(&dac1_i2c_spec);
		eeprom_probe = i2c_address_probe(&external_eeprom_i2c);
		humidity_probe = i2c_address_probe(&humidity_i2c_spec);
		argisense_power_idle_state();
	} else {
		shell_error(shell, "measurement rail enable failed: %d", power_ret);
		dac0_probe = power_ret;
		dac1_probe = power_ret;
		eeprom_probe = power_ret;
		humidity_probe = power_ret;
	}
	argisense_power_measurement_unlock();

	shell_print(shell, "ArgiSense driver readiness and I2C presence probe");
	shell_print(shell, "-------------------------------------------------");
	shell_print(shell, "%-18s %-18s %-10s %s", "device", "node", "driver",
		    "probe");
	print_device_no_probe_line(shell, "methane sensor", methane_sensor);
	print_device_no_probe_line(shell, "methane uart", methane_uart);
	print_device_no_probe_line(shell, "pressure sensor", pressure_sensor);
	print_device_no_probe_line(shell, "pressure spi", pressure_spi);
	print_device_probe_line(shell, "humidity sensor", humidity_sensor,
				humidity_probe);
	print_device_no_probe_line(shell, "humidity i2c", humidity_i2c);
	print_device_probe_line(shell, "dac0 methane", dac0, dac0_probe);
	print_device_no_probe_line(shell, "dac0 i2c", dac0_i2c);
	print_device_probe_line(shell, "dac1 pressure", dac1, dac1_probe);
	print_device_no_probe_line(shell, "dac1 i2c", dac1_i2c);
	print_device_probe_line(shell, "external eeprom", external_eeprom,
				eeprom_probe);
	print_device_no_probe_line(shell, "eeprom i2c", external_eeprom_i2c.bus);
	print_device_no_probe_line(shell, "rs485 uart", rs485_uart);

	shell_print(shell, "");
	shell_print(shell, "Mappings:");
	shell_print(shell, "  methane: %s on %s baud=%u",
		    methane_sensor->name, methane_uart->name,
		    DT_PROP(DT_BUS(METHANE_NODE), current_speed));
	shell_print(shell, "  pressure: %s on %s cs=%u freq=%u",
		    pressure_sensor->name, pressure_spi->name,
		    (unsigned int)DT_REG_ADDR(PRESSURE_NODE),
		    DT_PROP(PRESSURE_NODE, spi_max_frequency));
	shell_print(shell, "  dac0: %s addr=0x%02x max-current=%u uA",
		    dac0_i2c->name, DT_REG_ADDR(DAC0_NODE),
		    DT_PROP(DAC0_NODE, max_current_microamp));
	shell_print(shell, "  dac1: %s addr=0x%02x max-current=%u uA",
		    dac1_i2c->name, DT_REG_ADDR(DAC1_NODE),
		    DT_PROP(DAC1_NODE, max_current_microamp));
	shell_print(shell, "  eeprom: %s addr=0x%02x size=%zu bytes page=%u",
		    external_eeprom_i2c.bus->name, external_eeprom_i2c.addr,
		    eeprom_get_size(external_eeprom),
		    DT_PROP(EEPROM_NODE, pagesize));
	shell_print(shell, "");
	shell_print(shell,
		    "note: driver=Zephyr device init status; probe=I2C address ACK while measurement rails are on.");

	return 0;
}

static int write_dac_raw(const struct shell *shell,
			 const struct argisense_shell_dac *dac,
			 uint32_t raw)
{
	int ret;

	if (!device_is_ready(dac->i2c.bus)) {
		shell_error(shell, "%s I2C bus %s is not ready", dac->label,
			    dac->i2c.bus->name);
		return -ENODEV;
	}

	if (!device_is_ready(dac->dev)) {
		shell_error(shell, "%s device %s is not ready", dac->label,
			    dac->dev->name);
		return -ENODEV;
	}

	ret = dac_channel_setup(dac->dev, &gp8302_shell_channel_cfg);
	if (ret < 0) {
		shell_error(shell, "%s channel setup failed: %d", dac->label,
			    ret);
		return ret;
	}

	argisense_power_measurement_lock();
	ret = argisense_power_measurement_on();
	if (ret < 0) {
		argisense_power_measurement_unlock();
		shell_error(shell, "failed to power DAC/measurement rails: %d",
			    ret);
		return ret;
	}

	ret = dac_write_value(dac->dev, 0U, raw);
	argisense_power_idle_state();
	argisense_power_measurement_unlock();
	if (ret < 0) {
		shell_error(shell, "%s write failed: %d", dac->label, ret);
		return ret;
	}

	return 0;
}

static int cmd_argisense_dac(const struct shell *shell, size_t argc,
			     char **argv)
{
	const struct argisense_shell_dac *dac;
	uint32_t value;
	uint32_t raw;
	size_t index;
	bool raw_mode = false;
	int ret;

	if (argc >= 2U && strcmp(argv[1], "raw") == 0) {
		raw_mode = true;
	}

	if ((!raw_mode && argc != 3U) || (raw_mode && argc != 4U)) {
		shell_error(shell,
			    "usage: argisense dac <0|1|methane|pressure> <current-uA>");
		shell_error(shell,
			    "       argisense dac raw <0|1|methane|pressure> <0..4095>");
		return -EINVAL;
	}

	ret = parse_dac_channel_arg(raw_mode ? argv[2] : argv[1], &index);
	if (ret < 0 || index >= ARRAY_SIZE(shell_dacs)) {
		shell_error(shell, "invalid DAC channel: %s",
			    raw_mode ? argv[2] : argv[1]);
		return -EINVAL;
	}

	ret = parse_u32_arg(raw_mode ? argv[3] : argv[2], &value);
	if (ret < 0) {
		shell_error(shell, "invalid DAC value: %s",
			    raw_mode ? argv[3] : argv[2]);
		return ret;
	}

	dac = &shell_dacs[index];
	if (raw_mode) {
		if (value > GP8302_RAW_MAX) {
			shell_error(shell, "raw DAC value must be <= %u",
				    GP8302_RAW_MAX);
			return -EINVAL;
		}
		raw = value;
	} else {
		if (value > dac->max_current_ua) {
			shell_error(shell,
				    "current must be <= %u uA for %s",
				    dac->max_current_ua, dac->label);
			return -EINVAL;
		}
		raw = gp8302_raw_from_current(value, dac->max_current_ua);
	}

	ret = write_dac_raw(shell, dac, raw);
	if (ret < 0) {
		return ret;
	}

	if (raw_mode) {
		shell_print(shell, "%s wrote raw=0x%03x (%u)",
			    dac->label, raw, raw);
	} else {
		shell_print(shell,
			    "%s wrote current=%u uA raw=0x%03x max=%u uA",
			    dac->label, value, raw, dac->max_current_ua);
	}

	shell_print(shell,
		    "DAC rail returned to idle policy; periodic measurement may overwrite this test output.");

	return 0;
}

static void print_methane_sample(const struct shell *shell)
{
	struct sensor_value ppm;
	struct sensor_value percent;
	struct sensor_value status;
	int ret;

	ret = sensor_sample_fetch(methane_sensor);
	shell_print(shell, "methane fetch: %d", ret);
	if (ret < 0 && ret != -EAGAIN && ret != -EIO) {
		return;
	}

	if (sensor_channel_get(methane_sensor,
			       ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PPM,
			       &ppm) == 0) {
		print_sensor_value(shell, "methane", "ppm", &ppm);
	}

	if (sensor_channel_get(methane_sensor,
			       ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PERCENT_VOLUME,
			       &percent) == 0) {
		print_sensor_value(shell, "methane percent", "%vol", &percent);
	}

	if (sensor_channel_get(methane_sensor,
			       ARGISENSE_DYNAMENT_SENSOR_CHAN_STATUS_FLAGS,
			       &status) == 0) {
		shell_print(shell, "%-18s 0x%04x", "methane status",
			    (unsigned int)status.val1);
	}
}

static void print_pressure_sample(const struct shell *shell)
{
	struct sensor_value pressure;
	struct sensor_value temperature;
	int ret;

	ret = sensor_sample_fetch(pressure_sensor);
	shell_print(shell, "pressure fetch: %d", ret);
	if (ret < 0) {
		shell_print(shell,
			    "  note: check MS5803 wiring, SPI, PROM CRC, and measurement rail");
		return;
	}

	if (sensor_channel_get(pressure_sensor, SENSOR_CHAN_PRESS,
			       &pressure) == 0) {
		print_sensor_value(shell, "pressure", "kPa", &pressure);
	}

	if (sensor_channel_get(pressure_sensor, SENSOR_CHAN_AMBIENT_TEMP,
			       &temperature) == 0) {
		print_sensor_value(shell, "pressure temp", "C", &temperature);
	}
}

static void print_humidity_sample(const struct shell *shell)
{
	struct sensor_value humidity;
	struct sensor_value temperature;
	int ret;

	ret = sensor_sample_fetch(humidity_sensor);
	shell_print(shell, "humidity fetch: %d", ret);
	if (ret < 0) {
		return;
	}

	if (sensor_channel_get(humidity_sensor, SENSOR_CHAN_HUMIDITY,
			       &humidity) == 0) {
		print_sensor_value(shell, "humidity", "%RH", &humidity);
	}

	if (sensor_channel_get(humidity_sensor, SENSOR_CHAN_AMBIENT_TEMP,
			       &temperature) == 0) {
		print_sensor_value(shell, "humidity temp", "C", &temperature);
	}
}

static int cmd_argisense_sensors(const struct shell *shell, size_t argc,
				 char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "Powering measurement rails for shell sensor read...");
	argisense_power_measurement_lock();
	ret = argisense_power_measurement_on();
	if (ret < 0) {
		argisense_power_measurement_unlock();
		shell_error(shell, "failed to power measurement rails: %d", ret);
		return ret;
	}

	print_methane_sample(shell);
	print_pressure_sample(shell);
	print_humidity_sample(shell);

	argisense_power_idle_state();
	argisense_power_measurement_unlock();
	shell_print(shell, "Measurement rails returned to idle state.");

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
			shell_print(shell, "%04u  ----  %-24s ret=%d", addr,
				    name != NULL ? name : "reserved", ret);
			continue;
		}

		shell_print(shell, "%04u  0x%04x %-24s %u", addr, reg,
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

	shell_print(shell, "RS485 holding registers");
	shell_print(shell, "addr  hex    name                     dec");
	dump_rs485_range(shell, start, count);

	if (argc == 1U) {
		shell_print(shell, "");
		dump_rs485_range(shell, 40U, 20U);
		shell_print(shell, "");
		dump_rs485_range(shell, 70U, 13U);
#if defined(CONFIG_ARGISENSE_RS485_DFU)
		shell_print(shell, "");
		dump_rs485_range(shell, ARGISENSE_DFU_REG_CONTROL, 36U);
#endif
	}

	return 0;
}

static int cmd_argisense_settings(const struct shell *shell, size_t argc,
				  char **argv)
{
	const struct argisense_runtime_config *config = argisense_settings_get();
	const struct argisense_shell_setting *setting;
	struct argisense_runtime_config updated;
	int64_t value;
	int ret;

	if (argc == 1U || (argc == 2U && strcmp(argv[1], "list") == 0)) {
		shell_print(shell, "schema=%u size=%u", config->schema_version,
			    config->struct_size);
		shell_print(shell, "setting                          value        unit     alias            range");
		shell_print(shell, "-------------------------------------------------------------------------------");
		for (size_t i = 0U; i < ARRAY_SIZE(shell_settings); i++) {
			print_shell_setting(shell, &shell_settings[i], config);
		}
		shell_print(shell, "");
		shell_print(shell,
			    "rs485_data_bits is fixed at 8 for Modbus RTU; parity codes: 0=none, 1=odd, 2=even; stop bits: 1 or 2");
		shell_print(shell,
			    "usage: argisense settings get <name|alias>");
		shell_print(shell,
			    "       argisense settings set <name|alias> <value>");
		shell_print(shell,
			    "       argisense settings reset");
		return 0;
	}

	if (argc == 2U && strcmp(argv[1], "reset") == 0) {
		ret = argisense_settings_reset_defaults();
		if (ret < 0) {
			shell_error(shell, "settings reset failed: %d", ret);
			return ret;
		}

		shell_print(shell, "settings reset to defaults and saved to NVS");
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

		print_shell_setting(shell, setting, config);
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

		updated = *config;
		shell_setting_set_value(&updated, setting, value);

		ret = argisense_settings_save(&updated);
		if (ret < 0) {
			shell_error(shell,
				    "settings save rejected %s=%lld: %d",
				    setting->name, value, ret);
			shell_error(shell,
				    "check related limits such as low < high and min < max");
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

static int cmd_argisense_eeprom(const struct shell *shell, size_t argc,
				char **argv)
{
	uint8_t buf[64];
	uint32_t offset = 0U;
	uint32_t length = 16U;
	size_t size;
	int ret;

	if (argc > 3U) {
		shell_error(shell, "usage: argisense eeprom [offset] [length]");
		return -EINVAL;
	}

	if (argc >= 2U) {
		ret = parse_u32_arg(argv[1], &offset);
		if (ret < 0) {
			shell_error(shell, "invalid EEPROM offset: %s", argv[1]);
			return ret;
		}
	}

	if (argc >= 3U) {
		ret = parse_u32_arg(argv[2], &length);
		if (ret < 0 || length == 0U || length > sizeof(buf)) {
			shell_error(shell, "invalid EEPROM length: %s", argv[2]);
			return -EINVAL;
		}
	}

	size = eeprom_get_size(external_eeprom);
	if (offset >= size || length > (size - offset)) {
		shell_error(shell, "EEPROM range outside device size %zu", size);
		return -EINVAL;
	}

	shell_print(shell, "AT24C512C EEPROM: device=%s bus=%s addr=0x%02x size=%zu",
		    external_eeprom->name, external_eeprom_i2c.bus->name,
		    external_eeprom_i2c.addr, size);
	shell_print(shell, "Powering measurement rail for EEPROM read...");

	argisense_power_measurement_lock();
	ret = argisense_power_measurement_on();
	if (ret < 0) {
		argisense_power_measurement_unlock();
		shell_error(shell, "failed to power measurement rails: %d", ret);
		return ret;
	}

	ret = eeprom_read(external_eeprom, (off_t)offset, buf, length);
	argisense_power_idle_state();
	argisense_power_measurement_unlock();
	if (ret < 0) {
		shell_error(shell, "EEPROM read failed: %d", ret);
		return ret;
	}

	for (uint32_t pos = 0U; pos < length;
	     pos += SHELL_HEXDUMP_BYTES_IN_LINE) {
		size_t line_len = MIN(length - pos, SHELL_HEXDUMP_BYTES_IN_LINE);

		shell_hexdump_line(shell, offset + pos, &buf[pos], line_len);
	}

	shell_print(shell, "Measurement rails returned to idle state.");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	argisense_cmds,
	SHELL_CMD(drivers, NULL, "Show ArgiSense driver/device readiness.",
		  cmd_argisense_drivers),
	SHELL_CMD(dac, NULL,
		  "Write GP8302 output: dac <ch> <uA> or dac raw <ch> <raw>.",
		  cmd_argisense_dac),
	SHELL_CMD(eeprom, NULL, "Read AT24C512C EEPROM: eeprom [offset] [length].",
		  cmd_argisense_eeprom),
	SHELL_CMD(sensors, NULL, "Fetch and print methane, pressure, and humidity.",
		  cmd_argisense_sensors),
	SHELL_CMD(rs485, NULL, "Dump RS485 holding registers: rs485 [start] [count].",
		  cmd_argisense_rs485),
	SHELL_CMD(settings, NULL, "Print runtime settings.",
		  cmd_argisense_settings),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(argisense, &argisense_cmds,
		   "ArgiSense diagnostic commands.", NULL);
