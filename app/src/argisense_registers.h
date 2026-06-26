/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_REGISTERS_H_
#define ARGISENSE_REGISTERS_H_

#include <stdint.h>

#include "current_loop_output.h"

#define ARGISENSE_REGISTER_MAP_VERSION 3U

int argisense_register_read_holding(uint16_t addr, uint16_t *reg);

int argisense_register_write_holding(uint16_t addr, uint16_t reg);

const char *argisense_register_holding_name(uint16_t addr);

void argisense_register_update_sample(
	const struct argisense_measurement_sample *sample,
	const struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT]);

#endif /* ARGISENSE_REGISTERS_H_ */
