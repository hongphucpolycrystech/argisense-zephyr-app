/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

#include "current_loop_output.h"

LOG_MODULE_REGISTER(argisense, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define DAC0_NODE DT_ALIAS(current_loop_dac0)
#define DAC1_NODE DT_ALIAS(current_loop_dac1)

BUILD_ASSERT(DT_NODE_HAS_STATUS(DAC0_NODE, okay),
	     "Board must define okay current-loop-dac0 alias");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DAC1_NODE, okay),
	     "Board must define okay current-loop-dac1 alias");
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

static const struct gpio_dt_spec dac_power =
	GPIO_DT_SPEC_GET(USER_NODE, dac_power_gpios);
static const struct gpio_dt_spec dac_alarm =
	GPIO_DT_SPEC_GET(USER_NODE, dac_alarm_gpios);
static const struct gpio_dt_spec pre_power =
	GPIO_DT_SPEC_GET(USER_NODE, pre_power_gpios);
static const struct gpio_dt_spec analog_power =
	GPIO_DT_SPEC_GET(USER_NODE, analog_power_gpios);
static const struct gpio_dt_spec rs485_termination =
	GPIO_DT_SPEC_GET(USER_NODE, rs485_termination_gpios);
static const struct gpio_dt_spec pressure_ps =
	GPIO_DT_SPEC_GET(USER_NODE, pressure_ps_gpios);
static const struct gpio_dt_spec pressure_cs =
	GPIO_DT_SPEC_GET(USER_NODE, pressure_cs_gpios);

struct argisense_dac_device {
	const struct i2c_dt_spec i2c;
	const char *name;
};

static const struct argisense_dac_device dac_devices[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT] = {
	[ARGISENSE_CURRENT_LOOP_METHANE] = {
		.i2c = I2C_DT_SPEC_GET(DAC0_NODE),
		.name = "DAC0 methane GP8302",
	},
	[ARGISENSE_CURRENT_LOOP_PRESSURE] = {
		.i2c = I2C_DT_SPEC_GET(DAC1_NODE),
		.name = "DAC1 pressure GP8302",
	},
};

static int configure_output_inactive(const struct gpio_dt_spec *gpio, const char *name)
{
	int ret;

	if (!gpio_is_ready_dt(gpio)) {
		LOG_ERR("%s GPIO controller is not ready", name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure %s: %d", name, ret);
	}

	return ret;
}

static int configure_input_pulldown(const struct gpio_dt_spec *gpio, const char *name)
{
	int ret;

	if (!gpio_is_ready_dt(gpio)) {
		LOG_ERR("%s GPIO controller is not ready", name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(gpio, GPIO_INPUT | GPIO_PULL_DOWN);
	if (ret < 0) {
		LOG_ERR("Failed to configure %s: %d", name, ret);
	}

	return ret;
}

static int set_gpio(const struct gpio_dt_spec *gpio, const char *name, bool active)
{
	int ret = gpio_pin_set_dt(gpio, active ? 1 : 0);

	if (ret < 0) {
		LOG_ERR("Failed to set %s %s: %d", name, active ? "on" : "off", ret);
	}

	return ret;
}

static int argisense_dac_devices_check(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(dac_devices); i++) {
		const struct argisense_dac_device *dac = &dac_devices[i];

		if (!device_is_ready(dac->i2c.bus)) {
			LOG_ERR("%s I2C bus is not ready", dac->name);
			return -ENODEV;
		}

		LOG_INF("%s mapped to %s address 0x%02x", dac->name,
			dac->i2c.bus->name, dac->i2c.addr);
	}

	return 0;
}

static int32_t midpoint(int32_t low, int32_t high)
{
	return low + (int32_t)(((int64_t)high - low) / 2);
}

static void argisense_prepare_output_placeholders(void)
{
	struct argisense_measurement_sample sample = {
		.methane_ppm_x100 =
			midpoint(CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM,
				 CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM) *
			100,
		.pressure_pa =
			midpoint(CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA,
				 CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA),
		.methane_valid = true,
		.pressure_valid = true,
	};
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT];

	argisense_current_loop_prepare(&sample, commands);

	for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
		LOG_INF("DAC channel %u maps %s value %d to %d uA",
			(unsigned int)commands[i].channel, commands[i].name,
			commands[i].source_value, commands[i].current_ua);
	}
}

static int argisense_power_configure(void)
{
	int ret;

	ret = configure_output_inactive(&dac_power, "DAC power");
	if (ret < 0) {
		return ret;
	}

	ret = configure_output_inactive(&pre_power, "3V3_PRE power");
	if (ret < 0) {
		return ret;
	}

	ret = configure_output_inactive(&analog_power, "analog power");
	if (ret < 0) {
		return ret;
	}

	ret = configure_output_inactive(&rs485_termination, "RS485 termination");
	if (ret < 0) {
		return ret;
	}

	ret = configure_output_inactive(&pressure_ps, "pressure PS");
	if (ret < 0) {
		return ret;
	}

	ret = configure_output_inactive(&pressure_cs, "pressure CS");
	if (ret < 0) {
		return ret;
	}

	return configure_input_pulldown(&dac_alarm, "DAC alarm");
}

static void argisense_power_all_off(void)
{
	(void)set_gpio(&rs485_termination, "RS485 termination", false);
	(void)set_gpio(&dac_power, "DAC power", false);
	(void)set_gpio(&analog_power, "analog power", false);
	(void)set_gpio(&pressure_ps, "pressure PS", false);
	(void)set_gpio(&pressure_cs, "pressure CS", false);
	(void)set_gpio(&pre_power, "3V3_PRE power", false);
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

static int argisense_power_measurement_on(void)
{
	int ret;

	ret = set_gpio(&pressure_cs, "pressure CS", false);
	if (ret < 0) {
		return ret;
	}

	ret = set_gpio(&rs485_termination, "RS485 termination", false);
	if (ret < 0) {
		return ret;
	}

	ret = set_gpio(&pre_power, "3V3_PRE power", true);
	if (ret < 0) {
		return ret;
	}

	k_msleep(CONFIG_ARGISENSE_PRE_RAIL_SETTLE_MS);

	ret = set_gpio(&pressure_ps, "pressure PS",
		       IS_ENABLED(CONFIG_ARGISENSE_PRESSURE_PS_ACTIVE));
	if (ret < 0) {
		return ret;
	}

	ret = set_gpio(&analog_power, "analog power", true);
	if (ret < 0) {
		return ret;
	}

	if (IS_ENABLED(CONFIG_ARGISENSE_DAC_POWER_DURING_MEASUREMENT)) {
		ret = set_gpio(&dac_power, "DAC power", true);
		if (ret < 0) {
			return ret;
		}
	}

	k_msleep(CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS);

	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("ArgiSense Zephyr 4.4 application started");

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

	argisense_power_all_off();

	ret = argisense_boot_confirm_if_ready();
	if (ret < 0) {
		return ret;
	}

	LOG_INF("Power manager ready: period=%ds, window=%dms",
		CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS,
		CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS);
	LOG_INF("Dual GP8302 output model ready: DAC0=methane, DAC1=pressure");

	while (true) {
		LOG_INF("Powering sensor rails for measurement");

		ret = argisense_power_measurement_on();
		if (ret == 0) {
			k_msleep(CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS);
			argisense_prepare_output_placeholders();
		}

		argisense_power_all_off();
		LOG_INF("External rails off; entering low-power idle");

		k_sleep(K_SECONDS(CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS));
	}
}
