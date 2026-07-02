/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_PH_SETTINGS_H_
#define ARGISENSE_PH_SETTINGS_H_

#include <stdint.h>

#define ARGISENSE_SETTINGS_SCHEMA_VERSION 2U
#define ARGISENSE_MODBUS_ADDRESS_MIN 1U
#define ARGISENSE_MODBUS_ADDRESS_MAX 247U
#define ARGISENSE_MEASUREMENT_PERIOD_SECONDS_MIN 1U
#define ARGISENSE_MEASUREMENT_WINDOW_MS_MIN 1U
#define ARGISENSE_MEASUREMENT_WINDOW_MS_MAX 60000U
#define ARGISENSE_RS485_PARITY_NONE 0U
#define ARGISENSE_RS485_PARITY_ODD 1U
#define ARGISENSE_RS485_PARITY_EVEN 2U
#define ARGISENSE_RS485_STOP_BITS_1 1U
#define ARGISENSE_RS485_STOP_BITS_2 2U
#define ARGISENSE_RS485_DATA_BITS_8 8U
#define ARGISENSE_PH_CAL_MODE_DISABLED 0U
#define ARGISENSE_PH_CAL_MODE_ISFET_REFET 1U
#define ARGISENSE_PH_CAL_MODE_REFERENCE_ELECTRODE 2U

struct argisense_runtime_config {
	uint16_t schema_version;
	uint16_t struct_size;
	uint32_t measurement_period_seconds;
	uint32_t measurement_window_ms;
	int32_t ph_range_low_x1000;
	int32_t ph_range_high_x1000;
	int32_t temperature_range_low_centi_c;
	int32_t temperature_range_high_centi_c;
	int32_t ph_slope_x1000;
	int32_t ph_offset_x1000;
	int32_t ph7_raw_uv;
	int32_t ph_slope_uv_per_ph;
	int32_t ph_temp_reference_centi_c;
	int32_t ph_temp_coeff_ppm_per_c;
	int32_t dac_min_current_ua;
	int32_t dac_max_current_ua;
	int32_t dac_fault_current_ua;
	int32_t dac0_4ma_trim_ua;
	int32_t dac0_20ma_trim_ua;
	int32_t dac1_4ma_trim_ua;
	int32_t dac1_20ma_trim_ua;
	uint32_t rs485_baudrate;
	uint8_t modbus_address;
	uint8_t rs485_termination_enabled;
	uint8_t rs485_parity;
	uint8_t rs485_stop_bits;
	uint8_t rs485_data_bits;
	uint8_t ph_calibration_mode;
	uint8_t ph_calibration_valid;
	uint8_t reserved[1];
};

int argisense_settings_init(void);

const struct argisense_runtime_config *argisense_settings_get(void);

void argisense_settings_get_copy(struct argisense_runtime_config *config);

int argisense_settings_save(const struct argisense_runtime_config *config);

int argisense_settings_reset_defaults(void);

void argisense_settings_log_summary(void);

#endif /* ARGISENSE_PH_SETTINGS_H_ */
