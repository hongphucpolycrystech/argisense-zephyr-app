/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <argisense/drivers/sensor/dynament_platinum.h>
#include <argisense/drivers/sensor/ms5803_05ba.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

#include "argisense_settings.h"
#include "argisense_rs485.h"
#include "argisense_registers.h"
#include "argisense_power.h"
#include "current_loop_output.h"

LOG_MODULE_REGISTER(argisense, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define DAC0_NODE DT_ALIAS(current_loop_dac0)
#define DAC1_NODE DT_ALIAS(current_loop_dac1)
#define EEPROM_NODE DT_ALIAS(eeprom0)
#define PRESSURE_NODE DT_ALIAS(pressure_sensor)
#define METHANE_NODE DT_ALIAS(methane_sensor)
#define HUMIDITY_NODE DT_ALIAS(humidity_sensor)
#define GP8302_RAW_MAX 0x0FFFU

BUILD_ASSERT(DT_NODE_HAS_STATUS(DAC0_NODE, okay),
	     "Board must define okay current-loop-dac0 alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DAC1_NODE, okay),
	     "Board must define okay current-loop-dac1 alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(EEPROM_NODE, okay),
	     "Board must define okay eeprom0 alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(PRESSURE_NODE, okay),
	     "Board must define okay pressure-sensor alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(METHANE_NODE, okay),
	     "Board must define okay methane-sensor alias");
BUILD_ASSERT(DT_PROP(METHANE_NODE, live_data_variable_id) == 0x06,
	     "Methane sensor live-data-variable-id must match Dynament live-data-simple");
BUILD_ASSERT(DT_NODE_HAS_STATUS(HUMIDITY_NODE, okay),
	     "Board must define okay humidity-sensor alias");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, dac_channel_count),
	     "zephyr,user must define dac-channel-count");
BUILD_ASSERT(DT_PROP(USER_NODE, dac_channel_count) ==
	     ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT,
	     "zephyr,user dac-channel-count must match current-loop channels");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, dac_power_gpios),
	     "zephyr,user must define dac-power-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, dac_alarm_gpios),
	     "zephyr,user must define dac-alarm-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, pre_power_gpios),
	     "zephyr,user must define pre-power-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, analog_power_gpios),
	     "zephyr,user must define analog-power-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, rs485_termination_gpios),
	     "zephyr,user must define rs485-termination-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, pressure_ps_gpios),
	     "zephyr,user must define pressure-ps-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, pressure_cs_gpios),
	     "zephyr,user must define pressure-cs-gpios");

static const struct device *const pressure_sensor = DEVICE_DT_GET(PRESSURE_NODE);
static const struct device *const pressure_spi = DEVICE_DT_GET(DT_BUS(PRESSURE_NODE));
static const struct device *const methane_sensor = DEVICE_DT_GET(METHANE_NODE);
static const struct device *const methane_uart = DEVICE_DT_GET(DT_BUS(METHANE_NODE));
static const struct device *const humidity_sensor = DEVICE_DT_GET(HUMIDITY_NODE);
static const struct device *const humidity_i2c = DEVICE_DT_GET(DT_BUS(HUMIDITY_NODE));
static const struct device *const external_eeprom = DEVICE_DT_GET(EEPROM_NODE);
static const struct i2c_dt_spec external_eeprom_i2c = I2C_DT_SPEC_GET(EEPROM_NODE);

struct argisense_dac_device {
	const struct i2c_dt_spec i2c;
	const struct device *dev;
	uint32_t max_current_ua;
	const char *name;
};

static const struct argisense_dac_device dac_devices[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT] = {
	[ARGISENSE_CURRENT_LOOP_METHANE] = {
		.i2c = I2C_DT_SPEC_GET(DAC0_NODE),
		.dev = DEVICE_DT_GET(DAC0_NODE),
		.max_current_ua = DT_PROP(DAC0_NODE, max_current_microamp),
		.name = "DAC0 methane GP8302",
	},
	[ARGISENSE_CURRENT_LOOP_PRESSURE] = {
		.i2c = I2C_DT_SPEC_GET(DAC1_NODE),
		.dev = DEVICE_DT_GET(DAC1_NODE),
		.max_current_ua = DT_PROP(DAC1_NODE, max_current_microamp),
		.name = "DAC1 pressure GP8302",
	},
};

static const struct dac_channel_cfg gp8302_channel_cfg = {
	.channel_id = 0U,
	.resolution = 12U,
};

static void argisense_usb_shell_diagnostics(void)
{
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_shell_uart), zephyr_cdc_acm_uart)
	const struct device *const shell_uart =
		DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));
	uint32_t dtr = 0U;
	int ret;

	LOG_INF("USB CDC shell selected: device=%s ready=%s",
		shell_uart->name, device_is_ready(shell_uart) ? "yes" : "no");

#if DT_NODE_HAS_STATUS(DT_NODELABEL(zephyr_udc0), okay)
	const struct device *const udc = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));

	LOG_INF("USB device controller: device=%s ready=%s",
		udc->name, device_is_ready(udc) ? "yes" : "no");
#else
	LOG_ERR("USB CDC shell selected but zephyr_udc0 is not okay in DTS");
#endif

#if defined(CONFIG_CDC_ACM_SERIAL_PRODUCT_STRING)
	LOG_INF("USB CDC product='%s' pid=0x%04x auto-init=%s auto-enable=%s",
		CONFIG_CDC_ACM_SERIAL_PRODUCT_STRING,
		CONFIG_CDC_ACM_SERIAL_PID,
		IS_ENABLED(CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT) ? "yes" : "no",
		IS_ENABLED(CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT) ? "yes" : "no");
#elif defined(CONFIG_ARGISENSE_USB_PRODUCT_STRING)
	LOG_INF("USB CDC product='%s' pid=0x%04x app-composite-init=%s",
		CONFIG_ARGISENSE_USB_PRODUCT_STRING,
		CONFIG_ARGISENSE_USB_PID,
		IS_ENABLED(CONFIG_ARGISENSE_USB_DEVICE) ? "yes" : "no");
#endif

	if (!device_is_ready(shell_uart)) {
		LOG_WRN("USB CDC shell UART is not ready; skip DTR read");
		return;
	}

	ret = uart_line_ctrl_get(shell_uart, UART_LINE_CTRL_DTR, &dtr);
	if (ret == 0) {
		LOG_INF("USB CDC DTR=%u; 0 means host has not opened the COM port",
			(unsigned int)dtr);
	} else {
		LOG_WRN("USB CDC DTR read failed: %d", ret);
	}
#else
	LOG_INF("USB CDC shell is not selected in DTS");
#endif

#if DT_HAS_CHOSEN(zephyr_uart_mcumgr)
	const struct device *const mcumgr_uart =
		DEVICE_DT_GET(DT_CHOSEN(zephyr_uart_mcumgr));
	uint32_t mcumgr_dtr = 0U;
	int mcumgr_ret;

	LOG_INF("USB CDC MCUmgr update selected: device=%s ready=%s",
		mcumgr_uart->name, device_is_ready(mcumgr_uart) ? "yes" : "no");

	if (device_is_ready(mcumgr_uart)) {
		mcumgr_ret = uart_line_ctrl_get(mcumgr_uart, UART_LINE_CTRL_DTR,
						&mcumgr_dtr);
		if (mcumgr_ret == 0) {
			LOG_INF("USB CDC MCUmgr DTR=%u; open this COM port with mcumgr",
				(unsigned int)mcumgr_dtr);
		} else {
			LOG_WRN("USB CDC MCUmgr DTR read failed: %d", mcumgr_ret);
		}
	}
#else
	LOG_INF("USB CDC MCUmgr update UART is not selected in DTS");
#endif
}

struct argisense_humidity_cache {
	int32_t humidity_rh_x100;
	int32_t temperature_centi_c;
	int32_t last_error;
	int64_t last_read_ms;
	bool valid;
	bool initialized;
};

static struct argisense_humidity_cache humidity_cache;

static int argisense_dac_devices_check(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(dac_devices); i++) {
		const struct argisense_dac_device *dac = &dac_devices[i];
		int ret;

		if (!device_is_ready(dac->i2c.bus)) {
			LOG_ERR("%s I2C bus is not ready", dac->name);
			return -ENODEV;
		}

		if (!device_is_ready(dac->dev)) {
			LOG_ERR("%s device is not ready", dac->name);
			return -ENODEV;
		}

		if (dac->max_current_ua == 0U) {
			LOG_ERR("%s max current must be greater than zero",
				dac->name);
			return -EINVAL;
		}

		ret = dac_channel_setup(dac->dev, &gp8302_channel_cfg);
		if (ret < 0) {
			LOG_ERR("%s channel setup failed: %d", dac->name, ret);
			return ret;
		}

		LOG_INF("%s mapped to %s address 0x%02x max-current=%u uA",
			dac->name, dac->i2c.bus->name, dac->i2c.addr,
			dac->max_current_ua);
	}

	return 0;
}

static int argisense_methane_sensor_check(void)
{
	const uint32_t full_scale = DT_PROP(METHANE_NODE, full_scale_percent_x100);

	if (!device_is_ready(methane_uart)) {
		LOG_ERR("Methane sensor UART %s is not ready", methane_uart->name);
		return -ENODEV;
	}

	if (!device_is_ready(methane_sensor)) {
		LOG_ERR("Dynament methane sensor device %s is not ready",
			methane_sensor->name);
		return -ENODEV;
	}

	LOG_INF("Dynament methane sensor mapped to %s on %s at %u baud",
		methane_sensor->name, methane_uart->name,
		DT_PROP(DT_BUS(METHANE_NODE), current_speed));
	LOG_INF("Dynament protocol=%s full-scale=%u.%02u%%vol warmup=%ums",
		DT_PROP(METHANE_NODE, protocol), full_scale / 100, full_scale % 100,
		DT_PROP(METHANE_NODE, warmup_time_ms));

	return 0;
}

static int argisense_humidity_sensor_check(void)
{
	if (!device_is_ready(humidity_i2c)) {
		LOG_ERR("HTU21D I2C bus %s is not ready", humidity_i2c->name);
		return -ENODEV;
	}

	if (!device_is_ready(humidity_sensor)) {
		LOG_ERR("HTU21D sensor device %s is not ready", humidity_sensor->name);
		return -ENODEV;
	}

	LOG_INF("HTU21D humidity sensor mapped to %s on %s address 0x%02x",
		humidity_sensor->name, humidity_i2c->name, DT_REG_ADDR(HUMIDITY_NODE));

	return 0;
}

static int argisense_external_eeprom_check(void)
{
	if (!device_is_ready(external_eeprom_i2c.bus)) {
		LOG_ERR("AT24C512C EEPROM I2C bus %s is not ready",
			external_eeprom_i2c.bus->name);
		return -ENODEV;
	}

	if (!device_is_ready(external_eeprom)) {
		LOG_ERR("AT24C512C EEPROM device %s is not ready",
			external_eeprom->name);
		return -ENODEV;
	}

	LOG_INF("AT24C512C EEPROM mapped to %s address 0x%02x size=%zu bytes page=%u address-width=%u timeout=%ums",
		external_eeprom_i2c.bus->name, external_eeprom_i2c.addr,
		eeprom_get_size(external_eeprom),
		DT_PROP(EEPROM_NODE, pagesize),
		DT_PROP(EEPROM_NODE, address_width),
		DT_PROP(EEPROM_NODE, timeout));

	return 0;
}

static int argisense_pressure_sensor_check(void)
{
	if (!device_is_ready(pressure_spi)) {
		LOG_ERR("MS5803 SPI bus %s is not ready", pressure_spi->name);
		return -ENODEV;
	}

	if (!device_is_ready(pressure_sensor)) {
		LOG_ERR("MS5803 pressure sensor device %s is not ready",
			pressure_sensor->name);
		return -ENODEV;
	}

	LOG_INF("MS5803 pressure sensor %s mapped to %s CS%u at %u Hz range=%u kPa",
		DT_PROP(PRESSURE_NODE, part_number), pressure_spi->name,
		(unsigned int)DT_REG_ADDR(PRESSURE_NODE),
		DT_PROP(PRESSURE_NODE, spi_max_frequency),
		DT_PROP(PRESSURE_NODE, pressure_range_kpa));

	return 0;
}

static int argisense_log_firmware_version(void)
{
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	struct mcuboot_img_header header;
	const struct mcuboot_img_sem_ver *version;
	uint8_t active_slot;
	int ret;

	active_slot = boot_fetch_active_slot();
	ret = boot_read_bank_header(active_slot, &header, sizeof(header));
	if (ret < 0) {
		LOG_ERR("Failed to read MCUboot image header from area %u: %d",
			(unsigned int)active_slot, ret);
		return ret;
	}

	if (header.mcuboot_version != 1U) {
		LOG_ERR("Unsupported MCUboot image header version %u",
			(unsigned int)header.mcuboot_version);
		return -ENOTSUP;
	}

	version = &header.h.v1.sem_ver;
	LOG_INF("MCUboot image version %u.%u.%u+%u from flash area %u",
		(unsigned int)version->major, (unsigned int)version->minor,
		(unsigned int)version->revision, (unsigned int)version->build_num,
		(unsigned int)active_slot);
	return 0;
#else
	LOG_WRN("MCUboot image version unavailable: firmware is not built with MCUboot");
	return 0;
#endif
}

static int argisense_read_methane_sample(
	struct argisense_measurement_sample *sample)
{
	const struct argisense_runtime_config *config = argisense_settings_get();
	struct sensor_value gas_ppm;
	struct sensor_value gas_percent;
	struct sensor_value status_flags;
	struct sensor_value protocol_version;
	int64_t methane_ppm_x100;
	bool percent_ready = false;
	int fetch_ret;
	int ret;

	fetch_ret = sensor_sample_fetch(methane_sensor);
	sample->methane_last_error = fetch_ret;
	if (fetch_ret < 0 && fetch_ret != -EAGAIN && fetch_ret != -EIO) {
		LOG_ERR("Dynament methane sample fetch failed: %d", fetch_ret);
		sample->methane_valid = false;
		return fetch_ret;
	}

	ret = sensor_channel_get(methane_sensor,
				 ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PPM,
				 &gas_ppm);
	if (ret < 0) {
		LOG_ERR("Dynament methane gas channel read failed: %d", ret);
		sample->methane_valid = false;
		return ret;
	}

	methane_ppm_x100 = sensor_value_to_micro(&gas_ppm) / 10000LL;
	methane_ppm_x100 += config->methane_zero_offset_ppm_x100;
	if (methane_ppm_x100 < INT32_MIN || methane_ppm_x100 > INT32_MAX) {
		LOG_ERR("Dynament methane value out of range: %lld ppm_x100",
			(long long)methane_ppm_x100);
		sample->methane_valid = false;
		return -ERANGE;
	}

	sample->methane_ppm_x100 = (int32_t)methane_ppm_x100;
	sample->methane_valid = fetch_ret == 0;

	ret = sensor_channel_get(methane_sensor,
				 ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PERCENT_VOLUME,
				 &gas_percent);
	if (ret < 0) {
		LOG_WRN("Dynament methane percent channel read failed: %d", ret);
	} else {
		percent_ready = true;
	}

	ret = sensor_channel_get(methane_sensor,
				 ARGISENSE_DYNAMENT_SENSOR_CHAN_STATUS_FLAGS,
				 &status_flags);
	if (ret == 0) {
		sample->methane_status_flags = (uint16_t)status_flags.val1;
	}

	ret = sensor_channel_get(methane_sensor,
				 ARGISENSE_DYNAMENT_SENSOR_CHAN_PROTOCOL_VERSION,
				 &protocol_version);
	if (ret == 0) {
		sample->methane_protocol_version =
			(uint16_t)protocol_version.val1;
	}

	if (ret == 0 && percent_ready) {
		LOG_INF("Dynament methane=%d.%02d ppm status=0x%04x percent=%d.%06d%%vol",
			sample->methane_ppm_x100 / 100,
			abs(sample->methane_ppm_x100 % 100),
			(unsigned int)sample->methane_status_flags,
			gas_percent.val1,
			gas_percent.val2);
	} else if (ret == 0) {
		LOG_INF("Dynament methane=%d.%02d ppm status=0x%04x",
			sample->methane_ppm_x100 / 100,
			abs(sample->methane_ppm_x100 % 100),
			(unsigned int)sample->methane_status_flags);
	}

	return fetch_ret;
}

static int argisense_read_pressure_sample(
	struct argisense_measurement_sample *sample)
{
	const struct argisense_runtime_config *config = argisense_settings_get();
	struct sensor_value pressure_kpa;
	struct sensor_value temperature_c;
	struct sensor_value diagnostic;
	int64_t pressure_pa;
	int64_t temperature_centi_c;
	int ret;

	ret = sensor_sample_fetch(pressure_sensor);
	sample->pressure_last_error = ret;
	if (ret < 0) {
		LOG_ERR("MS5803 sample fetch failed: %d", ret);
		sample->pressure_valid = false;
		return ret;
	}

	ret = sensor_channel_get(pressure_sensor, SENSOR_CHAN_PRESS,
				 &pressure_kpa);
	if (ret < 0) {
		LOG_ERR("MS5803 pressure channel read failed: %d", ret);
		sample->pressure_valid = false;
		return ret;
	}

	pressure_pa = sensor_value_to_micro(&pressure_kpa) / 1000LL;
	pressure_pa += config->pressure_offset_pa;
	if (pressure_pa < INT32_MIN || pressure_pa > INT32_MAX) {
		LOG_ERR("MS5803 pressure value out of range: %lld Pa",
			(long long)pressure_pa);
		sample->pressure_valid = false;
		return -ERANGE;
	}

	sample->pressure_pa = (int32_t)pressure_pa;
	sample->pressure_valid = true;

	ret = sensor_channel_get(pressure_sensor, SENSOR_CHAN_AMBIENT_TEMP,
				 &temperature_c);
	if (ret == 0) {
		temperature_centi_c = sensor_value_to_micro(&temperature_c) /
				      10000LL;
		sample->pressure_temperature_centi_c =
			(int32_t)temperature_centi_c;
		LOG_INF("MS5803 pressure=%lld Pa temperature=%d.%06d C",
			(long long)pressure_pa, temperature_c.val1,
			temperature_c.val2);
	} else {
		LOG_WRN("MS5803 temperature channel read failed: %d", ret);
	}

	if (sensor_channel_get(pressure_sensor,
			       ARGISENSE_MS5803_SENSOR_CHAN_D1_RAW,
			       &diagnostic) == 0) {
		sample->pressure_d1_raw = (uint32_t)diagnostic.val1;
	}

	if (sensor_channel_get(pressure_sensor,
			       ARGISENSE_MS5803_SENSOR_CHAN_D2_RAW,
			       &diagnostic) == 0) {
		sample->pressure_d2_raw = (uint32_t)diagnostic.val1;
	}

	if (sensor_channel_get(pressure_sensor,
			       ARGISENSE_MS5803_SENSOR_CHAN_PROM_CRC_READ,
			       &diagnostic) == 0) {
		sample->pressure_prom_crc_read = (uint8_t)diagnostic.val1;
	}

	if (sensor_channel_get(pressure_sensor,
			       ARGISENSE_MS5803_SENSOR_CHAN_PROM_CRC_CALC,
			       &diagnostic) == 0) {
		sample->pressure_prom_crc_calc = (uint8_t)diagnostic.val1;
	}

	return 0;
}

static void argisense_apply_humidity_cache(
	struct argisense_measurement_sample *sample)
{
	sample->humidity_rh_x100 = humidity_cache.humidity_rh_x100;
	sample->humidity_temperature_centi_c = humidity_cache.temperature_centi_c;
	sample->humidity_last_error = humidity_cache.last_error;
	sample->humidity_valid = humidity_cache.valid;
}

static int argisense_read_humidity_sample(
	struct argisense_measurement_sample *sample)
{
	const struct argisense_runtime_config *config = argisense_settings_get();
	const int64_t now_ms = k_uptime_get();
	const int64_t read_period_ms =
		(int64_t)config->humidity_read_period_seconds * MSEC_PER_SEC;
	struct sensor_value humidity;
	struct sensor_value temperature;
	int64_t humidity_rh_x100;
	int64_t temperature_centi_c;
	int ret;

	if (humidity_cache.initialized &&
	    (now_ms - humidity_cache.last_read_ms) < read_period_ms) {
		argisense_apply_humidity_cache(sample);
		return humidity_cache.last_error;
	}

	ret = sensor_sample_fetch(humidity_sensor);
	humidity_cache.last_error = ret;
	humidity_cache.last_read_ms = now_ms;
	humidity_cache.initialized = true;
	if (ret < 0) {
		LOG_ERR("HTU21D sample fetch failed: %d", ret);
		humidity_cache.valid = false;
		argisense_apply_humidity_cache(sample);
		return ret;
	}

	ret = sensor_channel_get(humidity_sensor, SENSOR_CHAN_HUMIDITY,
				 &humidity);
	if (ret < 0) {
		LOG_ERR("HTU21D humidity channel read failed: %d", ret);
		humidity_cache.last_error = ret;
		humidity_cache.valid = false;
		argisense_apply_humidity_cache(sample);
		return ret;
	}

	ret = sensor_channel_get(humidity_sensor, SENSOR_CHAN_AMBIENT_TEMP,
				 &temperature);
	if (ret < 0) {
		LOG_ERR("HTU21D temperature channel read failed: %d", ret);
		humidity_cache.last_error = ret;
		humidity_cache.valid = false;
		argisense_apply_humidity_cache(sample);
		return ret;
	}

	humidity_rh_x100 = sensor_value_to_micro(&humidity) / 10000LL;
	temperature_centi_c = sensor_value_to_micro(&temperature) / 10000LL;

	if (humidity_rh_x100 < INT32_MIN || humidity_rh_x100 > INT32_MAX ||
	    temperature_centi_c < INT32_MIN ||
	    temperature_centi_c > INT32_MAX) {
		LOG_ERR("HTU21D value out of range");
		humidity_cache.last_error = -ERANGE;
		humidity_cache.valid = false;
		argisense_apply_humidity_cache(sample);
		return -ERANGE;
	}

	humidity_cache.humidity_rh_x100 = (int32_t)humidity_rh_x100;
	humidity_cache.temperature_centi_c = (int32_t)temperature_centi_c;
	humidity_cache.last_error = 0;
	humidity_cache.valid = true;
	argisense_apply_humidity_cache(sample);

	LOG_INF("HTU21D humidity=%d.%02d%%RH temperature=%d.%02d C",
		sample->humidity_rh_x100 / 100,
		abs(sample->humidity_rh_x100 % 100),
		sample->humidity_temperature_centi_c / 100,
		abs(sample->humidity_temperature_centi_c % 100));

	return 0;
}

static uint32_t argisense_gp8302_raw_from_current(
	const struct argisense_dac_device *dac, int32_t current_ua)
{
	uint32_t clamped_current_ua;

	if (current_ua <= 0) {
		return 0U;
	}

	if ((uint32_t)current_ua >= dac->max_current_ua) {
		return GP8302_RAW_MAX;
	}

	clamped_current_ua = (uint32_t)current_ua;

	return (uint32_t)(((uint64_t)clamped_current_ua * GP8302_RAW_MAX +
			   (dac->max_current_ua / 2U)) /
			  dac->max_current_ua);
}

static int argisense_dac_write_outputs(
	const struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT])
{
	if (!IS_ENABLED(CONFIG_ARGISENSE_DAC_POWER_DURING_MEASUREMENT)) {
		LOG_WRN("DAC power during measurement is disabled; skipping GP8302 writes");
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(dac_devices); i++) {
		const struct argisense_current_loop_command *command = &commands[i];
		const struct argisense_dac_device *dac = &dac_devices[command->channel];
		uint32_t raw_value;
		int ret;

		raw_value = argisense_gp8302_raw_from_current(dac,
							      command->current_ua);
		ret = dac_write_value(dac->dev, 0U, raw_value);
		if (ret < 0) {
			LOG_ERR("%s write failed: current=%d uA raw=0x%03x ret=%d",
				dac->name, command->current_ua, raw_value, ret);
			return ret;
		}

		LOG_INF("%s output written: current=%d uA raw=0x%03x",
			dac->name, command->current_ua, raw_value);
	}

	return 0;
}

static int argisense_prepare_outputs(void)
{
	const struct argisense_runtime_config *config = argisense_settings_get();
	const struct argisense_current_loop_limits limits = {
		.dac_min_current_ua = config->dac_min_current_ua,
		.dac_max_current_ua = config->dac_max_current_ua,
		.dac_fault_current_ua = config->dac_fault_current_ua,
		.methane_range_low_ppm = config->methane_dac_range_low_ppm,
		.methane_range_high_ppm = config->methane_dac_range_high_ppm,
		.pressure_range_low_pa = config->pressure_dac_range_low_pa,
		.pressure_range_high_pa = config->pressure_dac_range_high_pa,
	};
	struct argisense_measurement_sample sample = {
		.methane_valid = false,
		.pressure_valid = false,
	};
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT];
	int methane_ret;
	int pressure_ret;
	int humidity_ret;
	int ret;

	methane_ret = argisense_read_methane_sample(&sample);
	if (methane_ret < 0) {
		LOG_WRN("Methane sample unavailable; DAC0/RS485 will report fault");
	}

	pressure_ret = argisense_read_pressure_sample(&sample);
	if (pressure_ret < 0) {
		LOG_WRN("Pressure sample unavailable; DAC1/RS485 will report fault");
	}

	humidity_ret = argisense_read_humidity_sample(&sample);
	if (humidity_ret < 0) {
		LOG_WRN("Humidity sample unavailable; RS485 will report fault");
	}

	argisense_current_loop_prepare_with_limits(&sample, &limits, commands);
	argisense_register_update_sample(&sample, commands);

	for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
		LOG_INF("DAC channel %u maps %s value %d to %d uA",
			(unsigned int)commands[i].channel, commands[i].name,
			commands[i].source_value, commands[i].current_ua);
	}

	ret = argisense_dac_write_outputs(commands);
	if (ret < 0) {
		return ret;
	}

	if (methane_ret < 0) {
		return methane_ret;
	}

	return pressure_ret;
}

static int argisense_boot_confirm_if_ready(void)
{
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	int ret;

	if (boot_is_img_confirmed()) {
		LOG_INF("MCUboot image already confirmed");
		return 0;
	}

	ret = boot_write_img_confirmed();
	if (ret < 0) {
		LOG_ERR("Failed to confirm MCUboot image: %d", ret);
		return ret;
	}

	LOG_INF("MCUboot image confirmed after bring-up checks");
#endif

	return 0;
}

int main(void)
{
	const struct argisense_runtime_config *config;
	int ret;

	LOG_INF("ArgiSense Zephyr 4.4 application started");
	argisense_usb_shell_diagnostics();

	ret = argisense_log_firmware_version();
	if (ret < 0) {
		return ret;
	}

	ret = argisense_settings_init();
	if (ret < 0) {
		LOG_ERR("Persistent settings initialization failed: %d", ret);
		return ret;
	}

	argisense_settings_log_summary();

	ret = argisense_rs485_init();
	if (ret < 0) {
		LOG_ERR("RS485 initialization failed: %d", ret);
		return ret;
	}

	ret = argisense_power_configure();
	if (ret < 0) {
		LOG_ERR("Power manager configuration failed: %d", ret);
		return ret;
	}

	ret = argisense_dac_devices_check();
	if (ret < 0) {
		LOG_ERR("DAC I2C mapping check failed: %d", ret);
		return ret;
	}

	ret = argisense_methane_sensor_check();
	if (ret < 0) {
		LOG_ERR("Methane sensor mapping check failed: %d", ret);
		return ret;
	}

	ret = argisense_humidity_sensor_check();
	if (ret < 0) {
		LOG_ERR("Humidity sensor mapping check failed: %d", ret);
		return ret;
	}

	ret = argisense_external_eeprom_check();
	if (ret < 0) {
		LOG_ERR("External EEPROM mapping check failed: %d", ret);
		return ret;
	}

	ret = argisense_pressure_sensor_check();
	if (ret < 0) {
		LOG_ERR("Pressure sensor mapping check failed: %d", ret);
		return ret;
	}

	argisense_power_idle_state();

	ret = argisense_boot_confirm_if_ready();
	if (ret < 0) {
		return ret;
	}

	config = argisense_settings_get();
	LOG_INF("Power manager ready: period=%us, window=%ums",
		config->measurement_period_seconds, config->measurement_window_ms);
	LOG_INF("HTU21D humidity read period=%us",
		config->humidity_read_period_seconds);
	LOG_INF("+3V3_PRE idle policy: %s",
		IS_ENABLED(CONFIG_ARGISENSE_PRE_RAIL_ALWAYS_ON) ?
		"kept on for Dynament methane sensor" : "powered off");
	LOG_INF("DAC rail idle policy: %s",
		IS_ENABLED(CONFIG_ARGISENSE_DAC_POWER_IDLE) ?
		"kept on for continuous 4-20 mA output" : "powered off");
	LOG_INF("Dual GP8302 output model ready: DAC0=methane, DAC1=pressure");

	while (true) {
		config = argisense_settings_get();
		LOG_INF("Powering sensor rails for measurement");

		argisense_power_measurement_lock();
		ret = argisense_power_measurement_on();
		if (ret == 0) {
			k_msleep((int32_t)config->measurement_window_ms);
			ret = argisense_prepare_outputs();
			if (ret < 0) {
				LOG_ERR("Output refresh failed: %d", ret);
			}
		}

		argisense_power_idle_state();
		argisense_power_measurement_unlock();
		LOG_INF("External switched rails in idle state; entering low-power idle");

		k_sleep(K_SECONDS(config->measurement_period_seconds));
	}
}
