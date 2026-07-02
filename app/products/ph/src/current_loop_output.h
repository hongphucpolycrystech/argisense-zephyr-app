/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_PH_CURRENT_LOOP_OUTPUT_H_
#define ARGISENSE_PH_CURRENT_LOOP_OUTPUT_H_

#include "argisense_registers.h"
#include "argisense_settings.h"

int current_loop_output_init(void);

int current_loop_output_update(const struct argisense_runtime_config *config,
			       struct argisense_ph_measurement_sample *sample);

#endif /* ARGISENSE_PH_CURRENT_LOOP_OUTPUT_H_ */
