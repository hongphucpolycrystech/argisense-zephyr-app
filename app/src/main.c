/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(argisense, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

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

	argisense_power_all_off();

	LOG_INF("Power manager ready: period=%ds, window=%dms",
		CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS,
		CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS);

	while (true) {
		LOG_INF("Powering sensor rails for measurement");

		ret = argisense_power_measurement_on();
		if (ret == 0) {
			k_msleep(CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS);
		}

		argisense_power_all_off();
		LOG_INF("External rails off; entering low-power idle");

		k_sleep(K_SECONDS(CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS));
	}
}
