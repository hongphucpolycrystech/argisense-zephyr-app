/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_DRIVERS_ADC_ADS124S08_H_
#define ARGISENSE_DRIVERS_ADC_ADS124S08_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#define ADS124S08_AINCOM 12U
#define ADS124S08_CHANNEL_COUNT 12U
#define ADS124S08_RESOLUTION_BITS 24U

int ads124s08_reset(const struct device *dev);

int ads124s08_read_status(const struct device *dev, uint8_t *status);

int ads124s08_read_single_ended(const struct device *dev, uint8_t positive,
				int32_t *raw);

int ads124s08_read_differential(const struct device *dev, uint8_t positive,
				uint8_t negative, bool enable_idac,
				uint8_t idac_pin, int32_t *raw);

int ads124s08_raw_to_microvolts(const struct device *dev, int32_t raw,
				int32_t *microvolts);

#endif /* ARGISENSE_DRIVERS_ADC_ADS124S08_H_ */
