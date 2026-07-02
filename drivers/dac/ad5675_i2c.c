/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT argisense_ad5675_i2c

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ad5675_i2c, CONFIG_DAC_LOG_LEVEL);

#define AD5675_CHANNEL_COUNT 8U
#define AD5675_RESOLUTION_BITS 16U
#define AD5675_CMD_WRITE_INPUT 0x10U
#define AD5675_CMD_UPDATE_DAC 0x20U
#define AD5675_CMD_WRITE_UPDATE 0x30U
#define AD5675_CMD_GAIN_SETUP 0x70U
#define AD5675_GAIN_2_BIT BIT(2)

struct ad5675_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec ldac;
	uint8_t resolution_bits;
	uint8_t gain;
};

struct ad5675_data {
	uint8_t configured_channels;
};

static int ad5675_write_command(const struct device *dev, uint8_t command,
				uint8_t channel, uint16_t value)
{
	const struct ad5675_config *config = dev->config;
	uint8_t tx_buf[3];

	if (channel >= AD5675_CHANNEL_COUNT) {
		return -EINVAL;
	}

	tx_buf[0] = command | (channel & 0x0FU);
	sys_put_be16(value, &tx_buf[1]);

	return i2c_write_dt(&config->i2c, tx_buf, sizeof(tx_buf));
}

static int ad5675_channel_setup(const struct device *dev,
				const struct dac_channel_cfg *channel_cfg)
{
	const struct ad5675_config *config = dev->config;
	struct ad5675_data *data = dev->data;

	if (channel_cfg == NULL || channel_cfg->channel_id >= AD5675_CHANNEL_COUNT) {
		return -EINVAL;
	}

	if (channel_cfg->resolution != config->resolution_bits) {
		LOG_ERR("Unsupported resolution %u; expected %u",
			channel_cfg->resolution, config->resolution_bits);
		return -ENOTSUP;
	}

	if (channel_cfg->internal) {
		return -ENOTSUP;
	}

	data->configured_channels |= BIT(channel_cfg->channel_id);
	return 0;
}

static int ad5675_write_value(const struct device *dev, uint8_t channel,
			      uint32_t value)
{
	const struct ad5675_config *config = dev->config;
	struct ad5675_data *data = dev->data;

	if (channel >= AD5675_CHANNEL_COUNT) {
		return -EINVAL;
	}

	if ((data->configured_channels & BIT(channel)) == 0U) {
		return -EINVAL;
	}

	if (value > UINT16_MAX || config->resolution_bits != AD5675_RESOLUTION_BITS) {
		return -EINVAL;
	}

	return ad5675_write_command(dev, AD5675_CMD_WRITE_UPDATE, channel,
				    (uint16_t)value);
}

static int ad5675_hw_reset(const struct device *dev)
{
	const struct ad5675_config *config = dev->config;
	int ret;

	if (config->reset.port == NULL) {
		return 0;
	}

	if (!gpio_is_ready_dt(&config->reset)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&config->reset, 1);
	if (ret < 0) {
		return ret;
	}

	k_usleep(10);

	ret = gpio_pin_set_dt(&config->reset, 0);
	if (ret < 0) {
		return ret;
	}

	k_msleep(1);
	return 0;
}

static int ad5675_configure_ldac(const struct device *dev)
{
	const struct ad5675_config *config = dev->config;

	if (config->ldac.port == NULL) {
		return 0;
	}

	if (!gpio_is_ready_dt(&config->ldac)) {
		return -ENODEV;
	}

	/*
	 * The driver uses the AD5675 write-and-update command, so LDAC does not
	 * need to be pulsed. Keep a routed LDAC pin inactive for predictable
	 * bring-up and future simultaneous-update support.
	 */
	return gpio_pin_configure_dt(&config->ldac, GPIO_OUTPUT_INACTIVE);
}

static int ad5675_set_gain(const struct device *dev)
{
	const struct ad5675_config *config = dev->config;
	uint16_t value;

	if (config->gain != 1U && config->gain != 2U) {
		return -EINVAL;
	}

	value = config->gain == 2U ? AD5675_GAIN_2_BIT : 0U;
	return ad5675_write_command(dev, AD5675_CMD_GAIN_SETUP, 0U, value);
}

static int ad5675_init(const struct device *dev)
{
	const struct ad5675_config *config = dev->config;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C bus %s is not ready", config->i2c.bus->name);
		return -ENODEV;
	}

	ret = ad5675_configure_ldac(dev);
	if (ret < 0) {
		LOG_ERR("LDAC GPIO configuration failed: %d", ret);
		return ret;
	}

	ret = ad5675_hw_reset(dev);
	if (ret < 0) {
		LOG_ERR("Hardware reset failed: %d", ret);
		return ret;
	}

	ret = ad5675_set_gain(dev);
	if (ret < 0) {
		LOG_ERR("Gain setup failed: %d", ret);
		return ret;
	}

	LOG_INF("AD5675 ready on %s address 0x%02x gain=%u ldac-gpio=%s",
		config->i2c.bus->name, config->i2c.addr, config->gain,
		config->ldac.port != NULL ? "yes" : "no");

	return 0;
}

static DEVICE_API(dac, ad5675_driver_api) = {
	.channel_setup = ad5675_channel_setup,
	.write_value = ad5675_write_value,
};

#define AD5675_DEFINE(inst)                                                \
	static const struct ad5675_config ad5675_config_##inst = {        \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                         \
		.reset = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}), \
		.ldac = GPIO_DT_SPEC_INST_GET_OR(inst, ldac_gpios, {0}),   \
		.resolution_bits = DT_INST_PROP(inst, resolution_bits),     \
		.gain = DT_INST_PROP(inst, gain),                           \
	};                                                                 \
	static struct ad5675_data ad5675_data_##inst;                      \
	DEVICE_DT_INST_DEFINE(inst, ad5675_init, NULL,                    \
			      &ad5675_data_##inst, &ad5675_config_##inst, \
			      POST_KERNEL, CONFIG_DAC_INIT_PRIORITY,      \
			      &ad5675_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AD5675_DEFINE)
