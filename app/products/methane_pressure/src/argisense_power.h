/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_POWER_H_
#define ARGISENSE_POWER_H_

int argisense_power_configure(void);

void argisense_power_idle_state(void);

int argisense_power_measurement_on(void);

void argisense_power_measurement_lock(void);

void argisense_power_measurement_unlock(void);

#endif /* ARGISENSE_POWER_H_ */
