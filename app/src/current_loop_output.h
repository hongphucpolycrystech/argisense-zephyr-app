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
	bool methane_valid;
	bool pressure_valid;
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

int32_t argisense_current_loop_scale_ua(int32_t value, int32_t range_low,
					int32_t range_high, bool valid);

void argisense_current_loop_prepare(
	const struct argisense_measurement_sample *sample,
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT]);

#endif /* ARGISENSE_CURRENT_LOOP_OUTPUT_H_ */
