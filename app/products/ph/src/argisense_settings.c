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

LOG_MODULE_REGISTER(argisense_ph_settings, LOG_LEVEL_INF);

#define ARGISENSE_SETTINGS_HANDLER_NAME "argisense_ph"
#define ARGISENSE_SETTINGS_CONFIG_KEY "config"
#define ARGISENSE_SETTINGS_CONFIG_PATH \
	ARGISENSE_SETTINGS_HANDLER_NAME "/" ARGISENSE_SETTINGS_CONFIG_KEY
#define ARGISENSE_RS485_NODE DT_ALIAS(rs485_uart)

BUILD_ASSERT(sizeof(struct argisense_runtime_config) <= SETTINGS_MAX_VAL_LEN,
	     "ArgiSense pH settings record must fit in one Zephyr settings value");

static struct argisense_runtime_config runtime_config;
static K_MUTEX_DEFINE(runtime_config_lock);
static K_MUTEX_DEFINE(settings_save_lock);
static bool settings_loaded_from_storage;

static void argisense_settings_normalize(
	struct argisense_runtime_config *config)
{
	if (config->rs485_stop_bits == 0U) {
		config->rs485_stop_bits = ARGISENSE_RS485_STOP_BITS_2;
	}

	if (config->rs485_data_bits == 0U) {
		config->rs485_data_bits = ARGISENSE_RS485_DATA_BITS_8;
	}

	if (config->ph_calibration_mode == ARGISENSE_PH_CAL_MODE_DISABLED) {
		config->ph_calibration_valid = 0U;
	}
}

static void argisense_settings_defaults(struct argisense_runtime_config *config)
{
	*config = (struct argisense_runtime_config){
		.schema_version = ARGISENSE_SETTINGS_SCHEMA_VERSION,
		.struct_size = sizeof(*config),
		.measurement_period_seconds =
			CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS,
		.measurement_window_ms = CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS,
		.ph_range_low_x1000 = 0,
		.ph_range_high_x1000 = 14000,
		.temperature_range_low_centi_c = 0,
		.temperature_range_high_centi_c = 8000,
		.ph_slope_x1000 = 1000,
		.ph_offset_x1000 = 0,
		.ph7_raw_uv = 0,
		.ph_slope_uv_per_ph = 59000,
		.ph_temp_reference_centi_c = 2500,
		.ph_temp_coeff_ppm_per_c = 3354,
		.dac_min_current_ua = CONFIG_ARGISENSE_DAC_MIN_CURRENT_UA,
		.dac_max_current_ua = CONFIG_ARGISENSE_DAC_MAX_CURRENT_UA,
		.dac_fault_current_ua = CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA,
		.rs485_baudrate = DT_PROP(ARGISENSE_RS485_NODE, current_speed),
		.modbus_address = 1U,
		.rs485_parity = ARGISENSE_RS485_PARITY_NONE,
		.rs485_stop_bits = ARGISENSE_RS485_STOP_BITS_2,
		.rs485_data_bits = ARGISENSE_RS485_DATA_BITS_8,
		.ph_calibration_mode = ARGISENSE_PH_CAL_MODE_DISABLED,
		.ph_calibration_valid = 0U,
	};
}

static int32_t i32_abs_clamped(int32_t value)
{
	if (value == INT32_MIN) {
		return INT32_MAX;
	}

	return value < 0 ? -value : value;
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
	    config->measurement_window_ms > ARGISENSE_MEASUREMENT_WINDOW_MS_MAX) {
		return -EINVAL;
	}

	if (config->ph_range_low_x1000 >= config->ph_range_high_x1000 ||
	    config->temperature_range_low_centi_c >=
		    config->temperature_range_high_centi_c) {
		return -EINVAL;
	}

	if (config->ph_slope_x1000 < -100000 ||
	    config->ph_slope_x1000 > 100000 ||
	    config->ph_temp_coeff_ppm_per_c < -20000 ||
	    config->ph_temp_coeff_ppm_per_c > 20000 ||
	    config->ph_temp_reference_centi_c < -4000 ||
	    config->ph_temp_reference_centi_c > 13500) {
		return -EINVAL;
	}

	if (config->ph_calibration_mode >
		    ARGISENSE_PH_CAL_MODE_REFERENCE_ELECTRODE ||
	    config->ph_calibration_valid > 1U) {
		return -EINVAL;
	}

	if (config->ph_calibration_valid != 0U &&
	    (config->ph_calibration_mode == ARGISENSE_PH_CAL_MODE_DISABLED ||
	     i32_abs_clamped(config->ph_slope_uv_per_ph) < 1000 ||
	     i32_abs_clamped(config->ph_slope_uv_per_ph) > 200000)) {
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

	if (config->rs485_parity > ARGISENSE_RS485_PARITY_EVEN ||
	    (config->rs485_stop_bits != ARGISENSE_RS485_STOP_BITS_1 &&
	     config->rs485_stop_bits != ARGISENSE_RS485_STOP_BITS_2)) {
		return -EINVAL;
	}

	if (config->rs485_data_bits != ARGISENSE_RS485_DATA_BITS_8) {
		return -EINVAL;
	}

	return 0;
}

static int argisense_settings_get_handler(const char *name, char *val,
					  int val_len_max)
{
	struct argisense_runtime_config config;
	const char *next;

	if (settings_name_steq(name, ARGISENSE_SETTINGS_CONFIG_KEY, &next) &&
	    !next) {
		if (val_len_max < sizeof(config)) {
			return -ENOMEM;
		}

		argisense_settings_get_copy(&config);
		memcpy(val, &config, sizeof(config));
		return sizeof(config);
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
		LOG_WRN("Ignoring incompatible pH settings record size %u",
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

	argisense_settings_normalize(&loaded);

	ret = argisense_settings_validate(&loaded);
	if (ret < 0) {
		LOG_WRN("Ignoring invalid pH settings record: %d", ret);
		return 0;
	}

	k_mutex_lock(&runtime_config_lock, K_FOREVER);
	runtime_config = loaded;
	k_mutex_unlock(&runtime_config_lock);
	settings_loaded_from_storage = true;

	return 0;
}

static int argisense_settings_export_handler(
	int (*export_func)(const char *name, const void *val, size_t val_len))
{
	struct argisense_runtime_config config;

	argisense_settings_get_copy(&config);

	return export_func(ARGISENSE_SETTINGS_CONFIG_PATH, &config,
			   sizeof(config));
}

static struct settings_handler argisense_settings_handler = {
	.name = ARGISENSE_SETTINGS_HANDLER_NAME,
	.h_get = argisense_settings_get_handler,
	.h_set = argisense_settings_set_handler,
	.h_export = argisense_settings_export_handler,
};

int argisense_settings_save(const struct argisense_runtime_config *config)
{
	struct argisense_runtime_config normalized = *config;
	int ret;

	argisense_settings_normalize(&normalized);

	ret = argisense_settings_validate(&normalized);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&settings_save_lock, K_FOREVER);
	ret = settings_save_one(ARGISENSE_SETTINGS_CONFIG_PATH, &normalized,
				sizeof(normalized));
	if (ret < 0) {
		k_mutex_unlock(&settings_save_lock);
		return ret;
	}

	k_mutex_lock(&runtime_config_lock, K_FOREVER);
	runtime_config = normalized;
	k_mutex_unlock(&runtime_config_lock);
	settings_loaded_from_storage = true;
	k_mutex_unlock(&settings_save_lock);

	return 0;
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

void argisense_settings_get_copy(struct argisense_runtime_config *config)
{
	if (config == NULL) {
		return;
	}

	k_mutex_lock(&runtime_config_lock, K_FOREVER);
	*config = runtime_config;
	k_mutex_unlock(&runtime_config_lock);
}

void argisense_settings_log_summary(void)
{
	struct argisense_runtime_config config;

	argisense_settings_get_copy(&config);

	LOG_INF("pH settings source: %s",
		settings_loaded_from_storage ? "storage" : "defaults");
	LOG_INF("pH runtime config: period=%us window=%ums pH=%d..%d x1000 temperature=%d..%d centi-C",
		config.measurement_period_seconds, config.measurement_window_ms,
		config.ph_range_low_x1000, config.ph_range_high_x1000,
		config.temperature_range_low_centi_c,
		config.temperature_range_high_centi_c);
	LOG_INF("pH calibration: slope=%d x1000 offset=%d x1000",
		config.ph_slope_x1000, config.ph_offset_x1000);
	LOG_INF("pH raw calibration: mode=%u valid=%u ph7_raw=%d uV slope=%d uV/pH temp_ref=%d centi-C temp_coeff=%d ppm/C",
		config.ph_calibration_mode, config.ph_calibration_valid,
		config.ph7_raw_uv, config.ph_slope_uv_per_ph,
		config.ph_temp_reference_centi_c,
		config.ph_temp_coeff_ppm_per_c);
	LOG_INF("Runtime RS485 config: modbus-address=%u baud=%u data-bits=%u parity=%u stop-bits=%u termination=%s",
		config.modbus_address, config.rs485_baudrate,
		config.rs485_data_bits, config.rs485_parity,
		config.rs485_stop_bits,
		config.rs485_termination_enabled ? "on" : "off");
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
		LOG_ERR("pH settings handler register failed: %d", ret);
		return ret;
	}

	ret = settings_load();
	if (ret < 0) {
		LOG_ERR("Settings load failed: %d", ret);
		return ret;
	}

	if (!settings_loaded_from_storage) {
		LOG_INF("No persistent pH settings found; saving defaults");
		ret = argisense_settings_save(&runtime_config);
		if (ret < 0) {
			LOG_WRN("Failed to save default pH settings: %d", ret);
			return ret;
		}
	}

	return 0;
}
