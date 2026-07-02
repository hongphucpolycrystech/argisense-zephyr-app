/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/util.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

#include "argisense_registers.h"
#include "argisense_rs485.h"
#include "argisense_settings.h"
#include "current_loop_output.h"
#include "ph_measurement.h"

LOG_MODULE_REGISTER(argisense_ph, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define PH_ADC_NODE DT_ALIAS(ph_adc)
#define PH_BIAS_DAC_NODE DT_ALIAS(ph_bias_dac)
#define PH_ANALOG_RAIL_NODE DT_ALIAS(ph_analog_rail)
#define GP8302_DAC0_NODE DT_ALIAS(current_loop_dac0)
#define GP8302_DAC1_NODE DT_ALIAS(current_loop_dac1)
#define EEPROM_NODE DT_ALIAS(eeprom0)

BUILD_ASSERT(DT_NODE_HAS_STATUS(PH_ADC_NODE, okay),
	     "pH board must define okay ph-adc alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(PH_BIAS_DAC_NODE, okay),
	     "pH board must define okay ph-bias-dac alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(GP8302_DAC0_NODE, okay),
	     "pH board must define okay current-loop-dac0 alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(GP8302_DAC1_NODE, okay),
	     "pH board must define okay current-loop-dac1 alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(EEPROM_NODE, okay),
	     "pH board must define okay eeprom0 alias");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, adc_power_gpios),
	     "zephyr,user must define adc-power-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, electrode_zero_gpios),
	     "zephyr,user must define electrode-zero-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, rs485_termination_gpios),
	     "zephyr,user must define rs485-termination-gpios");

static const struct device *const ph_adc = DEVICE_DT_GET(PH_ADC_NODE);
static const struct device *const ph_adc_spi = DEVICE_DT_GET(DT_BUS(PH_ADC_NODE));
static const struct device *const ph_bias_dac = DEVICE_DT_GET(PH_BIAS_DAC_NODE);
static const struct device *const ph_bias_i2c =
	DEVICE_DT_GET(DT_BUS(PH_BIAS_DAC_NODE));
static const struct device *const current_loop0_i2c =
	DEVICE_DT_GET(DT_BUS(GP8302_DAC0_NODE));
static const struct device *const current_loop1_i2c =
	DEVICE_DT_GET(DT_BUS(GP8302_DAC1_NODE));

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
			LOG_WRN("USB CDC MCUmgr DTR read failed: %d",
				mcumgr_ret);
		}
	}
#else
	LOG_INF("USB CDC MCUmgr update UART is not selected in DTS");
#endif
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

static void argisense_update_measurement_sample(void)
{
	struct argisense_runtime_config config;
	struct argisense_ph_measurement_sample sample;

	argisense_settings_get_copy(&config);
	ph_measurement_sample(&sample);

	if (current_loop_output_update(&config, &sample) < 0) {
		LOG_WRN("pH current-loop output update failed");
	}

	argisense_register_update_sample(&sample);
}

int main(void)
{
	struct argisense_runtime_config config;
	int ret;

	LOG_INF("ArgiSense pH Zephyr application started");
	argisense_usb_shell_diagnostics();

	ret = argisense_log_firmware_version();
	if (ret < 0) {
		return ret;
	}

	ret = argisense_settings_init();
	if (ret < 0) {
		LOG_ERR("Persistent pH settings initialization failed: %d", ret);
		return ret;
	}

	argisense_settings_log_summary();

	ret = argisense_register_apply_runtime_outputs();
	if (ret < 0) {
		LOG_WRN("pH runtime output apply failed: %d", ret);
	}

	ret = argisense_rs485_init();
	if (ret < 0) {
		LOG_ERR("pH RS485 initialization failed: %d", ret);
		return ret;
	}

	ret = ph_measurement_init();
	if (ret < 0) {
		LOG_WRN("pH measurement initialization reported a hardware issue: %d",
			ret);
	}

	ret = current_loop_output_init();
	if (ret < 0) {
		LOG_WRN("pH current-loop initialization reported a hardware issue: %d",
			ret);
	}

	LOG_INF("Variant %s",
		DT_PROP_OR(USER_NODE, product_variant, "unknown"));
	LOG_INF("pH ADC %s on %s CS%u",
		DT_PROP_OR(USER_NODE, ph_adc_part_number, "ADS124S08"),
		ph_adc_spi->name, DT_REG_ADDR(PH_ADC_NODE));
	LOG_INF("pH bias DAC %s on %s address 0x%02x",
		DT_PROP_OR(USER_NODE, ph_bias_dac_part_number, "AD5675"),
		ph_bias_i2c->name, DT_REG_ADDR(PH_BIAS_DAC_NODE));
	LOG_INF("pH ADC SPI bus ready=%s, bias DAC I2C bus ready=%s",
		device_is_ready(ph_adc_spi) ? "yes" : "no",
		device_is_ready(ph_bias_i2c) ? "yes" : "no");
	LOG_INF("pH ADC device ready=%s, bias DAC device ready=%s",
		device_is_ready(ph_adc) ? "yes" : "no",
		device_is_ready(ph_bias_dac) ? "yes" : "no");
	LOG_INF("Current-loop DAC0 bus=%s DAC1 bus=%s",
		current_loop0_i2c->name, current_loop1_i2c->name);

#if DT_NODE_HAS_STATUS(PH_ANALOG_RAIL_NODE, okay)
	LOG_INF("pH analog rail controller enabled at address 0x%02x",
		DT_REG_ADDR(PH_ANALOG_RAIL_NODE));
#else
	LOG_INF("pH analog rail controller DTS node is present but disabled");
#endif

	argisense_update_measurement_sample();
	argisense_settings_get_copy(&config);
	LOG_INF("pH RS485 service ready: register map v%u, DFU window starts at 1000 when MCUboot DFU is enabled",
		ARGISENSE_REGISTER_MAP_VERSION);
	LOG_INF("pH USB-C service: use usbconsole for shell or usbupdate for shell + MCUmgr update");
	LOG_INF("pH measurement service active: ADS124S08 raw acquisition + AD5675 initial bias + GP8302 outputs DAC0=pH DAC1=temperature");
	LOG_INF("Measurement timing: period=%us window=%ums",
		config.measurement_period_seconds, config.measurement_window_ms);

	while (true) {
		argisense_settings_get_copy(&config);
		argisense_update_measurement_sample();
		k_sleep(K_SECONDS(config.measurement_period_seconds));
	}

	return 0;
}
