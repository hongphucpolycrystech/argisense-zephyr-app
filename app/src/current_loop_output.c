/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "current_loop_output.h"

#include <zephyr/kernel.h>

BUILD_ASSERT(CONFIG_ARGISENSE_DAC_MIN_CURRENT_UA <
	     CONFIG_ARGISENSE_DAC_MAX_CURRENT_UA,
	     "DAC minimum current must be less than maximum current");
BUILD_ASSERT(CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM <
	     CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM,
	     "Methane DAC range low must be less than range high");
BUILD_ASSERT(CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA <
	     CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA,
	     "Pressure DAC range low must be less than range high");

int32_t argisense_current_loop_scale_ua(int32_t value, int32_t range_low,
					int32_t range_high, bool valid)
{
	const int32_t min_current_ua = CONFIG_ARGISENSE_DAC_MIN_CURRENT_UA;
	const int32_t max_current_ua = CONFIG_ARGISENSE_DAC_MAX_CURRENT_UA;
	const int64_t range_span = (int64_t)range_high - range_low;
	int64_t value_span;

	if (!valid || range_span <= 0) {
		return CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA;
	}

	value_span = (int64_t)value - range_low;

	if (value_span <= 0) {
		return min_current_ua;
	}

	if (value_span >= range_span) {
		return max_current_ua;
	}

	return min_current_ua +
	       (int32_t)((value_span * (max_current_ua - min_current_ua) +
			  (range_span / 2)) /
			 range_span);
}

void argisense_current_loop_prepare(
	const struct argisense_measurement_sample *sample,
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT])
{
	const int32_t methane_low_x100 =
		CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM * 100;
	const int32_t methane_high_x100 =
		CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM * 100;

	commands[ARGISENSE_CURRENT_LOOP_METHANE] =
		(struct argisense_current_loop_command){
			.channel = ARGISENSE_CURRENT_LOOP_METHANE,
			.name = "methane",
			.source_value = sample->methane_ppm_x100,
			.range_low = methane_low_x100,
			.range_high = methane_high_x100,
			.valid = sample->methane_valid,
		};
	commands[ARGISENSE_CURRENT_LOOP_METHANE].current_ua =
		argisense_current_loop_scale_ua(
			commands[ARGISENSE_CURRENT_LOOP_METHANE].source_value,
			commands[ARGISENSE_CURRENT_LOOP_METHANE].range_low,
			commands[ARGISENSE_CURRENT_LOOP_METHANE].range_high,
			commands[ARGISENSE_CURRENT_LOOP_METHANE].valid);

	commands[ARGISENSE_CURRENT_LOOP_PRESSURE] =
		(struct argisense_current_loop_command){
			.channel = ARGISENSE_CURRENT_LOOP_PRESSURE,
			.name = "pressure",
			.source_value = sample->pressure_pa,
			.range_low = CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA,
			.range_high = CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA,
			.valid = sample->pressure_valid,
		};
	commands[ARGISENSE_CURRENT_LOOP_PRESSURE].current_ua =
		argisense_current_loop_scale_ua(
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].source_value,
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].range_low,
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].range_high,
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].valid);
}
