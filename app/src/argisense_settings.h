/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_SETTINGS_H_
#define ARGISENSE_SETTINGS_H_

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

struct argisense_runtime_config {
	uint16_t schema_version;
	uint16_t struct_size;
	uint32_t measurement_period_seconds;
	uint32_t measurement_window_ms;
	uint32_t methane_warmup_seconds;
	uint32_t methane_read_period_ms;
	uint32_t humidity_read_period_seconds;
	int32_t methane_dac_range_low_ppm;
	int32_t methane_dac_range_high_ppm;
	int32_t pressure_dac_range_low_pa;
	int32_t pressure_dac_range_high_pa;
	int32_t dac_min_current_ua;
	int32_t dac_max_current_ua;
	int32_t dac_fault_current_ua;
	int32_t methane_zero_offset_ppm_x100;
	int32_t pressure_offset_pa;
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
	uint8_t reserved[3];
};

int argisense_settings_init(void);

const struct argisense_runtime_config *argisense_settings_get(void);

int argisense_settings_save(const struct argisense_runtime_config *config);

int argisense_settings_reset_defaults(void);

void argisense_settings_log_summary(void);

#endif /* ARGISENSE_SETTINGS_H_ */
