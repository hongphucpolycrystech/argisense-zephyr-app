/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_PH_REGISTERS_H_
#define ARGISENSE_PH_REGISTERS_H_

#include <stdbool.h>
#include <stdint.h>

#define ARGISENSE_REGISTER_MAP_VERSION 3U
#define ARGISENSE_PH_OPERATING_STATUS_RAW_VALID (1U << 0)
#define ARGISENSE_PH_OPERATING_STATUS_CAL_VALID (1U << 1)
#define ARGISENSE_PH_OPERATING_STATUS_ISFET_VDS_OK (1U << 2)
#define ARGISENSE_PH_OPERATING_STATUS_REFET_VDS_OK (1U << 3)
#define ARGISENSE_PH_OPERATING_STATUS_PH_VALID (1U << 4)
#define ARGISENSE_PH_OPERATING_STATUS_TEMP_COMP_USED (1U << 5)

struct argisense_ph_measurement_sample {
	int32_t ph_x1000;
	int32_t temperature_centi_c;
	int32_t ph_raw_uv;
	int32_t vgs_isfet_uv;
	int32_t vgs_refet_uv;
	int32_t vs_isfet_uv;
	int32_t vs_refet_uv;
	int32_t gate_uv;
	int32_t isfet_drain_uv;
	int32_t refet_drain_uv;
	int32_t isfet_bulk_uv;
	int32_t isfet_subs_uv;
	int32_t refet_bulk_uv;
	int32_t refet_subs_uv;
	int32_t pt1000_uv;
	int32_t dac0_current_ua;
	int32_t dac1_current_ua;
	int32_t ph_last_error;
	int32_t temperature_last_error;
	uint32_t sequence;
	uint32_t uptime_seconds;
	uint16_t ph_adc_status;
	uint16_t bias_dac_status;
	uint16_t ph_operating_status;
	bool ph_valid;
	bool temperature_valid;
	bool sample_ready;
};

int argisense_register_read_holding(uint16_t addr, uint16_t *reg);

int argisense_register_write_holding(uint16_t addr, uint16_t reg);

const char *argisense_register_holding_name(uint16_t addr);

void argisense_register_update_sample(
	const struct argisense_ph_measurement_sample *sample);

int argisense_register_apply_runtime_outputs(void);

#endif /* ARGISENSE_PH_REGISTERS_H_ */
