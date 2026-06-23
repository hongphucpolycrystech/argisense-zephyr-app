/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "methane_sensor.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

uint16_t argisense_dynament_checksum(const uint8_t *data, size_t len)
{
	uint32_t sum = 0;

	for (size_t i = 0; i < len; i++) {
		sum += data[i];
	}

	return (uint16_t)sum;
}

int argisense_dynament_live_data_request(
	uint8_t request[ARGISENSE_DYNAMENT_LIVE_DATA_REQUEST_LEN])
{
	const uint8_t header[] = {
		ARGISENSE_DYNAMENT_DLE,
		ARGISENSE_DYNAMENT_RD,
		ARGISENSE_DYNAMENT_LIVE_DATA_SIMPLE_VARIABLE_ID,
		ARGISENSE_DYNAMENT_DLE,
		ARGISENSE_DYNAMENT_EOF,
	};
	const uint16_t checksum = argisense_dynament_checksum(header, sizeof(header));

	memcpy(request, header, sizeof(header));
	sys_put_be16(checksum, &request[sizeof(header)]);

	return sizeof(header) + sizeof(checksum);
}

int32_t argisense_dynament_percent_to_ppm_x100(float percent_volume)
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

int argisense_dynament_parse_live_data_simple(
	const uint8_t *frame, size_t len,
	struct argisense_dynament_live_data_simple *data)
{
	uint32_t gas_bits;
	uint16_t expected_checksum;
	uint16_t received_checksum;

	if (frame == NULL || data == NULL) {
		return -EINVAL;
	}

	/*
	 * This parser handles the compact, unescaped frame shown in AN0007.
	 * A full stream parser should de-stuff DLE bytes before calling this
	 * function because Dynament frames can grow when payload bytes equal 0x10.
	 */
	if (len != ARGISENSE_DYNAMENT_LIVE_DATA_SIMPLE_FRAME_LEN) {
		return -EMSGSIZE;
	}

	if (frame[0] != ARGISENSE_DYNAMENT_DLE ||
	    frame[1] != ARGISENSE_DYNAMENT_DAT ||
	    frame[2] != 0x08 ||
	    frame[11] != ARGISENSE_DYNAMENT_DLE ||
	    frame[12] != ARGISENSE_DYNAMENT_EOF) {
		return -EBADMSG;
	}

	expected_checksum = argisense_dynament_checksum(frame, len - 2);
	received_checksum = sys_get_be16(&frame[len - 2]);
	if (received_checksum != expected_checksum) {
		return -EIO;
	}

	data->version = sys_get_le16(&frame[3]);
	data->status_flags = sys_get_le16(&frame[5]);

	gas_bits = sys_get_le32(&frame[7]);
	memcpy(&data->gas_percent_volume, &gas_bits, sizeof(data->gas_percent_volume));

	data->gas_ppm_x100 =
		argisense_dynament_percent_to_ppm_x100(data->gas_percent_volume);
	data->gas_valid = data->gas_percent_volume >= 0.0f && data->status_flags == 0;

	return 0;
}
