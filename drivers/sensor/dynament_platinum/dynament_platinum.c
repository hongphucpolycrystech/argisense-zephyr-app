/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT argisense_dynament_platinum_hydrocarbon

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <argisense/drivers/sensor/dynament_platinum.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(dynament_platinum, CONFIG_SENSOR_LOG_LEVEL);

#define DYNAMENT_DLE 0x10
#define DYNAMENT_RD  0x13
#define DYNAMENT_DAT 0x1a
#define DYNAMENT_EOF 0x1f

#define DYNAMENT_LIVE_DATA_REQUEST_LEN 7U
#define DYNAMENT_LIVE_DATA_SIMPLE_FRAME_LEN 15U
#define DYNAMENT_DEFAULT_READ_TIMEOUT_MS 500U
#define DYNAMENT_LIVE_DATA_SIMPLE_PAYLOAD_LEN 8U

struct dynament_platinum_config {
	const struct device *uart;
	const char *gas_type;
	const char *protocol;
	uint32_t uart_baudrate;
	uint32_t full_scale_percent_x100;
	uint32_t warmup_time_ms;
	uint32_t read_period_ms;
	uint32_t logic_level_mv;
	uint8_t live_data_variable_id;
};

struct dynament_platinum_data {
	struct k_mutex lock;
	uint16_t protocol_version;
	uint16_t status_flags;
	int32_t gas_ppm_x100;
	bool gas_valid;
	bool sample_ready;
	int64_t init_uptime_ms;
};

static uint16_t dynament_checksum(const uint8_t *data, size_t len)
{
	uint32_t sum = 0U;

	for (size_t i = 0U; i < len; i++) {
		sum += data[i];
	}

	return (uint16_t)sum;
}

static int32_t dynament_percent_to_ppm_x100(float percent_volume)
{
	const double ppm_x100 = (double)percent_volume * 1000000.0;

	if (ppm_x100 >= (double)INT32_MAX) {
		return INT32_MAX;
	}

	if (ppm_x100 <= (double)INT32_MIN) {
		return INT32_MIN;
	}

	return (int32_t)(ppm_x100 + (ppm_x100 >= 0.0 ? 0.5 : -0.5));
}

static int dynament_build_live_data_request(const struct device *dev,
					    uint8_t request[DYNAMENT_LIVE_DATA_REQUEST_LEN])
{
	const struct dynament_platinum_config *config = dev->config;
	const uint8_t header[] = {
		DYNAMENT_DLE,
		DYNAMENT_RD,
		config->live_data_variable_id,
		DYNAMENT_DLE,
		DYNAMENT_EOF,
	};
	const uint16_t checksum = dynament_checksum(header, sizeof(header));

	memcpy(request, header, sizeof(header));
	sys_put_be16(checksum, &request[sizeof(header)]);

	return sizeof(header) + sizeof(checksum);
}

static void dynament_uart_flush(const struct device *uart)
{
	uint8_t byte;

	while (uart_poll_in(uart, &byte) == 0) {
	}
}

static int dynament_uart_write(const struct device *uart, const uint8_t *buf,
			       size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		uart_poll_out(uart, buf[i]);
	}

	return 0;
}

static int dynament_uart_read_byte_until(const struct device *uart,
					 uint8_t *byte, int64_t deadline)
{
	while (true) {
		if (uart_poll_in(uart, byte) == 0) {
			return 0;
		}

		if (k_uptime_get() >= deadline) {
			return -ETIMEDOUT;
		}

		k_msleep(1);
	}
}

static int dynament_uart_read_payload_byte(const struct device *uart,
					   uint8_t *byte, int64_t deadline)
{
	int ret;

	ret = dynament_uart_read_byte_until(uart, byte, deadline);
	if (ret < 0 || *byte != DYNAMENT_DLE) {
		return ret;
	}

	ret = dynament_uart_read_byte_until(uart, byte, deadline);
	if (ret < 0) {
		return ret;
	}

	if (*byte == DYNAMENT_EOF) {
		return -EBADMSG;
	}

	return 0;
}

static int dynament_uart_read_frame(const struct device *uart, uint8_t *buf,
				    size_t len, uint32_t timeout_ms)
{
	const int64_t deadline = k_uptime_get() + timeout_ms;
	uint8_t byte;
	uint8_t payload_len;
	size_t compact_len;
	int ret;

	if (len < 7U) {
		return -ENOMEM;
	}

	while (true) {
		ret = dynament_uart_read_byte_until(uart, &byte, deadline);
		if (ret < 0) {
			return ret;
		}

		if (byte != DYNAMENT_DLE) {
			continue;
		}

		ret = dynament_uart_read_byte_until(uart, &byte, deadline);
		if (ret < 0) {
			return ret;
		}

		if (byte == DYNAMENT_DAT) {
			break;
		}
	}

	ret = dynament_uart_read_byte_until(uart, &payload_len, deadline);
	if (ret < 0) {
		return ret;
	}

	compact_len = 7U + payload_len;
	if (compact_len > len) {
		return -EMSGSIZE;
	}

	buf[0] = DYNAMENT_DLE;
	buf[1] = DYNAMENT_DAT;
	buf[2] = payload_len;

	for (size_t i = 0U; i < payload_len; i++) {
		ret = dynament_uart_read_payload_byte(uart, &buf[3U + i],
						      deadline);
		if (ret < 0) {
			return ret;
		}
	}

	ret = dynament_uart_read_byte_until(uart, &byte, deadline);
	if (ret < 0) {
		return ret;
	}
	if (byte != DYNAMENT_DLE) {
		return -EBADMSG;
	}

	ret = dynament_uart_read_byte_until(uart, &byte, deadline);
	if (ret < 0) {
		return ret;
	}
	if (byte != DYNAMENT_EOF) {
		return -EBADMSG;
	}

	buf[3U + payload_len] = DYNAMENT_DLE;
	buf[4U + payload_len] = DYNAMENT_EOF;

	ret = dynament_uart_read_byte_until(uart, &buf[5U + payload_len],
					    deadline);
	if (ret < 0) {
		return ret;
	}

	ret = dynament_uart_read_byte_until(uart, &buf[6U + payload_len],
					    deadline);
	if (ret < 0) {
		return ret;
	}

	return compact_len == len ? 0 : -EMSGSIZE;
}

static int dynament_parse_live_data_simple(const uint8_t *frame, size_t len,
					   struct dynament_platinum_data *data)
{
	uint32_t gas_bits;
	float gas_percent_volume;
	uint16_t expected_checksum;
	uint16_t received_checksum;

	if (len != DYNAMENT_LIVE_DATA_SIMPLE_FRAME_LEN) {
		return -EMSGSIZE;
	}

	if (frame[0] != DYNAMENT_DLE || frame[1] != DYNAMENT_DAT ||
	    frame[2] != DYNAMENT_LIVE_DATA_SIMPLE_PAYLOAD_LEN ||
	    frame[11] != DYNAMENT_DLE || frame[12] != DYNAMENT_EOF) {
		return -EBADMSG;
	}

	expected_checksum = dynament_checksum(frame, len - 2U);
	received_checksum = sys_get_be16(&frame[len - 2U]);
	if (received_checksum != expected_checksum) {
		return -EIO;
	}

	data->protocol_version = sys_get_le16(&frame[3]);
	data->status_flags = sys_get_le16(&frame[5]);

	gas_bits = sys_get_le32(&frame[7]);
	memcpy(&gas_percent_volume, &gas_bits, sizeof(gas_percent_volume));

	data->gas_ppm_x100 = dynament_percent_to_ppm_x100(gas_percent_volume);
	data->gas_valid = gas_percent_volume >= 0.0f && data->status_flags == 0U;
	data->sample_ready = true;

	return 0;
}

static int dynament_platinum_sample_fetch(const struct device *dev,
					  enum sensor_channel chan)
{
	struct dynament_platinum_data *data = dev->data;
	const struct dynament_platinum_config *config = dev->config;
	uint8_t request[DYNAMENT_LIVE_DATA_REQUEST_LEN];
	uint8_t frame[DYNAMENT_LIVE_DATA_SIMPLE_FRAME_LEN];
	const int channel = (int)chan;
	int ret;

	if (channel != (int)SENSOR_CHAN_ALL &&
	    channel != ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PPM &&
	    channel != ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PERCENT_VOLUME &&
	    channel != ARGISENSE_DYNAMENT_SENSOR_CHAN_STATUS_FLAGS &&
	    channel != ARGISENSE_DYNAMENT_SENSOR_CHAN_PROTOCOL_VERSION) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	dynament_build_live_data_request(dev, request);
	dynament_uart_flush(config->uart);
	ret = dynament_uart_write(config->uart, request, sizeof(request));
	if (ret < 0) {
		goto out;
	}

	ret = dynament_uart_read_frame(config->uart, frame, sizeof(frame),
				       DYNAMENT_DEFAULT_READ_TIMEOUT_MS);
	if (ret < 0) {
		LOG_ERR("Live-data-simple UART read failed: %d", ret);
		data->sample_ready = false;
		goto out;
	}

	ret = dynament_parse_live_data_simple(frame, sizeof(frame), data);
	if (ret < 0) {
		LOG_ERR("Live-data-simple frame parse failed: %d", ret);
		data->sample_ready = false;
		goto out;
	}

	if ((uint32_t)(k_uptime_get() - data->init_uptime_ms) <
	    config->warmup_time_ms) {
		data->gas_valid = false;
		ret = -EAGAIN;
		goto out;
	}

	if (!data->gas_valid) {
		ret = -EIO;
		goto out;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int dynament_platinum_channel_get(const struct device *dev,
					 enum sensor_channel chan,
					 struct sensor_value *val)
{
	struct dynament_platinum_data *data = dev->data;
	const int channel = (int)chan;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->sample_ready) {
		ret = -ENODATA;
		goto out;
	}

	switch (channel) {
	case ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PPM:
		ret = sensor_value_from_micro(
			val, (int64_t)data->gas_ppm_x100 * 10000LL);
		break;
	case ARGISENSE_DYNAMENT_SENSOR_CHAN_GAS_PERCENT_VOLUME:
		ret = sensor_value_from_micro(val, data->gas_ppm_x100);
		break;
	case ARGISENSE_DYNAMENT_SENSOR_CHAN_STATUS_FLAGS:
		val->val1 = data->status_flags;
		val->val2 = 0;
		break;
	case ARGISENSE_DYNAMENT_SENSOR_CHAN_PROTOCOL_VERSION:
		val->val1 = data->protocol_version;
		val->val2 = 0;
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int dynament_platinum_init(const struct device *dev)
{
	struct dynament_platinum_data *data = dev->data;
	const struct dynament_platinum_config *config = dev->config;

	k_mutex_init(&data->lock);
	data->init_uptime_ms = k_uptime_get();

	if (!device_is_ready(config->uart)) {
		LOG_ERR("UART bus is not ready");
		return -ENODEV;
	}

	LOG_INF("Dynament Platinum %s sensor ready on %s at %u baud",
		config->gas_type, config->uart->name, config->uart_baudrate);

	return 0;
}

static DEVICE_API(sensor, dynament_platinum_api) = {
	.sample_fetch = dynament_platinum_sample_fetch,
	.channel_get = dynament_platinum_channel_get,
};

#define DYNAMENT_PLATINUM_DEFINE(inst)                                      \
	static struct dynament_platinum_data dynament_platinum_data_##inst; \
	static const struct dynament_platinum_config                         \
		dynament_platinum_config_##inst = {                         \
			.uart = DEVICE_DT_GET(DT_BUS(DT_DRV_INST(inst))),    \
			.gas_type = DT_INST_PROP(inst, gas_type),            \
			.protocol = DT_INST_PROP(inst, protocol),            \
			.uart_baudrate =                                    \
				DT_PROP(DT_BUS(DT_DRV_INST(inst)),           \
					current_speed),                       \
			.full_scale_percent_x100 =                          \
				DT_INST_PROP(inst, full_scale_percent_x100), \
			.warmup_time_ms = DT_INST_PROP(inst, warmup_time_ms), \
			.read_period_ms = DT_INST_PROP(inst, read_period_ms), \
			.logic_level_mv = DT_INST_PROP(inst, logic_level_mv), \
			.live_data_variable_id =                            \
				DT_INST_PROP(inst, live_data_variable_id),   \
		};                                                           \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, dynament_platinum_init, NULL,     \
				     &dynament_platinum_data_##inst,        \
				     &dynament_platinum_config_##inst,      \
				     POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, \
				     &dynament_platinum_api);

DT_INST_FOREACH_STATUS_OKAY(DYNAMENT_PLATINUM_DEFINE)
