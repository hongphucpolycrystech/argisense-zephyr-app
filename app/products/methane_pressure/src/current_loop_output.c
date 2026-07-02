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

static const struct argisense_current_loop_limits default_limits = {
	.dac_min_current_ua = CONFIG_ARGISENSE_DAC_MIN_CURRENT_UA,
	.dac_max_current_ua = CONFIG_ARGISENSE_DAC_MAX_CURRENT_UA,
	.dac_fault_current_ua = CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA,
	.methane_range_low_ppm = CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM,
	.methane_range_high_ppm = CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM,
	.pressure_range_low_pa = CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA,
	.pressure_range_high_pa = CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA,
};

int32_t argisense_current_loop_scale_ua_with_limits(
	int32_t value, int32_t range_low, int32_t range_high, bool valid,
	const struct argisense_current_loop_limits *limits)
{
	const int32_t min_current_ua = limits->dac_min_current_ua;
	const int32_t max_current_ua = limits->dac_max_current_ua;
	const int64_t range_span = (int64_t)range_high - range_low;
	int64_t value_span;

	if (!valid || range_span <= 0) {
		return limits->dac_fault_current_ua;
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

int32_t argisense_current_loop_scale_ua(int32_t value, int32_t range_low,
					int32_t range_high, bool valid)
{
	return argisense_current_loop_scale_ua_with_limits(
		value, range_low, range_high, valid, &default_limits);
}

void argisense_current_loop_prepare_with_limits(
	const struct argisense_measurement_sample *sample,
	const struct argisense_current_loop_limits *limits,
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT])
{
	const int32_t methane_low_x100 = limits->methane_range_low_ppm * 100;
	const int32_t methane_high_x100 = limits->methane_range_high_ppm * 100;

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
		argisense_current_loop_scale_ua_with_limits(
			commands[ARGISENSE_CURRENT_LOOP_METHANE].source_value,
			commands[ARGISENSE_CURRENT_LOOP_METHANE].range_low,
			commands[ARGISENSE_CURRENT_LOOP_METHANE].range_high,
			commands[ARGISENSE_CURRENT_LOOP_METHANE].valid, limits);

	commands[ARGISENSE_CURRENT_LOOP_PRESSURE] =
		(struct argisense_current_loop_command){
			.channel = ARGISENSE_CURRENT_LOOP_PRESSURE,
			.name = "pressure",
			.source_value = sample->pressure_pa,
			.range_low = limits->pressure_range_low_pa,
			.range_high = limits->pressure_range_high_pa,
			.valid = sample->pressure_valid,
		};
	commands[ARGISENSE_CURRENT_LOOP_PRESSURE].current_ua =
		argisense_current_loop_scale_ua_with_limits(
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].source_value,
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].range_low,
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].range_high,
			commands[ARGISENSE_CURRENT_LOOP_PRESSURE].valid, limits);
}

void argisense_current_loop_prepare(
	const struct argisense_measurement_sample *sample,
	struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT])
{
	argisense_current_loop_prepare_with_limits(sample, &default_limits,
						  commands);
}
