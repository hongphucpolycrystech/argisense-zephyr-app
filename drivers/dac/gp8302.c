/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT argisense_gp8302

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gp8302, CONFIG_DAC_LOG_LEVEL);

#define GP8302_MAX_RAW_VALUE 0x0FFFU

struct gp8302_config {
	struct i2c_dt_spec i2c;
	uint8_t resolution_bits;
	uint8_t output_register;
};

static int gp8302_channel_setup(const struct device *dev,
				const struct dac_channel_cfg *channel_cfg)
{
	const struct gp8302_config *config = dev->config;

	if (channel_cfg->channel_id != 0U) {
		LOG_ERR("Unsupported channel %u", channel_cfg->channel_id);
		return -ENOTSUP;
	}

	if (channel_cfg->resolution != config->resolution_bits) {
		LOG_ERR("Unsupported resolution %u; expected %u",
			channel_cfg->resolution, config->resolution_bits);
		return -ENOTSUP;
	}

	if (channel_cfg->internal) {
		LOG_ERR("Internal output path is not supported");
		return -ENOTSUP;
	}

	return 0;
}

static int gp8302_write_value(const struct device *dev, uint8_t channel,
			      uint32_t value)
{
	const struct gp8302_config *config = dev->config;
	uint8_t tx_buf[3];

	if (channel != 0U) {
		LOG_ERR("Unsupported channel %u", channel);
		return -ENOTSUP;
	}

	if (value > GP8302_MAX_RAW_VALUE) {
		LOG_ERR("Raw value %u out of range", value);
		return -EINVAL;
	}

	tx_buf[0] = config->output_register;
	tx_buf[1] = (uint8_t)((value << 4) & 0xF0U);
	tx_buf[2] = (uint8_t)((value >> 4) & 0xFFU);

	return i2c_write_dt(&config->i2c, tx_buf, sizeof(tx_buf));
}

static int gp8302_init(const struct device *dev)
{
	const struct gp8302_config *config = dev->config;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C bus %s is not ready", config->i2c.bus->name);
		return -ENODEV;
	}

	if (config->resolution_bits != 12U) {
		LOG_ERR("Unsupported configured resolution %u",
			config->resolution_bits);
		return -ENOTSUP;
	}

	return 0;
}

static DEVICE_API(dac, gp8302_driver_api) = {
	.channel_setup = gp8302_channel_setup,
	.write_value = gp8302_write_value,
};

#define GP8302_DEFINE(inst)                                                \
	static const struct gp8302_config gp8302_config_##inst = {         \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                         \
		.resolution_bits = DT_INST_PROP(inst, resolution_bits),     \
		.output_register = DT_INST_PROP(inst, output_register),     \
	};                                                                 \
	DEVICE_DT_INST_DEFINE(inst, gp8302_init, NULL, NULL,               \
			      &gp8302_config_##inst, POST_KERNEL,          \
			      CONFIG_DAC_INIT_PRIORITY, &gp8302_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GP8302_DEFINE)
