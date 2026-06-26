/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_CURRENT_LOOP_OUTPUT_H_
#define ARGISENSE_CURRENT_LOOP_OUTPUT_H_

#include <stdbool.h>
#include <stdint.h>

#define ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT 2

enum argisense_current_loop_channel {
	ARGISENSE_CURRENT_LOOP_METHANE = 0,
	ARGISENSE_CURRENT_LOOP_PRESSURE = 1,
};

struct argisense_measurement_sample {
	int32_t methane_ppm_x100;
	int32_t pressure_pa;
	int32_t methane_last_error;
	int32_t pressure_last_error;
	int32_t humidity_last_error;
	int32_t pressure_temperature_centi_c;
	int32_t humidity_rh_x100;
	int32_t humidity_temperature_centi_c;
	uint32_t pressure_d1_raw;
	uint32_t pressure_d2_raw;
	uint16_t methane_status_flags;
	uint16_t methane_protocol_version;
	uint8_t pressure_prom_crc_read;
	uint8_t pressure_prom_crc_calc;
	bool methane_valid;
	bool pressure_valid;
	bool humidity_valid;
};

struct argisense_current_loop_command {
	enum argisense_current_loop_channel channel;
	const char *name;
	int32_t source_value;
	int32_t range_low;
	int32_t range_high;
	int32_t current_ua;
	bool valid;
};

struct argisense_current_loop_limits {
	int32_t dac_min_current_ua;
	int32_t dac_max_current_ua;
	int32_t dac_fault_current_ua;
	int32_t methane_range_low_ppm;
	int32_t methane_range_high_ppm;
	int32_t pressure_range_low_pa;
	int32_t pressure_range_high_pa;
};

int32_t argisense_current_loop_scale_ua(int32_t value, int32_t range_low,
					int32_t range_high, bool valid);

int32_t argisense_current_loop_scale_ua_with_limits(
	int32_t value, int32_t range_low, int32_t range_high, bool valid,
	const struct argisense_current_loop_limits *limits);

void argisense_current_loop_prepare(
	const struct argisense_measurement_sample *sample,
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT]);

void argisense_current_loop_prepare_with_limits(
	const struct argisense_measurement_sample *sample,
	const struct argisense_current_loop_limits *limits,
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT]);

#endif /* ARGISENSE_CURRENT_LOOP_OUTPUT_H_ */
