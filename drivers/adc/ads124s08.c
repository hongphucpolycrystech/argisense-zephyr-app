/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT argisense_ads124s08

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <argisense/drivers/adc/ads124s08.h>

LOG_MODULE_REGISTER(ads124s08, CONFIG_ADC_LOG_LEVEL);

#define ADS124S08_CMD_NOP    0x00U
#define ADS124S08_CMD_RESET  0x06U
#define ADS124S08_CMD_START  0x08U
#define ADS124S08_CMD_STOP   0x0AU
#define ADS124S08_CMD_RDATA  0x12U
#define ADS124S08_CMD_RREG   0x20U
#define ADS124S08_CMD_WREG   0x40U

#define ADS124S08_REG_STATUS   0x01U
#define ADS124S08_REG_INPMUX   0x02U
#define ADS124S08_REG_PGA      0x03U
#define ADS124S08_REG_DATARATE 0x04U
#define ADS124S08_REG_REF      0x05U
#define ADS124S08_REG_IDACMAG  0x06U
#define ADS124S08_REG_IDACMUX  0x07U

#define ADS124S08_INPMUX(_pos, _neg) (((_pos) << 4) | ((_neg) & 0x0FU))
#define ADS124S08_PGA_GAIN_1_BYPASS 0x00U
#define ADS124S08_DATARATE_SINGLE_SHOT_20SPS_LOW_LATENCY 0x34U
#define ADS124S08_REF_EXTERNAL0_REFN_BUF_DISABLED 0x10U
#define ADS124S08_IDAC_OFF 0x00U
#define ADS124S08_IDACMUX_DISCONNECTED 0xFFU
#define ADS124S08_FULL_SCALE_CODE BIT(23)

struct ads124s08_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec drdy;
	struct gpio_dt_spec start_sync;
	uint32_t reference_mv;
	uint16_t idac_current_ua;
	uint16_t conversion_timeout_ms;
};

struct ads124s08_data {
	struct adc_channel_cfg channel_cfg;
	bool channel_configured;
};

static int ads124s08_send_command(const struct device *dev, uint8_t command)
{
	const struct ads124s08_config *config = dev->config;

	return spi_write_dt(&config->spi, &(const struct spi_buf_set){
		.buffers = &(const struct spi_buf){
			.buf = (void *)&command,
			.len = sizeof(command),
		},
		.count = 1,
	});
}

static int ads124s08_read_register(const struct device *dev, uint8_t reg,
				   uint8_t *value)
{
	const struct ads124s08_config *config = dev->config;
	uint8_t tx_buf[3] = {
		ADS124S08_CMD_RREG | (reg & 0x1FU),
		0x00U,
		ADS124S08_CMD_NOP,
	};
	uint8_t rx_buf[sizeof(tx_buf)] = {0};
	const struct spi_buf tx_buffers[] = {
		{
			.buf = tx_buf,
			.len = sizeof(tx_buf),
		},
	};
	const struct spi_buf rx_buffers[] = {
		{
			.buf = rx_buf,
			.len = sizeof(rx_buf),
		},
	};
	const struct spi_buf_set tx = {
		.buffers = tx_buffers,
		.count = ARRAY_SIZE(tx_buffers),
	};
	const struct spi_buf_set rx = {
		.buffers = rx_buffers,
		.count = ARRAY_SIZE(rx_buffers),
	};
	int ret;

	if (value == NULL) {
		return -EINVAL;
	}

	ret = spi_transceive_dt(&config->spi, &tx, &rx);
	if (ret < 0) {
		return ret;
	}

	*value = rx_buf[2];
	return 0;
}

static int ads124s08_write_registers(const struct device *dev,
				     uint8_t start_reg, const uint8_t *values,
				     size_t count)
{
	const struct ads124s08_config *config = dev->config;
	uint8_t tx_buf[2 + 7];
	const struct spi_buf tx_buffers[] = {
		{
			.buf = tx_buf,
			.len = 2U + count,
		},
	};
	const struct spi_buf_set tx = {
		.buffers = tx_buffers,
		.count = ARRAY_SIZE(tx_buffers),
	};

	if (values == NULL || count == 0U || count > 7U) {
		return -EINVAL;
	}

	tx_buf[0] = ADS124S08_CMD_WREG | (start_reg & 0x1FU);
	tx_buf[1] = (uint8_t)(count - 1U);
	memcpy(&tx_buf[2], values, count);

	return spi_write_dt(&config->spi, &tx);
}

static bool ads124s08_input_valid(uint8_t input)
{
	return input < ADS124S08_CHANNEL_COUNT || input == ADS124S08_AINCOM;
}

static int ads124s08_idac_code(uint16_t current_ua, uint8_t *code)
{
	switch (current_ua) {
	case 0U:
		*code = 0x00U;
		return 0;
	case 10U:
		*code = 0x01U;
		return 0;
	case 50U:
		*code = 0x02U;
		return 0;
	case 100U:
		*code = 0x03U;
		return 0;
	case 250U:
		*code = 0x04U;
		return 0;
	case 500U:
		*code = 0x05U;
		return 0;
	case 750U:
		*code = 0x06U;
		return 0;
	case 1000U:
		*code = 0x07U;
		return 0;
	case 1500U:
		*code = 0x08U;
		return 0;
	case 2000U:
		*code = 0x09U;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ads124s08_configure_input(const struct device *dev, uint8_t positive,
				     uint8_t negative, bool differential,
				     bool enable_idac, uint8_t idac_pin)
{
	const struct ads124s08_config *config = dev->config;
	uint8_t idac_code = 0U;
	uint8_t idac_mux = ADS124S08_IDACMUX_DISCONNECTED;
	uint8_t regs[] = {
		ADS124S08_INPMUX(positive, differential ? negative : ADS124S08_AINCOM),
		ADS124S08_PGA_GAIN_1_BYPASS,
		ADS124S08_DATARATE_SINGLE_SHOT_20SPS_LOW_LATENCY,
		ADS124S08_REF_EXTERNAL0_REFN_BUF_DISABLED,
		ADS124S08_IDAC_OFF,
		ADS124S08_IDACMUX_DISCONNECTED,
	};
	int ret;

	if (!ads124s08_input_valid(positive) ||
	    (differential && !ads124s08_input_valid(negative)) ||
	    (differential && positive == negative)) {
		return -EINVAL;
	}

	if (enable_idac) {
		if (!ads124s08_input_valid(idac_pin)) {
			return -EINVAL;
		}

		ret = ads124s08_idac_code(config->idac_current_ua, &idac_code);
		if (ret < 0) {
			return ret;
		}

		idac_mux = (0x0FU << 4) | (idac_pin & 0x0FU);
		regs[4] = idac_code;
		regs[5] = idac_mux;
	}

	return ads124s08_write_registers(dev, ADS124S08_REG_INPMUX, regs,
					 ARRAY_SIZE(regs));
}

static int ads124s08_wait_data_ready(const struct device *dev)
{
	const struct ads124s08_config *config = dev->config;
	int64_t deadline = k_uptime_get() + config->conversion_timeout_ms;

	do {
		int ready = gpio_pin_get_dt(&config->drdy);

		if (ready < 0) {
			return ready;
		}

		if (ready > 0) {
			return 0;
		}

		k_msleep(1);
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

static int ads124s08_read_raw_configured(const struct device *dev,
					 int32_t *raw)
{
	const struct ads124s08_config *config = dev->config;
	uint8_t tx_buf[4] = {
		ADS124S08_CMD_RDATA,
		ADS124S08_CMD_NOP,
		ADS124S08_CMD_NOP,
		ADS124S08_CMD_NOP,
	};
	uint8_t rx_buf[sizeof(tx_buf)] = {0};
	const struct spi_buf tx_buffers[] = {
		{
			.buf = tx_buf,
			.len = sizeof(tx_buf),
		},
	};
	const struct spi_buf rx_buffers[] = {
		{
			.buf = rx_buf,
			.len = sizeof(rx_buf),
		},
	};
	const struct spi_buf_set tx = {
		.buffers = tx_buffers,
		.count = ARRAY_SIZE(tx_buffers),
	};
	const struct spi_buf_set rx = {
		.buffers = rx_buffers,
		.count = ARRAY_SIZE(rx_buffers),
	};
	uint32_t unsigned_raw;
	int ret;

	if (raw == NULL) {
		return -EINVAL;
	}

	ret = ads124s08_send_command(dev, ADS124S08_CMD_START);
	if (ret < 0) {
		return ret;
	}

	ret = ads124s08_wait_data_ready(dev);
	if (ret < 0) {
		return ret;
	}

	ret = spi_transceive_dt(&config->spi, &tx, &rx);
	if (ret < 0) {
		return ret;
	}

	unsigned_raw = ((uint32_t)rx_buf[1] << 16) |
		       ((uint32_t)rx_buf[2] << 8) |
		       (uint32_t)rx_buf[3];

	if ((unsigned_raw & BIT(23)) != 0U) {
		unsigned_raw |= 0xFF000000U;
	}

	*raw = (int32_t)unsigned_raw;
	return 0;
}

int ads124s08_reset(const struct device *dev)
{
	const struct ads124s08_config *config = dev->config;
	int ret;

	if (config->reset.port != NULL) {
		ret = gpio_pin_set_dt(&config->reset, 1);
		if (ret < 0) {
			return ret;
		}

		k_usleep(10);

		ret = gpio_pin_set_dt(&config->reset, 0);
		if (ret < 0) {
			return ret;
		}
	} else {
		ret = ads124s08_send_command(dev, ADS124S08_CMD_RESET);
		if (ret < 0) {
			return ret;
		}
	}

	k_msleep(3);
	return 0;
}

int ads124s08_read_status(const struct device *dev, uint8_t *status)
{
	return ads124s08_read_register(dev, ADS124S08_REG_STATUS, status);
}

static int ads124s08_read_channel(const struct device *dev, uint8_t positive,
				  uint8_t negative, bool differential,
				  bool enable_idac, uint8_t idac_pin,
				  int32_t *raw)
{
	int ret;

	ret = ads124s08_configure_input(dev, positive, negative, differential,
					enable_idac, idac_pin);
	if (ret < 0) {
		return ret;
	}

	return ads124s08_read_raw_configured(dev, raw);
}

int ads124s08_read_single_ended(const struct device *dev, uint8_t positive,
				int32_t *raw)
{
	return ads124s08_read_channel(dev, positive, ADS124S08_AINCOM, false,
				      false, ADS124S08_AINCOM, raw);
}

int ads124s08_read_differential(const struct device *dev, uint8_t positive,
				uint8_t negative, bool enable_idac,
				uint8_t idac_pin, int32_t *raw)
{
	return ads124s08_read_channel(dev, positive, negative, true,
				      enable_idac, idac_pin, raw);
}

int ads124s08_raw_to_microvolts(const struct device *dev, int32_t raw,
				int32_t *microvolts)
{
	const struct ads124s08_config *config = dev->config;
	int64_t value_uv;

	if (microvolts == NULL || config->reference_mv == 0U) {
		return -EINVAL;
	}

	value_uv = (int64_t)raw * (int64_t)config->reference_mv * 1000LL;
	value_uv /= ADS124S08_FULL_SCALE_CODE;

	if (value_uv > INT32_MAX || value_uv < INT32_MIN) {
		return -ERANGE;
	}

	*microvolts = (int32_t)value_uv;
	return 0;
}

static int ads124s08_channel_setup(const struct device *dev,
				   const struct adc_channel_cfg *channel_cfg)
{
	struct ads124s08_data *data = dev->data;

	if (channel_cfg == NULL || channel_cfg->channel_id != 0U) {
		return -EINVAL;
	}

	if (channel_cfg->gain != ADC_GAIN_1) {
		return -ENOTSUP;
	}

	data->channel_cfg = *channel_cfg;
	data->channel_configured = true;

	return 0;
}

static int ads124s08_read(const struct device *dev,
			  const struct adc_sequence *sequence)
{
	struct ads124s08_data *data = dev->data;
	uint8_t positive = 0U;
	uint8_t negative = ADS124S08_AINCOM;
	bool differential = false;
	bool enable_idac = false;
	uint8_t idac_pin = ADS124S08_AINCOM;

	if (!data->channel_configured || sequence == NULL ||
	    sequence->buffer == NULL || sequence->channels != BIT(0) ||
	    sequence->buffer_size < sizeof(int32_t) ||
	    sequence->resolution != ADS124S08_RESOLUTION_BITS) {
		return -EINVAL;
	}

#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
	positive = data->channel_cfg.input_positive;
	negative = data->channel_cfg.input_negative;
	differential = data->channel_cfg.differential;
#endif

#if defined(CONFIG_ADC_CONFIGURABLE_EXCITATION_CURRENT_SOURCE_PIN)
	enable_idac = data->channel_cfg.current_source_pin_set;
	idac_pin = data->channel_cfg.current_source_pin[0];
#endif

	return ads124s08_read_channel(dev, positive, negative, differential,
				      enable_idac, idac_pin,
				      (int32_t *)sequence->buffer);
}

static int ads124s08_init(const struct device *dev)
{
	const struct ads124s08_config *config = dev->config;
	uint8_t status;
	int ret;

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI bus %s is not ready", config->spi.bus->name);
		return -ENODEV;
	}

	if (config->reset.port != NULL) {
		if (!gpio_is_ready_dt(&config->reset)) {
			LOG_ERR("RESET GPIO controller is not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->reset,
					    GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	if (!gpio_is_ready_dt(&config->drdy)) {
		LOG_ERR("DRDY GPIO controller is not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->drdy, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	if (config->start_sync.port != NULL) {
		if (!gpio_is_ready_dt(&config->start_sync)) {
			LOG_ERR("START/SYNC GPIO controller is not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->start_sync,
					    GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	ret = ads124s08_reset(dev);
	if (ret < 0) {
		LOG_ERR("Reset failed: %d", ret);
		return ret;
	}

	ret = ads124s08_read_status(dev, &status);
	if (ret < 0) {
		LOG_ERR("Status read failed: %d", ret);
		return ret;
	}

	LOG_INF("ADS124S08 ready on %s CS%u status=0x%02x ref=%umV idac=%uuA",
		config->spi.bus->name, config->spi.config.cs.gpio.pin, status,
		config->reference_mv, config->idac_current_ua);

	return 0;
}

static DEVICE_API(adc, ads124s08_api) = {
	.channel_setup = ads124s08_channel_setup,
	.read = ads124s08_read,
};

#define ADS124S08_DEFINE(inst)                                             \
	static const struct ads124s08_config ads124s08_config_##inst = {   \
		.spi = SPI_DT_SPEC_INST_GET(inst,                         \
			SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB |            \
			SPI_MODE_CPHA | SPI_WORD_SET(8)),                  \
		.reset = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}), \
		.drdy = GPIO_DT_SPEC_INST_GET(inst, drdy_gpios),          \
		.start_sync = GPIO_DT_SPEC_INST_GET_OR(inst,              \
			start_sync_gpios, {0}),                           \
		.reference_mv = DT_INST_PROP(inst, reference_millivolt),  \
		.idac_current_ua =                                       \
			DT_INST_PROP(inst, idac_current_microamp),        \
		.conversion_timeout_ms =                                 \
			DT_INST_PROP(inst, conversion_timeout_ms),        \
	};                                                                 \
	static struct ads124s08_data ads124s08_data_##inst;                \
	DEVICE_DT_INST_DEFINE(inst, ads124s08_init, NULL,                  \
			      &ads124s08_data_##inst,                      \
			      &ads124s08_config_##inst, POST_KERNEL,       \
			      CONFIG_ADC_INIT_PRIORITY, &ads124s08_api);

DT_INST_FOREACH_STATUS_OKAY(ADS124S08_DEFINE)
