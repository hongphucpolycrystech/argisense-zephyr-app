/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT argisense_ms5803_05ba

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <argisense/drivers/sensor/ms5803_05ba.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ms5803_05ba, CONFIG_SENSOR_LOG_LEVEL);

#define MS5803_CMD_RESET       0x1e
#define MS5803_CMD_ADC_READ    0x00
#define MS5803_CMD_D1_OSR_4096 0x48
#define MS5803_CMD_D2_OSR_4096 0x58
#define MS5803_CMD_PROM_READ   0xa0

#define MS5803_PROM_WORDS 8U
#define MS5803_RESET_DELAY_MS 3U
#define MS5803_OSR_4096_DELAY_MS 9U

#define MS5803_SPI_OPERATION \
	(SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

struct ms5803_05ba_config {
	struct spi_dt_spec spi;
	uint32_t pressure_range_kpa;
	uint8_t resolution_bits;
};

struct ms5803_05ba_data {
	struct k_mutex lock;
	uint16_t prom[MS5803_PROM_WORDS];
	uint32_t d1_raw;
	uint32_t d2_raw;
	int32_t pressure_pa;
	int32_t temperature_centi_c;
	uint8_t prom_crc_read;
	uint8_t prom_crc_calc;
	bool prom_ready;
	bool prom_crc_available;
	bool sample_ready;
};

static int ms5803_05ba_cmd(const struct device *dev, uint8_t cmd)
{
	const struct ms5803_05ba_config *config = dev->config;
	const struct spi_buf tx = {
		.buf = &cmd,
		.len = sizeof(cmd),
	};
	const struct spi_buf_set tx_set = {
		.buffers = &tx,
		.count = 1U,
	};

	return spi_write_dt(&config->spi, &tx_set);
}

static int ms5803_05ba_reset(const struct device *dev)
{
	int ret;

	ret = ms5803_05ba_cmd(dev, MS5803_CMD_RESET);
	if (ret < 0) {
		return ret;
	}

	k_msleep(MS5803_RESET_DELAY_MS);

	return 0;
}

static int ms5803_05ba_read_prom_word(const struct device *dev, uint8_t index,
				      uint16_t *word)
{
	const struct ms5803_05ba_config *config = dev->config;
	uint8_t tx_buf[3] = { MS5803_CMD_PROM_READ + (index * 2U), 0x00, 0x00 };
	uint8_t rx_buf[3] = { 0 };
	const struct spi_buf tx = {
		.buf = tx_buf,
		.len = sizeof(tx_buf),
	};
	const struct spi_buf rx = {
		.buf = rx_buf,
		.len = sizeof(rx_buf),
	};
	const struct spi_buf_set tx_set = {
		.buffers = &tx,
		.count = 1U,
	};
	const struct spi_buf_set rx_set = {
		.buffers = &rx,
		.count = 1U,
	};
	int ret;

	ret = spi_transceive_dt(&config->spi, &tx_set, &rx_set);
	if (ret < 0) {
		return ret;
	}

	*word = ((uint16_t)rx_buf[1] << 8) | rx_buf[2];

	return 0;
}

static uint8_t ms5803_05ba_crc4(const uint16_t prom[MS5803_PROM_WORDS])
{
	uint16_t scratch[MS5803_PROM_WORDS];
	uint16_t n_rem = 0U;

	memcpy(scratch, prom, sizeof(scratch));
	scratch[7] &= 0xff00U;

	for (uint8_t cnt = 0U; cnt < 16U; cnt++) {
		if ((cnt & 1U) != 0U) {
			n_rem ^= scratch[cnt >> 1] & 0x00ffU;
		} else {
			n_rem ^= scratch[cnt >> 1] >> 8;
		}

		for (uint8_t bit = 0U; bit < 8U; bit++) {
			if ((n_rem & 0x8000U) != 0U) {
				n_rem = (uint16_t)((n_rem << 1) ^ 0x3000U);
			} else {
				n_rem <<= 1;
			}
		}
	}

	return (uint8_t)((n_rem >> 12) & 0x000fU);
}

static int ms5803_05ba_read_prom(const struct device *dev)
{
	struct ms5803_05ba_data *data = dev->data;
	uint8_t crc_read;
	uint8_t crc_calc;
	int ret;

	ret = ms5803_05ba_reset(dev);
	if (ret < 0) {
		LOG_ERR("Reset failed: %d", ret);
		return ret;
	}

	for (uint8_t i = 0U; i < MS5803_PROM_WORDS; i++) {
		ret = ms5803_05ba_read_prom_word(dev, i, &data->prom[i]);
		if (ret < 0) {
			LOG_ERR("PROM word %u read failed: %d", i, ret);
			return ret;
		}
	}

	for (uint8_t i = 1U; i <= 6U; i++) {
		if (data->prom[i] == 0U) {
			LOG_ERR("PROM calibration coefficient C%u is zero", i);
			return -EIO;
		}
	}

	crc_read = data->prom[7] & 0x000fU;
	crc_calc = ms5803_05ba_crc4(data->prom);
	data->prom_crc_read = crc_read;
	data->prom_crc_calc = crc_calc;
	data->prom_crc_available = true;
	if (crc_read != crc_calc) {
		LOG_ERR("PROM CRC mismatch: read=0x%x calculated=0x%x",
			crc_read, crc_calc);
		return -EIO;
	}

	data->prom_ready = true;
	LOG_INF("PROM calibration ready: C1=%u C2=%u C3=%u C4=%u C5=%u C6=%u",
		data->prom[1], data->prom[2], data->prom[3], data->prom[4],
		data->prom[5], data->prom[6]);

	return 0;
}

static int ms5803_05ba_ensure_prom_ready(const struct device *dev)
{
	struct ms5803_05ba_data *data = dev->data;

	if (data->prom_ready) {
		return 0;
	}

	return ms5803_05ba_read_prom(dev);
}

static int ms5803_05ba_read_adc(const struct device *dev, uint32_t *adc)
{
	const struct ms5803_05ba_config *config = dev->config;
	uint8_t tx_buf[4] = { MS5803_CMD_ADC_READ, 0x00, 0x00, 0x00 };
	uint8_t rx_buf[4] = { 0 };
	const struct spi_buf tx = {
		.buf = tx_buf,
		.len = sizeof(tx_buf),
	};
	const struct spi_buf rx = {
		.buf = rx_buf,
		.len = sizeof(rx_buf),
	};
	const struct spi_buf_set tx_set = {
		.buffers = &tx,
		.count = 1U,
	};
	const struct spi_buf_set rx_set = {
		.buffers = &rx,
		.count = 1U,
	};
	int ret;

	ret = spi_transceive_dt(&config->spi, &tx_set, &rx_set);
	if (ret < 0) {
		return ret;
	}

	*adc = ((uint32_t)rx_buf[1] << 16) | ((uint32_t)rx_buf[2] << 8) |
	       rx_buf[3];
	if (*adc == 0U) {
		return -EIO;
	}

	return 0;
}

static int ms5803_05ba_convert(const struct device *dev, uint8_t cmd,
			       uint32_t *adc)
{
	int ret;

	ret = ms5803_05ba_cmd(dev, cmd);
	if (ret < 0) {
		return ret;
	}

	k_msleep(MS5803_OSR_4096_DELAY_MS);

	return ms5803_05ba_read_adc(dev, adc);
}

static void ms5803_05ba_compensate(struct ms5803_05ba_data *data)
{
	const int64_t c1 = data->prom[1];
	const int64_t c2 = data->prom[2];
	const int64_t c3 = data->prom[3];
	const int64_t c4 = data->prom[4];
	const int64_t c5 = data->prom[5];
	const int64_t c6 = data->prom[6];
	const int64_t d1 = data->d1_raw;
	const int64_t d2 = data->d2_raw;
	int64_t dt;
	int64_t temp;
	int64_t off;
	int64_t sens;
	int64_t t2 = 0;
	int64_t off2 = 0;
	int64_t sens2 = 0;
	int64_t delta_temp;
	int64_t pressure;

	dt = d2 - (c5 << 8);
	temp = 2000 + ((dt * c6) / 8388608LL);
	off = (c2 * 262144LL) + ((c4 * dt) / 32LL);
	sens = (c1 * 131072LL) + ((c3 * dt) / 128LL);

	if (temp < 2000) {
		delta_temp = temp - 2000;
		t2 = (3 * dt * dt) / 8589934592LL;
		off2 = (3 * delta_temp * delta_temp) / 8LL;
		sens2 = (7 * delta_temp * delta_temp) / 8LL;

		if (temp < -1500) {
			delta_temp = temp + 1500;
			sens2 += 3 * delta_temp * delta_temp;
		}
	}

	temp -= t2;
	off -= off2;
	sens -= sens2;

	pressure = (((d1 * sens) / 2097152LL) - off) / 32768LL;

	data->temperature_centi_c = (int32_t)temp;
	data->pressure_pa = (int32_t)pressure;
}

static int ms5803_05ba_sample_fetch(const struct device *dev,
				    enum sensor_channel chan)
{
	struct ms5803_05ba_data *data = dev->data;
	int ret;

	if ((int)chan != SENSOR_CHAN_ALL && (int)chan != SENSOR_CHAN_PRESS &&
	    (int)chan != SENSOR_CHAN_AMBIENT_TEMP &&
	    (int)chan != ARGISENSE_MS5803_SENSOR_CHAN_D1_RAW &&
	    (int)chan != ARGISENSE_MS5803_SENSOR_CHAN_D2_RAW &&
	    (int)chan != ARGISENSE_MS5803_SENSOR_CHAN_PROM_CRC_READ &&
	    (int)chan != ARGISENSE_MS5803_SENSOR_CHAN_PROM_CRC_CALC) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ms5803_05ba_ensure_prom_ready(dev);
	if (ret < 0) {
		goto out;
	}

	ret = ms5803_05ba_convert(dev, MS5803_CMD_D1_OSR_4096,
				  &data->d1_raw);
	if (ret < 0) {
		LOG_ERR("D1 conversion failed: %d", ret);
		data->sample_ready = false;
		goto out;
	}

	ret = ms5803_05ba_convert(dev, MS5803_CMD_D2_OSR_4096,
				  &data->d2_raw);
	if (ret < 0) {
		LOG_ERR("D2 conversion failed: %d", ret);
		data->sample_ready = false;
		goto out;
	}

	ms5803_05ba_compensate(data);
	data->sample_ready = true;

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int ms5803_05ba_channel_get(const struct device *dev,
				   enum sensor_channel chan,
				   struct sensor_value *val)
{
	struct ms5803_05ba_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	switch ((int)chan) {
	case SENSOR_CHAN_PRESS:
		if (!data->sample_ready) {
			ret = -ENODATA;
			break;
		}
		ret = sensor_value_from_micro(val,
					      (int64_t)data->pressure_pa * 1000);
		break;
	case SENSOR_CHAN_AMBIENT_TEMP:
		if (!data->sample_ready) {
			ret = -ENODATA;
			break;
		}
		ret = sensor_value_from_micro(
			val, (int64_t)data->temperature_centi_c * 10000);
		break;
	case ARGISENSE_MS5803_SENSOR_CHAN_D1_RAW:
		if (!data->sample_ready) {
			ret = -ENODATA;
			break;
		}
		val->val1 = data->d1_raw;
		val->val2 = 0;
		break;
	case ARGISENSE_MS5803_SENSOR_CHAN_D2_RAW:
		if (!data->sample_ready) {
			ret = -ENODATA;
			break;
		}
		val->val1 = data->d2_raw;
		val->val2 = 0;
		break;
	case ARGISENSE_MS5803_SENSOR_CHAN_PROM_CRC_READ:
		if (!data->prom_crc_available) {
			ret = -ENODATA;
			break;
		}
		val->val1 = data->prom_crc_read;
		val->val2 = 0;
		break;
	case ARGISENSE_MS5803_SENSOR_CHAN_PROM_CRC_CALC:
		if (!data->prom_crc_available) {
			ret = -ENODATA;
			break;
		}
		val->val1 = data->prom_crc_calc;
		val->val2 = 0;
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static int ms5803_05ba_init(const struct device *dev)
{
	struct ms5803_05ba_data *data = dev->data;
	const struct ms5803_05ba_config *config = dev->config;

	k_mutex_init(&data->lock);

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI bus is not ready");
		return -ENODEV;
	}

	if (config->pressure_range_kpa != 500U) {
		LOG_WRN("Unexpected pressure range %u kPa for MS5803-05BA",
			config->pressure_range_kpa);
	}

	if (config->resolution_bits != 24U) {
		LOG_ERR("Unsupported ADC resolution %u", config->resolution_bits);
		return -ENOTSUP;
	}

	return 0;
}

static DEVICE_API(sensor, ms5803_05ba_api) = {
	.sample_fetch = ms5803_05ba_sample_fetch,
	.channel_get = ms5803_05ba_channel_get,
};

#define MS5803_05BA_DEFINE(inst)                                             \
	static struct ms5803_05ba_data ms5803_05ba_data_##inst;              \
	static const struct ms5803_05ba_config ms5803_05ba_config_##inst = { \
		.spi = SPI_DT_SPEC_INST_GET(inst, MS5803_SPI_OPERATION),     \
		.pressure_range_kpa = DT_INST_PROP(inst, pressure_range_kpa), \
		.resolution_bits = DT_INST_PROP(inst, resolution_bits),      \
	};                                                                    \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, ms5803_05ba_init, NULL,            \
				     &ms5803_05ba_data_##inst,               \
				     &ms5803_05ba_config_##inst, POST_KERNEL, \
				     CONFIG_SENSOR_INIT_PRIORITY,             \
				     &ms5803_05ba_api);

DT_INST_FOREACH_STATUS_OKAY(MS5803_05BA_DEFINE)
