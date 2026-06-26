/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "argisense_settings.h"

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(argisense_settings, LOG_LEVEL_INF);

#define ARGISENSE_SETTINGS_HANDLER_NAME "argisense"
#define ARGISENSE_SETTINGS_CONFIG_KEY "config"
#define ARGISENSE_SETTINGS_CONFIG_PATH \
	ARGISENSE_SETTINGS_HANDLER_NAME "/" ARGISENSE_SETTINGS_CONFIG_KEY
#define ARGISENSE_RS485_NODE DT_ALIAS(rs485_uart)

BUILD_ASSERT(sizeof(struct argisense_runtime_config) <= SETTINGS_MAX_VAL_LEN,
	     "ArgiSense settings record must fit in one Zephyr settings value");

static struct argisense_runtime_config runtime_config;
static bool settings_loaded_from_storage;

static void argisense_settings_defaults(struct argisense_runtime_config *config)
{
	*config = (struct argisense_runtime_config){
		.schema_version = ARGISENSE_SETTINGS_SCHEMA_VERSION,
		.struct_size = sizeof(*config),
		.measurement_period_seconds =
			CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS,
		.measurement_window_ms = CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS,
		.methane_warmup_seconds =
			CONFIG_ARGISENSE_METHANE_SENSOR_WARMUP_SECONDS,
		.methane_read_period_ms =
			CONFIG_ARGISENSE_METHANE_SENSOR_READ_PERIOD_MS,
		.humidity_read_period_seconds =
			CONFIG_ARGISENSE_HUMIDITY_SENSOR_READ_PERIOD_SECONDS,
		.methane_dac_range_low_ppm =
			CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM,
		.methane_dac_range_high_ppm =
			CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM,
		.pressure_dac_range_low_pa =
			CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA,
		.pressure_dac_range_high_pa =
			CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA,
		.dac_min_current_ua = CONFIG_ARGISENSE_DAC_MIN_CURRENT_UA,
		.dac_max_current_ua = CONFIG_ARGISENSE_DAC_MAX_CURRENT_UA,
		.dac_fault_current_ua = CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA,
		.rs485_baudrate = DT_PROP(ARGISENSE_RS485_NODE, current_speed),
		.modbus_address = 1U,
	};
}

static int argisense_settings_validate(
	const struct argisense_runtime_config *config)
{
	if (config->schema_version != ARGISENSE_SETTINGS_SCHEMA_VERSION ||
	    config->struct_size != sizeof(*config)) {
		return -EINVAL;
	}

	if (config->measurement_period_seconds <
		    ARGISENSE_MEASUREMENT_PERIOD_SECONDS_MIN ||
	    config->measurement_window_ms < ARGISENSE_MEASUREMENT_WINDOW_MS_MIN ||
	    config->measurement_window_ms > ARGISENSE_MEASUREMENT_WINDOW_MS_MAX ||
	    config->methane_read_period_ms < 100U ||
	    config->humidity_read_period_seconds < 1U) {
		return -EINVAL;
	}

	if (config->methane_dac_range_low_ppm >=
	    config->methane_dac_range_high_ppm) {
		return -EINVAL;
	}

	if (config->pressure_dac_range_low_pa >=
	    config->pressure_dac_range_high_pa) {
		return -EINVAL;
	}

	if (config->dac_min_current_ua < 0 ||
	    config->dac_max_current_ua > 25000 ||
	    config->dac_fault_current_ua < 0 ||
	    config->dac_fault_current_ua > 25000 ||
	    config->dac_min_current_ua >= config->dac_max_current_ua) {
		return -EINVAL;
	}

	if (config->dac0_4ma_trim_ua < -2000 ||
	    config->dac0_4ma_trim_ua > 2000 ||
	    config->dac0_20ma_trim_ua < -2000 ||
	    config->dac0_20ma_trim_ua > 2000 ||
	    config->dac1_4ma_trim_ua < -2000 ||
	    config->dac1_4ma_trim_ua > 2000 ||
	    config->dac1_20ma_trim_ua < -2000 ||
	    config->dac1_20ma_trim_ua > 2000) {
		return -EINVAL;
	}

	if (config->modbus_address < ARGISENSE_MODBUS_ADDRESS_MIN ||
	    config->modbus_address > ARGISENSE_MODBUS_ADDRESS_MAX ||
	    config->rs485_baudrate == 0U) {
		return -EINVAL;
	}

	if (config->rs485_termination_enabled > 1U) {
		return -EINVAL;
	}

	return 0;
}

static int argisense_settings_get_handler(const char *name, char *val,
					  int val_len_max)
{
	const char *next;

	if (settings_name_steq(name, ARGISENSE_SETTINGS_CONFIG_KEY, &next) &&
	    !next) {
		if (val_len_max < sizeof(runtime_config)) {
			return -ENOMEM;
		}

		memcpy(val, &runtime_config, sizeof(runtime_config));
		return sizeof(runtime_config);
	}

	return -ENOENT;
}

static int argisense_settings_set_handler(const char *name, size_t len,
					  settings_read_cb read_cb,
					  void *cb_arg)
{
	struct argisense_runtime_config loaded;
	const char *next;
	ssize_t bytes_read;
	int ret;

	if (!settings_name_steq(name, ARGISENSE_SETTINGS_CONFIG_KEY, &next) ||
	    next) {
		return -ENOENT;
	}

	if (len != sizeof(loaded)) {
		LOG_WRN("Ignoring incompatible settings record size %u",
			(unsigned int)len);
		return 0;
	}

	bytes_read = read_cb(cb_arg, &loaded, sizeof(loaded));
	if (bytes_read < 0) {
		return (int)bytes_read;
	}

	if (bytes_read != sizeof(loaded)) {
		return -EIO;
	}

	ret = argisense_settings_validate(&loaded);
	if (ret < 0) {
		LOG_WRN("Ignoring invalid persistent settings record: %d", ret);
		return 0;
	}

	runtime_config = loaded;
	settings_loaded_from_storage = true;
	return 0;
}

static int argisense_settings_export_handler(
	int (*export_func)(const char *name, const void *val, size_t val_len))
{
	return export_func(ARGISENSE_SETTINGS_CONFIG_PATH, &runtime_config,
			   sizeof(runtime_config));
}

static struct settings_handler argisense_settings_handler = {
	.name = ARGISENSE_SETTINGS_HANDLER_NAME,
	.h_get = argisense_settings_get_handler,
	.h_set = argisense_settings_set_handler,
	.h_export = argisense_settings_export_handler,
};

int argisense_settings_save(const struct argisense_runtime_config *config)
{
	int ret;

	ret = argisense_settings_validate(config);
	if (ret < 0) {
		return ret;
	}

	runtime_config = *config;

	return settings_save_one(ARGISENSE_SETTINGS_CONFIG_PATH, &runtime_config,
				 sizeof(runtime_config));
}

int argisense_settings_reset_defaults(void)
{
	struct argisense_runtime_config defaults;

	argisense_settings_defaults(&defaults);

	return argisense_settings_save(&defaults);
}

const struct argisense_runtime_config *argisense_settings_get(void)
{
	return &runtime_config;
}

void argisense_settings_log_summary(void)
{
	LOG_INF("Settings source: %s",
		settings_loaded_from_storage ? "storage" : "defaults");
	LOG_INF("Runtime config: period=%us window=%ums methane-warmup=%us methane-read=%ums humidity-read=%us",
		runtime_config.measurement_period_seconds,
		runtime_config.measurement_window_ms,
		runtime_config.methane_warmup_seconds,
		runtime_config.methane_read_period_ms,
		runtime_config.humidity_read_period_seconds);
	LOG_INF("Runtime DAC config: methane=%d..%d ppm pressure=%d..%d Pa current=%d..%d uA fault=%d uA",
		runtime_config.methane_dac_range_low_ppm,
		runtime_config.methane_dac_range_high_ppm,
		runtime_config.pressure_dac_range_low_pa,
		runtime_config.pressure_dac_range_high_pa,
		runtime_config.dac_min_current_ua,
		runtime_config.dac_max_current_ua,
		runtime_config.dac_fault_current_ua);
	LOG_INF("Runtime RS485 config: modbus-address=%u baud=%u termination=%s",
		runtime_config.modbus_address, runtime_config.rs485_baudrate,
		runtime_config.rs485_termination_enabled ? "on" : "off");
}

int argisense_settings_init(void)
{
	int ret;

	argisense_settings_defaults(&runtime_config);

	ret = settings_subsys_init();
	if (ret < 0) {
		LOG_ERR("Settings subsystem init failed: %d", ret);
		return ret;
	}

	ret = settings_register(&argisense_settings_handler);
	if (ret < 0) {
		LOG_ERR("Settings handler register failed: %d", ret);
		return ret;
	}

	ret = settings_load();
	if (ret < 0) {
		LOG_ERR("Settings load failed: %d", ret);
		return ret;
	}

	if (!settings_loaded_from_storage) {
		LOG_INF("No persistent settings found; saving defaults");
		ret = argisense_settings_save(&runtime_config);
		if (ret < 0) {
			LOG_WRN("Failed to save default settings: %d", ret);
			return ret;
		}
	}

	return 0;
}
