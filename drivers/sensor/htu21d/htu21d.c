/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT argisense_htu21d

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(htu21d, CONFIG_SENSOR_LOG_LEVEL);

#define HTU21D_CMD_MEASURE_TEMP_NO_HOLD     0xf3
#define HTU21D_CMD_MEASURE_HUMIDITY_NO_HOLD 0xf5
#define HTU21D_CMD_SOFT_RESET               0xfe

#define HTU21D_SOFT_RESET_DELAY_MS       15U
#define HTU21D_TEMP_CONVERSION_DELAY_MS  50U
#define HTU21D_RH_CONVERSION_DELAY_MS    16U
#define HTU21D_RAW_STATUS_MASK           0x0003U
#define HTU21D_CRC_POLYNOMIAL            0x31U

struct htu21d_config {
	struct i2c_dt_spec i2c;
	const char *part_number;
};

struct htu21d_data {
	struct k_mutex lock;
	int32_t humidity_rh_x100;
	int32_t temperature_centi_c;
	bool sample_ready;
};

static uint8_t htu21d_crc8(const uint8_t *buf, size_t len)
{
	uint8_t crc = 0U;

	for (size_t i = 0U; i < len; i++) {
		crc ^= buf[i];

		for (uint8_t bit = 0U; bit < 8U; bit++) {
			if ((crc & 0x80U) != 0U) {
				crc = (uint8_t)((crc << 1) ^ HTU21D_CRC_POLYNOMIAL);
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

static int htu21d_write_command(const struct device *dev, uint8_t command)
{
	const struct htu21d_config *config = dev->config;

	return i2c_write_dt(&config->i2c, &command, sizeof(command));
}

static int htu21d_read_raw(const struct device *dev, uint8_t command,
			   uint32_t delay_ms, uint16_t *raw)
{
	const struct htu21d_config *config = dev->config;
	uint8_t buf[3];
	int ret;

	ret = htu21d_write_command(dev, command);
	if (ret < 0) {
		return ret;
	}

	k_msleep(delay_ms);

	ret = i2c_read_dt(&config->i2c, buf, sizeof(buf));
	if (ret < 0) {
		return ret;
	}

	if (htu21d_crc8(buf, 2U) != buf[2]) {
		LOG_ERR("CRC mismatch for command 0x%02x", command);
		return -EIO;
	}

	*raw = (((uint16_t)buf[0] << 8) | buf[1]) & ~HTU21D_RAW_STATUS_MASK;

	return 0;
}

static int32_t htu21d_humidity_raw_to_rh_x100(uint16_t raw)
{
	int64_t rh_x100 = -600LL + ((12500LL * raw) / 65536LL);

	return (int32_t)CLAMP(rh_x100, 0LL, 10000LL);
}

static int32_t htu21d_temperature_raw_to_centi_c(uint16_t raw)
{
	return (int32_t)(-4685LL + ((17572LL * raw) / 65536LL));
}

static int htu21d_sample_fetch(const struct device *dev,
			       enum sensor_channel chan)
{
	struct htu21d_data *data = dev->data;
	uint16_t raw_humidity;
	uint16_t raw_temperature;
	int ret;

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_HUMIDITY &&
	    chan != SENSOR_CHAN_AMBIENT_TEMP) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_HUMIDITY) {
		ret = htu21d_read_raw(dev, HTU21D_CMD_MEASURE_HUMIDITY_NO_HOLD,
				      HTU21D_RH_CONVERSION_DELAY_MS,
				      &raw_humidity);
		if (ret < 0) {
			data->sample_ready = false;
			goto out;
		}

		data->humidity_rh_x100 =
			htu21d_humidity_raw_to_rh_x100(raw_humidity);
	}

	if (chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_AMBIENT_TEMP) {
		ret = htu21d_read_raw(dev, HTU21D_CMD_MEASURE_TEMP_NO_HOLD,
				      HTU21D_TEMP_CONVERSION_DELAY_MS,
				      &raw_temperature);
		if (ret < 0) {
			data->sample_ready = false;
			goto out;
		}

		data->temperature_centi_c =
			htu21d_temperature_raw_to_centi_c(raw_temperature);
	}

	data->sample_ready = true;
	ret = 0;

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int htu21d_channel_get(const struct device *dev,
			      enum sensor_channel chan,
			      struct sensor_value *val)
{
	struct htu21d_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->sample_ready) {
		ret = -ENODATA;
		goto out;
	}

	switch (chan) {
	case SENSOR_CHAN_HUMIDITY:
		ret = sensor_value_from_micro(
			val, (int64_t)data->humidity_rh_x100 * 10000LL);
		break;
	case SENSOR_CHAN_AMBIENT_TEMP:
		ret = sensor_value_from_micro(
			val, (int64_t)data->temperature_centi_c * 10000LL);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int htu21d_init(const struct device *dev)
{
	struct htu21d_data *data = dev->data;
	const struct htu21d_config *config = dev->config;
	int ret;

	k_mutex_init(&data->lock);

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C bus is not ready");
		return -ENODEV;
	}

	ret = htu21d_write_command(dev, HTU21D_CMD_SOFT_RESET);
	if (ret < 0) {
		LOG_ERR("Soft reset failed: %d", ret);
		return ret;
	}

	k_msleep(HTU21D_SOFT_RESET_DELAY_MS);

	LOG_INF("%s sensor ready on %s address 0x%02x", config->part_number,
		config->i2c.bus->name, config->i2c.addr);

	return 0;
}

static DEVICE_API(sensor, htu21d_api) = {
	.sample_fetch = htu21d_sample_fetch,
	.channel_get = htu21d_channel_get,
};

#define HTU21D_DEFINE(inst)                                                \
	static struct htu21d_data htu21d_data_##inst;                      \
	static const struct htu21d_config htu21d_config_##inst = {         \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                         \
		.part_number = DT_INST_PROP(inst, part_number),            \
	};                                                                 \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, htu21d_init, NULL,              \
				     &htu21d_data_##inst,                 \
				     &htu21d_config_##inst, POST_KERNEL,  \
				     CONFIG_SENSOR_INIT_PRIORITY,         \
				     &htu21d_api);

DT_INST_FOREACH_STATUS_OKAY(HTU21D_DEFINE)
