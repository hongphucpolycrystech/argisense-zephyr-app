/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "argisense_registers.h"

#include "argisense_dfu.h"
#include "argisense_settings.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

LOG_MODULE_REGISTER(argisense_registers, LOG_LEVEL_INF);

enum argisense_holding_register {
	ARGISENSE_REG_DEVICE_ID = 0,
	ARGISENSE_REG_MAP_VERSION = 1,
	ARGISENSE_REG_STATUS_FLAGS = 2,
	ARGISENSE_REG_MODBUS_ADDRESS = 3,
	ARGISENSE_REG_BAUDRATE_HI = 4,
	ARGISENSE_REG_BAUDRATE_LO = 5,
	ARGISENSE_REG_MEASUREMENT_PERIOD_SECONDS = 6,
	ARGISENSE_REG_MEASUREMENT_WINDOW_MS = 7,
	ARGISENSE_REG_BAUD_PRESET = 8,
	ARGISENSE_REG_TERMINATION_ENABLED = 9,
	ARGISENSE_REG_METHANE_PPM_X100_HI = 10,
	ARGISENSE_REG_METHANE_PPM_X100_LO = 11,
	ARGISENSE_REG_PRESSURE_PA_HI = 12,
	ARGISENSE_REG_PRESSURE_PA_LO = 13,
	ARGISENSE_REG_DAC0_CURRENT_UA = 14,
	ARGISENSE_REG_DAC1_CURRENT_UA = 15,
	ARGISENSE_REG_SAMPLE_SEQUENCE_HI = 16,
	ARGISENSE_REG_SAMPLE_SEQUENCE_LO = 17,
	ARGISENSE_REG_SAMPLE_UPTIME_SECONDS_HI = 18,
	ARGISENSE_REG_SAMPLE_UPTIME_SECONDS_LO = 19,
	ARGISENSE_REG_FW_VERSION_MAJOR = 20,
	ARGISENSE_REG_FW_VERSION_MINOR = 21,
	ARGISENSE_REG_FW_VERSION_PATCH = 22,
	ARGISENSE_REG_FW_VERSION_BUILD_HI = 23,
	ARGISENSE_REG_FW_VERSION_BUILD_LO = 24,
	ARGISENSE_REG_BOOT_FLAGS = 25,
	ARGISENSE_REG_ACTIVE_SLOT = 26,
	ARGISENSE_REG_REBOOT_REQUIRED = 27,
	ARGISENSE_REG_LAST_COMMAND = 28,
	ARGISENSE_REG_LAST_COMMAND_RESULT = 29,
	ARGISENSE_REG_DAC_MIN_CURRENT_UA = 30,
	ARGISENSE_REG_DAC_MAX_CURRENT_UA = 31,
	ARGISENSE_REG_DAC_FAULT_CURRENT_UA = 32,
	ARGISENSE_REG_COMMAND = 33,
	ARGISENSE_REG_RS485_PARITY = 34,
	ARGISENSE_REG_RS485_STOP_BITS = 35,
	ARGISENSE_REG_RS485_DATA_BITS = 36,
	ARGISENSE_REG_METHANE_RANGE_LOW_HI = 40,
	ARGISENSE_REG_METHANE_RANGE_LOW_LO = 41,
	ARGISENSE_REG_METHANE_RANGE_HIGH_HI = 42,
	ARGISENSE_REG_METHANE_RANGE_HIGH_LO = 43,
	ARGISENSE_REG_PRESSURE_RANGE_LOW_HI = 44,
	ARGISENSE_REG_PRESSURE_RANGE_LOW_LO = 45,
	ARGISENSE_REG_PRESSURE_RANGE_HIGH_HI = 46,
	ARGISENSE_REG_PRESSURE_RANGE_HIGH_LO = 47,
	ARGISENSE_REG_METHANE_ZERO_OFFSET_HI = 48,
	ARGISENSE_REG_METHANE_ZERO_OFFSET_LO = 49,
	ARGISENSE_REG_PRESSURE_OFFSET_HI = 50,
	ARGISENSE_REG_PRESSURE_OFFSET_LO = 51,
	ARGISENSE_REG_DAC0_4MA_TRIM_HI = 52,
	ARGISENSE_REG_DAC0_4MA_TRIM_LO = 53,
	ARGISENSE_REG_DAC0_20MA_TRIM_HI = 54,
	ARGISENSE_REG_DAC0_20MA_TRIM_LO = 55,
	ARGISENSE_REG_DAC1_4MA_TRIM_HI = 56,
	ARGISENSE_REG_DAC1_4MA_TRIM_LO = 57,
	ARGISENSE_REG_DAC1_20MA_TRIM_HI = 58,
	ARGISENSE_REG_DAC1_20MA_TRIM_LO = 59,
	ARGISENSE_REG_METHANE_STATUS_FLAGS = 70,
	ARGISENSE_REG_METHANE_PROTOCOL_VERSION = 71,
	ARGISENSE_REG_METHANE_LAST_ERROR = 72,
	ARGISENSE_REG_PRESSURE_LAST_ERROR = 73,
	ARGISENSE_REG_PRESSURE_TEMP_CENTI_C = 74,
	ARGISENSE_REG_PRESSURE_D1_RAW_HI = 75,
	ARGISENSE_REG_PRESSURE_D1_RAW_LO = 76,
	ARGISENSE_REG_PRESSURE_D2_RAW_HI = 77,
	ARGISENSE_REG_PRESSURE_D2_RAW_LO = 78,
	ARGISENSE_REG_PRESSURE_PROM_CRC = 79,
	ARGISENSE_REG_HUMIDITY_RH_X100 = 80,
	ARGISENSE_REG_HUMIDITY_TEMP_CENTI_C = 81,
	ARGISENSE_REG_HUMIDITY_LAST_ERROR = 82,
};

#define ARGISENSE_DEVICE_ID 0xA651U
#define ARGISENSE_BAUD_PRESET_CUSTOM 0xFFFFU
#define ARGISENSE_ACTIVE_SLOT_UNKNOWN 0xFFFFU

#define ARGISENSE_REGISTER_COMMAND_NONE           0x0000U
#define ARGISENSE_REGISTER_COMMAND_REBOOT         0xA551U
#define ARGISENSE_REGISTER_COMMAND_RESET_DEFAULTS 0xA552U
#define ARGISENSE_REGISTER_COMMAND_CONFIRM_IMAGE  0xA553U

#define ARGISENSE_STATUS_METHANE_VALID BIT(0)
#define ARGISENSE_STATUS_PRESSURE_VALID BIT(1)
#define ARGISENSE_STATUS_SAMPLE_READY BIT(2)
#define ARGISENSE_STATUS_TERMINATION_ENABLED BIT(3)
#define ARGISENSE_STATUS_HUMIDITY_VALID BIT(4)
#define ARGISENSE_STATUS_HUMIDITY_ERROR BIT(5)

#define ARGISENSE_BOOT_FLAG_MCUBOOT_ENABLED BIT(0)
#define ARGISENSE_BOOT_FLAG_IMAGE_CONFIRMED BIT(1)
#define ARGISENSE_BOOT_FLAG_INFO_VALID BIT(2)
#define ARGISENSE_BOOT_FLAG_REBOOT_REQUIRED BIT(3)

struct argisense_register_snapshot {
	int32_t methane_ppm_x100;
	int32_t pressure_pa;
	int32_t dac0_current_ua;
	int32_t dac1_current_ua;
	int32_t methane_last_error;
	int32_t pressure_last_error;
	int32_t humidity_last_error;
	int32_t pressure_temperature_centi_c;
	int32_t humidity_rh_x100;
	int32_t humidity_temperature_centi_c;
	uint32_t pressure_d1_raw;
	uint32_t pressure_d2_raw;
	uint32_t sequence;
	uint32_t uptime_seconds;
	uint16_t methane_status_flags;
	uint16_t methane_protocol_version;
	uint8_t pressure_prom_crc_read;
	uint8_t pressure_prom_crc_calc;
	bool methane_valid;
	bool pressure_valid;
	bool humidity_valid;
	bool sample_ready;
};

struct argisense_firmware_info {
	uint16_t major;
	uint16_t minor;
	uint16_t patch;
	uint32_t build;
	uint16_t boot_flags;
	uint16_t active_slot;
};

static struct argisense_register_snapshot sample_snapshot;
static struct k_spinlock sample_lock;
static bool reboot_required;
static uint16_t last_command;
static int16_t last_command_result;

static uint16_t reg_u32_hi(uint32_t value)
{
	return (uint16_t)(value >> 16);
}

static uint16_t reg_u32_lo(uint32_t value)
{
	return (uint16_t)value;
}

static uint16_t reg_i32_hi(int32_t value)
{
	return reg_u32_hi((uint32_t)value);
}

static uint16_t reg_i32_lo(int32_t value)
{
	return reg_u32_lo((uint32_t)value);
}

static int32_t reg_i32_replace_hi(int32_t current, uint16_t hi)
{
	return (int32_t)((((uint32_t)hi) << 16) | ((uint32_t)current & 0xffffU));
}

static int32_t reg_i32_replace_lo(int32_t current, uint16_t lo)
{
	return (int32_t)(((uint32_t)current & 0xffff0000U) | lo);
}

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Rebooting by RS485 command");
	sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

static void firmware_info_get(struct argisense_firmware_info *info)
{
	*info = (struct argisense_firmware_info){
		.active_slot = ARGISENSE_ACTIVE_SLOT_UNKNOWN,
	};

	if (reboot_required) {
		info->boot_flags |= ARGISENSE_BOOT_FLAG_REBOOT_REQUIRED;
	}

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	struct mcuboot_img_header header;
	const struct mcuboot_img_sem_ver *version;
	uint8_t active_slot;
	int ret;

	info->boot_flags |= ARGISENSE_BOOT_FLAG_MCUBOOT_ENABLED;

	if (boot_is_img_confirmed()) {
		info->boot_flags |= ARGISENSE_BOOT_FLAG_IMAGE_CONFIRMED;
	}

	active_slot = boot_fetch_active_slot();
	info->active_slot = active_slot;

	ret = boot_read_bank_header(active_slot, &header, sizeof(header));
	if (ret < 0 || header.mcuboot_version != 1U) {
		return;
	}

	version = &header.h.v1.sem_ver;
	info->major = version->major;
	info->minor = version->minor;
	info->patch = version->revision;
	info->build = version->build_num;
	info->boot_flags |= ARGISENSE_BOOT_FLAG_INFO_VALID;
#endif
}

static uint16_t baud_preset_from_baud(uint32_t baudrate)
{
	switch (baudrate) {
	case 9600U:
		return 0U;
	case 19200U:
		return 1U;
	case 38400U:
		return 2U;
	case 57600U:
		return 3U;
	case 115200U:
		return 4U;
	default:
		return ARGISENSE_BAUD_PRESET_CUSTOM;
	}
}

static int baud_from_preset(uint16_t preset, uint32_t *baudrate)
{
	switch (preset) {
	case 0U:
		*baudrate = 9600U;
		return 0;
	case 1U:
		*baudrate = 19200U;
		return 0;
	case 2U:
		*baudrate = 38400U;
		return 0;
	case 3U:
		*baudrate = 57600U;
		return 0;
	case 4U:
		*baudrate = 115200U;
		return 0;
	default:
		return -EINVAL;
	}
}

static uint16_t status_flags(
	const struct argisense_register_snapshot *snapshot,
	const struct argisense_runtime_config *config)
{
	uint16_t flags = 0U;

	if (snapshot->methane_valid) {
		flags |= ARGISENSE_STATUS_METHANE_VALID;
	}

	if (snapshot->pressure_valid) {
		flags |= ARGISENSE_STATUS_PRESSURE_VALID;
	}

	if (snapshot->humidity_valid) {
		flags |= ARGISENSE_STATUS_HUMIDITY_VALID;
	}

	if (snapshot->humidity_last_error < 0) {
		flags |= ARGISENSE_STATUS_HUMIDITY_ERROR;
	}

	if (snapshot->sample_ready) {
		flags |= ARGISENSE_STATUS_SAMPLE_READY;
	}

	if (config->rs485_termination_enabled != 0U) {
		flags |= ARGISENSE_STATUS_TERMINATION_ENABLED;
	}

	return flags;
}

static struct argisense_register_snapshot sample_get(void)
{
	struct argisense_register_snapshot snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&sample_lock);
	snapshot = sample_snapshot;
	k_spin_unlock(&sample_lock, key);

	return snapshot;
}

int argisense_register_read_holding(uint16_t addr, uint16_t *reg)
{
	const struct argisense_runtime_config *config = argisense_settings_get();
	const struct argisense_register_snapshot snapshot = sample_get();
	struct argisense_firmware_info firmware_info;

	if (reg == NULL) {
		return -EINVAL;
	}

	firmware_info_get(&firmware_info);

	if (argisense_dfu_register_is_supported(addr)) {
		return argisense_dfu_register_read(addr, reg);
	}

	switch (addr) {
	case ARGISENSE_REG_DEVICE_ID:
		*reg = ARGISENSE_DEVICE_ID;
		return 0;
	case ARGISENSE_REG_MAP_VERSION:
		*reg = ARGISENSE_REGISTER_MAP_VERSION;
		return 0;
	case ARGISENSE_REG_STATUS_FLAGS:
		*reg = status_flags(&snapshot, config);
		return 0;
	case ARGISENSE_REG_MODBUS_ADDRESS:
		*reg = config->modbus_address;
		return 0;
	case ARGISENSE_REG_BAUDRATE_HI:
		*reg = reg_u32_hi(config->rs485_baudrate);
		return 0;
	case ARGISENSE_REG_BAUDRATE_LO:
		*reg = reg_u32_lo(config->rs485_baudrate);
		return 0;
	case ARGISENSE_REG_MEASUREMENT_PERIOD_SECONDS:
		*reg = (uint16_t)MIN(config->measurement_period_seconds,
				      UINT16_MAX);
		return 0;
	case ARGISENSE_REG_MEASUREMENT_WINDOW_MS:
		*reg = (uint16_t)MIN(config->measurement_window_ms,
				      UINT16_MAX);
		return 0;
	case ARGISENSE_REG_BAUD_PRESET:
		*reg = baud_preset_from_baud(config->rs485_baudrate);
		return 0;
	case ARGISENSE_REG_TERMINATION_ENABLED:
		*reg = config->rs485_termination_enabled != 0U ? 1U : 0U;
		return 0;
	case ARGISENSE_REG_METHANE_PPM_X100_HI:
		*reg = reg_i32_hi(snapshot.methane_ppm_x100);
		return 0;
	case ARGISENSE_REG_METHANE_PPM_X100_LO:
		*reg = reg_i32_lo(snapshot.methane_ppm_x100);
		return 0;
	case ARGISENSE_REG_PRESSURE_PA_HI:
		*reg = reg_i32_hi(snapshot.pressure_pa);
		return 0;
	case ARGISENSE_REG_PRESSURE_PA_LO:
		*reg = reg_i32_lo(snapshot.pressure_pa);
		return 0;
	case ARGISENSE_REG_DAC0_CURRENT_UA:
		*reg = (uint16_t)snapshot.dac0_current_ua;
		return 0;
	case ARGISENSE_REG_DAC1_CURRENT_UA:
		*reg = (uint16_t)snapshot.dac1_current_ua;
		return 0;
	case ARGISENSE_REG_SAMPLE_SEQUENCE_HI:
		*reg = reg_u32_hi(snapshot.sequence);
		return 0;
	case ARGISENSE_REG_SAMPLE_SEQUENCE_LO:
		*reg = reg_u32_lo(snapshot.sequence);
		return 0;
	case ARGISENSE_REG_SAMPLE_UPTIME_SECONDS_HI:
		*reg = reg_u32_hi(snapshot.uptime_seconds);
		return 0;
	case ARGISENSE_REG_SAMPLE_UPTIME_SECONDS_LO:
		*reg = reg_u32_lo(snapshot.uptime_seconds);
		return 0;
	case ARGISENSE_REG_FW_VERSION_MAJOR:
		*reg = firmware_info.major;
		return 0;
	case ARGISENSE_REG_FW_VERSION_MINOR:
		*reg = firmware_info.minor;
		return 0;
	case ARGISENSE_REG_FW_VERSION_PATCH:
		*reg = firmware_info.patch;
		return 0;
	case ARGISENSE_REG_FW_VERSION_BUILD_HI:
		*reg = reg_u32_hi(firmware_info.build);
		return 0;
	case ARGISENSE_REG_FW_VERSION_BUILD_LO:
		*reg = reg_u32_lo(firmware_info.build);
		return 0;
	case ARGISENSE_REG_BOOT_FLAGS:
		*reg = firmware_info.boot_flags;
		return 0;
	case ARGISENSE_REG_ACTIVE_SLOT:
		*reg = firmware_info.active_slot;
		return 0;
	case ARGISENSE_REG_REBOOT_REQUIRED:
		*reg = reboot_required ? 1U : 0U;
		return 0;
	case ARGISENSE_REG_LAST_COMMAND:
		*reg = last_command;
		return 0;
	case ARGISENSE_REG_LAST_COMMAND_RESULT:
		*reg = (uint16_t)last_command_result;
		return 0;
	case ARGISENSE_REG_DAC_MIN_CURRENT_UA:
		*reg = (uint16_t)config->dac_min_current_ua;
		return 0;
	case ARGISENSE_REG_DAC_MAX_CURRENT_UA:
		*reg = (uint16_t)config->dac_max_current_ua;
		return 0;
	case ARGISENSE_REG_DAC_FAULT_CURRENT_UA:
		*reg = (uint16_t)config->dac_fault_current_ua;
		return 0;
	case ARGISENSE_REG_COMMAND:
		*reg = ARGISENSE_REGISTER_COMMAND_NONE;
		return 0;
	case ARGISENSE_REG_RS485_PARITY:
		*reg = config->rs485_parity;
		return 0;
	case ARGISENSE_REG_RS485_STOP_BITS:
		*reg = config->rs485_stop_bits;
		return 0;
	case ARGISENSE_REG_RS485_DATA_BITS:
		*reg = config->rs485_data_bits;
		return 0;
	case ARGISENSE_REG_METHANE_RANGE_LOW_HI:
		*reg = reg_i32_hi(config->methane_dac_range_low_ppm);
		return 0;
	case ARGISENSE_REG_METHANE_RANGE_LOW_LO:
		*reg = reg_i32_lo(config->methane_dac_range_low_ppm);
		return 0;
	case ARGISENSE_REG_METHANE_RANGE_HIGH_HI:
		*reg = reg_i32_hi(config->methane_dac_range_high_ppm);
		return 0;
	case ARGISENSE_REG_METHANE_RANGE_HIGH_LO:
		*reg = reg_i32_lo(config->methane_dac_range_high_ppm);
		return 0;
	case ARGISENSE_REG_PRESSURE_RANGE_LOW_HI:
		*reg = reg_i32_hi(config->pressure_dac_range_low_pa);
		return 0;
	case ARGISENSE_REG_PRESSURE_RANGE_LOW_LO:
		*reg = reg_i32_lo(config->pressure_dac_range_low_pa);
		return 0;
	case ARGISENSE_REG_PRESSURE_RANGE_HIGH_HI:
		*reg = reg_i32_hi(config->pressure_dac_range_high_pa);
		return 0;
	case ARGISENSE_REG_PRESSURE_RANGE_HIGH_LO:
		*reg = reg_i32_lo(config->pressure_dac_range_high_pa);
		return 0;
	case ARGISENSE_REG_METHANE_ZERO_OFFSET_HI:
		*reg = reg_i32_hi(config->methane_zero_offset_ppm_x100);
		return 0;
	case ARGISENSE_REG_METHANE_ZERO_OFFSET_LO:
		*reg = reg_i32_lo(config->methane_zero_offset_ppm_x100);
		return 0;
	case ARGISENSE_REG_PRESSURE_OFFSET_HI:
		*reg = reg_i32_hi(config->pressure_offset_pa);
		return 0;
	case ARGISENSE_REG_PRESSURE_OFFSET_LO:
		*reg = reg_i32_lo(config->pressure_offset_pa);
		return 0;
	case ARGISENSE_REG_DAC0_4MA_TRIM_HI:
		*reg = reg_i32_hi(config->dac0_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC0_4MA_TRIM_LO:
		*reg = reg_i32_lo(config->dac0_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC0_20MA_TRIM_HI:
		*reg = reg_i32_hi(config->dac0_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC0_20MA_TRIM_LO:
		*reg = reg_i32_lo(config->dac0_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_4MA_TRIM_HI:
		*reg = reg_i32_hi(config->dac1_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_4MA_TRIM_LO:
		*reg = reg_i32_lo(config->dac1_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_20MA_TRIM_HI:
		*reg = reg_i32_hi(config->dac1_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_20MA_TRIM_LO:
		*reg = reg_i32_lo(config->dac1_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_METHANE_STATUS_FLAGS:
		*reg = snapshot.methane_status_flags;
		return 0;
	case ARGISENSE_REG_METHANE_PROTOCOL_VERSION:
		*reg = snapshot.methane_protocol_version;
		return 0;
	case ARGISENSE_REG_METHANE_LAST_ERROR:
		*reg = (uint16_t)snapshot.methane_last_error;
		return 0;
	case ARGISENSE_REG_PRESSURE_LAST_ERROR:
		*reg = (uint16_t)snapshot.pressure_last_error;
		return 0;
	case ARGISENSE_REG_PRESSURE_TEMP_CENTI_C:
		*reg = (uint16_t)snapshot.pressure_temperature_centi_c;
		return 0;
	case ARGISENSE_REG_PRESSURE_D1_RAW_HI:
		*reg = reg_u32_hi(snapshot.pressure_d1_raw);
		return 0;
	case ARGISENSE_REG_PRESSURE_D1_RAW_LO:
		*reg = reg_u32_lo(snapshot.pressure_d1_raw);
		return 0;
	case ARGISENSE_REG_PRESSURE_D2_RAW_HI:
		*reg = reg_u32_hi(snapshot.pressure_d2_raw);
		return 0;
	case ARGISENSE_REG_PRESSURE_D2_RAW_LO:
		*reg = reg_u32_lo(snapshot.pressure_d2_raw);
		return 0;
	case ARGISENSE_REG_PRESSURE_PROM_CRC:
		*reg = ((uint16_t)snapshot.pressure_prom_crc_read << 8) |
		       snapshot.pressure_prom_crc_calc;
		return 0;
	case ARGISENSE_REG_HUMIDITY_RH_X100:
		*reg = (uint16_t)snapshot.humidity_rh_x100;
		return 0;
	case ARGISENSE_REG_HUMIDITY_TEMP_CENTI_C:
		*reg = (uint16_t)snapshot.humidity_temperature_centi_c;
		return 0;
	case ARGISENSE_REG_HUMIDITY_LAST_ERROR:
		*reg = (uint16_t)snapshot.humidity_last_error;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int save_config(const struct argisense_runtime_config *config,
		       const char *field_name)
{
	int ret;

	ret = argisense_settings_save(config);
	if (ret < 0) {
		LOG_WRN("Rejected register write to %s: %d", field_name, ret);
		return ret;
	}

	LOG_INF("Updated persistent setting from register write: %s",
		field_name);

	return 0;
}

static int save_i32_config_word(struct argisense_runtime_config *config,
				int32_t *field, uint16_t reg, bool high_word,
				const char *field_name)
{
	if (high_word) {
		*field = reg_i32_replace_hi(*field, reg);
	} else {
		*field = reg_i32_replace_lo(*field, reg);
	}

	return save_config(config, field_name);
}

static int handle_command(uint16_t command)
{
	int ret = 0;

	last_command = command;

	switch (command) {
	case ARGISENSE_REGISTER_COMMAND_NONE:
		ret = 0;
		break;
	case ARGISENSE_REGISTER_COMMAND_REBOOT:
		ret = k_work_schedule(&reboot_work, K_MSEC(500));
		if (ret >= 0) {
			ret = 0;
		}
		break;
	case ARGISENSE_REGISTER_COMMAND_RESET_DEFAULTS:
		ret = argisense_settings_reset_defaults();
		if (ret == 0) {
			reboot_required = true;
			LOG_INF("Settings reset to defaults by RS485 command");
		}
		break;
	case ARGISENSE_REGISTER_COMMAND_CONFIRM_IMAGE:
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
		ret = boot_write_img_confirmed();
		if (ret == 0) {
			LOG_INF("MCUboot image confirmed by RS485 command");
		}
#else
		ret = -ENOTSUP;
#endif
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	last_command_result = (int16_t)ret;

	return ret;
}

int argisense_register_write_holding(uint16_t addr, uint16_t reg)
{
	struct argisense_runtime_config config = *argisense_settings_get();
	uint32_t baudrate;
	int ret;

	if (argisense_dfu_register_is_supported(addr)) {
		return argisense_dfu_register_write(addr, reg);
	}

	switch (addr) {
	case ARGISENSE_REG_MODBUS_ADDRESS:
		if (reg < ARGISENSE_MODBUS_ADDRESS_MIN ||
		    reg > ARGISENSE_MODBUS_ADDRESS_MAX) {
			return -EINVAL;
		}

		config.modbus_address = (uint8_t)reg;
		ret = save_config(&config, "modbus-address");
		if (ret == 0) {
			reboot_required = true;
			LOG_INF("New Modbus address will be used after reboot");
		}
		return ret;
	case ARGISENSE_REG_MEASUREMENT_PERIOD_SECONDS:
		if (reg < ARGISENSE_MEASUREMENT_PERIOD_SECONDS_MIN) {
			return -EINVAL;
		}

		config.measurement_period_seconds = reg;
		return save_config(&config, "measurement-period-seconds");
	case ARGISENSE_REG_MEASUREMENT_WINDOW_MS:
		if (reg < ARGISENSE_MEASUREMENT_WINDOW_MS_MIN ||
		    reg > ARGISENSE_MEASUREMENT_WINDOW_MS_MAX) {
			return -EINVAL;
		}

		config.measurement_window_ms = reg;
		return save_config(&config, "measurement-window-ms");
	case ARGISENSE_REG_BAUD_PRESET:
		ret = baud_from_preset(reg, &baudrate);
		if (ret < 0) {
			return ret;
		}

		config.rs485_baudrate = baudrate;
		ret = save_config(&config, "rs485-baudrate");
		if (ret == 0) {
			reboot_required = true;
			LOG_INF("New RS485 baudrate will be used after reboot");
		}
		return ret;
	case ARGISENSE_REG_TERMINATION_ENABLED:
		if (reg > 1U) {
			return -EINVAL;
		}

		config.rs485_termination_enabled = (uint8_t)reg;
		return save_config(&config, "rs485-termination-enabled");
	case ARGISENSE_REG_DAC_MIN_CURRENT_UA:
		config.dac_min_current_ua = reg;
		return save_config(&config, "dac-min-current-ua");
	case ARGISENSE_REG_DAC_MAX_CURRENT_UA:
		config.dac_max_current_ua = reg;
		return save_config(&config, "dac-max-current-ua");
	case ARGISENSE_REG_DAC_FAULT_CURRENT_UA:
		config.dac_fault_current_ua = reg;
		return save_config(&config, "dac-fault-current-ua");
	case ARGISENSE_REG_COMMAND:
		return handle_command(reg);
	case ARGISENSE_REG_RS485_PARITY:
		if (reg > ARGISENSE_RS485_PARITY_EVEN) {
			return -EINVAL;
		}

		config.rs485_parity = (uint8_t)reg;
		ret = save_config(&config, "rs485-parity");
		if (ret == 0) {
			reboot_required = true;
			LOG_INF("New RS485 parity will be used after reboot");
		}
		return ret;
	case ARGISENSE_REG_RS485_STOP_BITS:
		if (reg != ARGISENSE_RS485_STOP_BITS_1 &&
		    reg != ARGISENSE_RS485_STOP_BITS_2) {
			return -EINVAL;
		}

		config.rs485_stop_bits = (uint8_t)reg;
		ret = save_config(&config, "rs485-stop-bits");
		if (ret == 0) {
			reboot_required = true;
			LOG_INF("New RS485 stop-bit setting will be used after reboot");
		}
		return ret;
	case ARGISENSE_REG_RS485_DATA_BITS:
		if (reg != ARGISENSE_RS485_DATA_BITS_8) {
			return -EINVAL;
		}

		config.rs485_data_bits = (uint8_t)reg;
		ret = save_config(&config, "rs485-data-bits");
		if (ret == 0) {
			reboot_required = true;
			LOG_INF("New RS485 data-bit setting will be used after reboot");
		}
		return ret;
	case ARGISENSE_REG_METHANE_RANGE_LOW_HI:
		return save_i32_config_word(&config,
					    &config.methane_dac_range_low_ppm,
					    reg, true, "methane-range-low-ppm");
	case ARGISENSE_REG_METHANE_RANGE_LOW_LO:
		return save_i32_config_word(&config,
					    &config.methane_dac_range_low_ppm,
					    reg, false, "methane-range-low-ppm");
	case ARGISENSE_REG_METHANE_RANGE_HIGH_HI:
		return save_i32_config_word(&config,
					    &config.methane_dac_range_high_ppm,
					    reg, true, "methane-range-high-ppm");
	case ARGISENSE_REG_METHANE_RANGE_HIGH_LO:
		return save_i32_config_word(&config,
					    &config.methane_dac_range_high_ppm,
					    reg, false, "methane-range-high-ppm");
	case ARGISENSE_REG_PRESSURE_RANGE_LOW_HI:
		return save_i32_config_word(&config,
					    &config.pressure_dac_range_low_pa,
					    reg, true, "pressure-range-low-pa");
	case ARGISENSE_REG_PRESSURE_RANGE_LOW_LO:
		return save_i32_config_word(&config,
					    &config.pressure_dac_range_low_pa,
					    reg, false, "pressure-range-low-pa");
	case ARGISENSE_REG_PRESSURE_RANGE_HIGH_HI:
		return save_i32_config_word(&config,
					    &config.pressure_dac_range_high_pa,
					    reg, true, "pressure-range-high-pa");
	case ARGISENSE_REG_PRESSURE_RANGE_HIGH_LO:
		return save_i32_config_word(&config,
					    &config.pressure_dac_range_high_pa,
					    reg, false, "pressure-range-high-pa");
	case ARGISENSE_REG_METHANE_ZERO_OFFSET_HI:
		return save_i32_config_word(&config,
					    &config.methane_zero_offset_ppm_x100,
					    reg, true, "methane-zero-offset");
	case ARGISENSE_REG_METHANE_ZERO_OFFSET_LO:
		return save_i32_config_word(&config,
					    &config.methane_zero_offset_ppm_x100,
					    reg, false, "methane-zero-offset");
	case ARGISENSE_REG_PRESSURE_OFFSET_HI:
		return save_i32_config_word(&config, &config.pressure_offset_pa,
					    reg, true, "pressure-offset-pa");
	case ARGISENSE_REG_PRESSURE_OFFSET_LO:
		return save_i32_config_word(&config, &config.pressure_offset_pa,
					    reg, false, "pressure-offset-pa");
	case ARGISENSE_REG_DAC0_4MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac0_4ma_trim_ua,
					    reg, true, "dac0-4ma-trim-ua");
	case ARGISENSE_REG_DAC0_4MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac0_4ma_trim_ua,
					    reg, false, "dac0-4ma-trim-ua");
	case ARGISENSE_REG_DAC0_20MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac0_20ma_trim_ua,
					    reg, true, "dac0-20ma-trim-ua");
	case ARGISENSE_REG_DAC0_20MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac0_20ma_trim_ua,
					    reg, false, "dac0-20ma-trim-ua");
	case ARGISENSE_REG_DAC1_4MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac1_4ma_trim_ua,
					    reg, true, "dac1-4ma-trim-ua");
	case ARGISENSE_REG_DAC1_4MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac1_4ma_trim_ua,
					    reg, false, "dac1-4ma-trim-ua");
	case ARGISENSE_REG_DAC1_20MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac1_20ma_trim_ua,
					    reg, true, "dac1-20ma-trim-ua");
	case ARGISENSE_REG_DAC1_20MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac1_20ma_trim_ua,
					    reg, false, "dac1-20ma-trim-ua");
	default:
		return -ENOTSUP;
	}
}

const char *argisense_register_holding_name(uint16_t addr)
{
	const char *dfu_name = argisense_dfu_register_name(addr);

	if (dfu_name != NULL) {
		return dfu_name;
	}

	switch (addr) {
	case ARGISENSE_REG_DEVICE_ID:
		return "device_id";
	case ARGISENSE_REG_MAP_VERSION:
		return "map_version";
	case ARGISENSE_REG_STATUS_FLAGS:
		return "status_flags";
	case ARGISENSE_REG_MODBUS_ADDRESS:
		return "modbus_address";
	case ARGISENSE_REG_BAUDRATE_HI:
		return "baudrate_hi";
	case ARGISENSE_REG_BAUDRATE_LO:
		return "baudrate_lo";
	case ARGISENSE_REG_MEASUREMENT_PERIOD_SECONDS:
		return "measurement_period_s";
	case ARGISENSE_REG_MEASUREMENT_WINDOW_MS:
		return "measurement_window_ms";
	case ARGISENSE_REG_BAUD_PRESET:
		return "baud_preset";
	case ARGISENSE_REG_TERMINATION_ENABLED:
		return "termination_enabled";
	case ARGISENSE_REG_METHANE_PPM_X100_HI:
		return "methane_ppm_x100_hi";
	case ARGISENSE_REG_METHANE_PPM_X100_LO:
		return "methane_ppm_x100_lo";
	case ARGISENSE_REG_PRESSURE_PA_HI:
		return "pressure_pa_hi";
	case ARGISENSE_REG_PRESSURE_PA_LO:
		return "pressure_pa_lo";
	case ARGISENSE_REG_DAC0_CURRENT_UA:
		return "dac0_current_ua";
	case ARGISENSE_REG_DAC1_CURRENT_UA:
		return "dac1_current_ua";
	case ARGISENSE_REG_SAMPLE_SEQUENCE_HI:
		return "sample_sequence_hi";
	case ARGISENSE_REG_SAMPLE_SEQUENCE_LO:
		return "sample_sequence_lo";
	case ARGISENSE_REG_SAMPLE_UPTIME_SECONDS_HI:
		return "sample_uptime_s_hi";
	case ARGISENSE_REG_SAMPLE_UPTIME_SECONDS_LO:
		return "sample_uptime_s_lo";
	case ARGISENSE_REG_FW_VERSION_MAJOR:
		return "fw_version_major";
	case ARGISENSE_REG_FW_VERSION_MINOR:
		return "fw_version_minor";
	case ARGISENSE_REG_FW_VERSION_PATCH:
		return "fw_version_patch";
	case ARGISENSE_REG_FW_VERSION_BUILD_HI:
		return "fw_version_build_hi";
	case ARGISENSE_REG_FW_VERSION_BUILD_LO:
		return "fw_version_build_lo";
	case ARGISENSE_REG_BOOT_FLAGS:
		return "boot_flags";
	case ARGISENSE_REG_ACTIVE_SLOT:
		return "active_slot";
	case ARGISENSE_REG_REBOOT_REQUIRED:
		return "reboot_required";
	case ARGISENSE_REG_LAST_COMMAND:
		return "last_command";
	case ARGISENSE_REG_LAST_COMMAND_RESULT:
		return "last_command_result";
	case ARGISENSE_REG_DAC_MIN_CURRENT_UA:
		return "dac_min_current_ua";
	case ARGISENSE_REG_DAC_MAX_CURRENT_UA:
		return "dac_max_current_ua";
	case ARGISENSE_REG_DAC_FAULT_CURRENT_UA:
		return "dac_fault_current_ua";
	case ARGISENSE_REG_COMMAND:
		return "command";
	case ARGISENSE_REG_RS485_PARITY:
		return "rs485_parity";
	case ARGISENSE_REG_RS485_STOP_BITS:
		return "rs485_stop_bits";
	case ARGISENSE_REG_RS485_DATA_BITS:
		return "rs485_data_bits";
	case ARGISENSE_REG_METHANE_RANGE_LOW_HI:
		return "methane_range_low_hi";
	case ARGISENSE_REG_METHANE_RANGE_LOW_LO:
		return "methane_range_low_lo";
	case ARGISENSE_REG_METHANE_RANGE_HIGH_HI:
		return "methane_range_high_hi";
	case ARGISENSE_REG_METHANE_RANGE_HIGH_LO:
		return "methane_range_high_lo";
	case ARGISENSE_REG_PRESSURE_RANGE_LOW_HI:
		return "pressure_range_low_hi";
	case ARGISENSE_REG_PRESSURE_RANGE_LOW_LO:
		return "pressure_range_low_lo";
	case ARGISENSE_REG_PRESSURE_RANGE_HIGH_HI:
		return "pressure_range_high_hi";
	case ARGISENSE_REG_PRESSURE_RANGE_HIGH_LO:
		return "pressure_range_high_lo";
	case ARGISENSE_REG_METHANE_ZERO_OFFSET_HI:
		return "methane_zero_offset_hi";
	case ARGISENSE_REG_METHANE_ZERO_OFFSET_LO:
		return "methane_zero_offset_lo";
	case ARGISENSE_REG_PRESSURE_OFFSET_HI:
		return "pressure_offset_hi";
	case ARGISENSE_REG_PRESSURE_OFFSET_LO:
		return "pressure_offset_lo";
	case ARGISENSE_REG_DAC0_4MA_TRIM_HI:
		return "dac0_4ma_trim_hi";
	case ARGISENSE_REG_DAC0_4MA_TRIM_LO:
		return "dac0_4ma_trim_lo";
	case ARGISENSE_REG_DAC0_20MA_TRIM_HI:
		return "dac0_20ma_trim_hi";
	case ARGISENSE_REG_DAC0_20MA_TRIM_LO:
		return "dac0_20ma_trim_lo";
	case ARGISENSE_REG_DAC1_4MA_TRIM_HI:
		return "dac1_4ma_trim_hi";
	case ARGISENSE_REG_DAC1_4MA_TRIM_LO:
		return "dac1_4ma_trim_lo";
	case ARGISENSE_REG_DAC1_20MA_TRIM_HI:
		return "dac1_20ma_trim_hi";
	case ARGISENSE_REG_DAC1_20MA_TRIM_LO:
		return "dac1_20ma_trim_lo";
	case ARGISENSE_REG_METHANE_STATUS_FLAGS:
		return "methane_status_flags";
	case ARGISENSE_REG_METHANE_PROTOCOL_VERSION:
		return "methane_protocol_version";
	case ARGISENSE_REG_METHANE_LAST_ERROR:
		return "methane_last_error";
	case ARGISENSE_REG_PRESSURE_LAST_ERROR:
		return "pressure_last_error";
	case ARGISENSE_REG_PRESSURE_TEMP_CENTI_C:
		return "pressure_temp_centi_c";
	case ARGISENSE_REG_PRESSURE_D1_RAW_HI:
		return "pressure_d1_raw_hi";
	case ARGISENSE_REG_PRESSURE_D1_RAW_LO:
		return "pressure_d1_raw_lo";
	case ARGISENSE_REG_PRESSURE_D2_RAW_HI:
		return "pressure_d2_raw_hi";
	case ARGISENSE_REG_PRESSURE_D2_RAW_LO:
		return "pressure_d2_raw_lo";
	case ARGISENSE_REG_PRESSURE_PROM_CRC:
		return "pressure_prom_crc";
	case ARGISENSE_REG_HUMIDITY_RH_X100:
		return "humidity_rh_x100";
	case ARGISENSE_REG_HUMIDITY_TEMP_CENTI_C:
		return "humidity_temp_centi_c";
	case ARGISENSE_REG_HUMIDITY_LAST_ERROR:
		return "humidity_last_error";
	default:
		return NULL;
	}
}

void argisense_register_update_sample(
	const struct argisense_measurement_sample *sample,
	const struct argisense_current_loop_command commands[ARGISENSE_CURRENT_LOOP_CHANNEL_COUNT])
{
	k_spinlock_key_t key;

	key = k_spin_lock(&sample_lock);
	sample_snapshot.methane_ppm_x100 = sample->methane_ppm_x100;
	sample_snapshot.pressure_pa = sample->pressure_pa;
	sample_snapshot.methane_valid = sample->methane_valid;
	sample_snapshot.pressure_valid = sample->pressure_valid;
	sample_snapshot.humidity_valid = sample->humidity_valid;
	sample_snapshot.methane_last_error = sample->methane_last_error;
	sample_snapshot.pressure_last_error = sample->pressure_last_error;
	sample_snapshot.humidity_last_error = sample->humidity_last_error;
	sample_snapshot.pressure_temperature_centi_c =
		sample->pressure_temperature_centi_c;
	sample_snapshot.humidity_rh_x100 = sample->humidity_rh_x100;
	sample_snapshot.humidity_temperature_centi_c =
		sample->humidity_temperature_centi_c;
	sample_snapshot.pressure_d1_raw = sample->pressure_d1_raw;
	sample_snapshot.pressure_d2_raw = sample->pressure_d2_raw;
	sample_snapshot.methane_status_flags = sample->methane_status_flags;
	sample_snapshot.methane_protocol_version =
		sample->methane_protocol_version;
	sample_snapshot.pressure_prom_crc_read =
		sample->pressure_prom_crc_read;
	sample_snapshot.pressure_prom_crc_calc =
		sample->pressure_prom_crc_calc;
	sample_snapshot.dac0_current_ua =
		commands[ARGISENSE_CURRENT_LOOP_METHANE].current_ua;
	sample_snapshot.dac1_current_ua =
		commands[ARGISENSE_CURRENT_LOOP_PRESSURE].current_ua;
	sample_snapshot.sequence++;
	sample_snapshot.uptime_seconds =
		(uint32_t)(k_uptime_get() / MSEC_PER_SEC);
	sample_snapshot.sample_ready = true;
	k_spin_unlock(&sample_lock, key);
}
