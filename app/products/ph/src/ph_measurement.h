/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_PH_MEASUREMENT_H_
#define ARGISENSE_PH_MEASUREMENT_H_

#include "argisense_registers.h"

int ph_measurement_init(void);

void ph_measurement_sample(struct argisense_ph_measurement_sample *sample);

int ph_measurement_read_adc_uv(uint8_t positive, uint8_t negative,
			       bool differential, bool enable_idac,
			       uint8_t idac_pin, int32_t *raw,
			       int32_t *microvolts);

int ph_measurement_set_bias_mv(uint8_t channel, uint32_t millivolt);

int ph_measurement_set_electrode_zero(bool closed);

#endif /* ARGISENSE_PH_MEASUREMENT_H_ */
