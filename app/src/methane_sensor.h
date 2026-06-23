/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_METHANE_SENSOR_H_
#define ARGISENSE_METHANE_SENSOR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARGISENSE_DYNAMENT_DLE 0x10
#define ARGISENSE_DYNAMENT_RD 0x13
#define ARGISENSE_DYNAMENT_DAT 0x1a
#define ARGISENSE_DYNAMENT_EOF 0x1f
#define ARGISENSE_DYNAMENT_LIVE_DATA_SIMPLE_VARIABLE_ID 0x06

#define ARGISENSE_DYNAMENT_LIVE_DATA_REQUEST_LEN 7
#define ARGISENSE_DYNAMENT_LIVE_DATA_SIMPLE_FRAME_LEN 15

struct argisense_dynament_live_data_simple {
	uint16_t version;
	uint16_t status_flags;
	float gas_percent_volume;
	int32_t gas_ppm_x100;
	bool gas_valid;
};

uint16_t argisense_dynament_checksum(const uint8_t *data, size_t len);

int argisense_dynament_live_data_request(
	uint8_t request[ARGISENSE_DYNAMENT_LIVE_DATA_REQUEST_LEN]);

int argisense_dynament_parse_live_data_simple(
	const uint8_t *frame, size_t len,
	struct argisense_dynament_live_data_simple *data);

int32_t argisense_dynament_percent_to_ppm_x100(float percent_volume);

#endif /* ARGISENSE_METHANE_SENSOR_H_ */
