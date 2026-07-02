/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "argisense_registers.h"

#include "argisense_dfu.h"
#include "argisense_settings.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

LOG_MODULE_REGISTER(argisense_ph_registers, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

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
	ARGISENSE_REG_PH_X1000_HI = 10,
	ARGISENSE_REG_PH_X1000_LO = 11,
	ARGISENSE_REG_TEMP_CENTI_C_HI = 12,
	ARGISENSE_REG_TEMP_CENTI_C_LO = 13,
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
	ARGISENSE_REG_PH_CAL_MODE = 37,
	ARGISENSE_REG_PH_CAL_VALID = 38,
	ARGISENSE_REG_PH_RANGE_LOW_HI = 40,
	ARGISENSE_REG_PH_RANGE_LOW_LO = 41,
	ARGISENSE_REG_PH_RANGE_HIGH_HI = 42,
	ARGISENSE_REG_PH_RANGE_HIGH_LO = 43,
	ARGISENSE_REG_TEMP_RANGE_LOW_HI = 44,
	ARGISENSE_REG_TEMP_RANGE_LOW_LO = 45,
	ARGISENSE_REG_TEMP_RANGE_HIGH_HI = 46,
	ARGISENSE_REG_TEMP_RANGE_HIGH_LO = 47,
	ARGISENSE_REG_PH_SLOPE_HI = 48,
	ARGISENSE_REG_PH_SLOPE_LO = 49,
	ARGISENSE_REG_PH_OFFSET_HI = 50,
	ARGISENSE_REG_PH_OFFSET_LO = 51,
	ARGISENSE_REG_DAC0_4MA_TRIM_HI = 52,
	ARGISENSE_REG_DAC0_4MA_TRIM_LO = 53,
	ARGISENSE_REG_DAC0_20MA_TRIM_HI = 54,
	ARGISENSE_REG_DAC0_20MA_TRIM_LO = 55,
	ARGISENSE_REG_DAC1_4MA_TRIM_HI = 56,
	ARGISENSE_REG_DAC1_4MA_TRIM_LO = 57,
	ARGISENSE_REG_DAC1_20MA_TRIM_HI = 58,
	ARGISENSE_REG_DAC1_20MA_TRIM_LO = 59,
	ARGISENSE_REG_PH7_RAW_UV_HI = 60,
	ARGISENSE_REG_PH7_RAW_UV_LO = 61,
	ARGISENSE_REG_PH_SLOPE_UV_PER_PH_HI = 62,
	ARGISENSE_REG_PH_SLOPE_UV_PER_PH_LO = 63,
	ARGISENSE_REG_PH_TEMP_REF_CENTI_C_HI = 64,
	ARGISENSE_REG_PH_TEMP_REF_CENTI_C_LO = 65,
	ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_HI = 66,
	ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_LO = 67,
	ARGISENSE_REG_PH_LAST_ERROR = 70,
	ARGISENSE_REG_TEMP_LAST_ERROR = 71,
	ARGISENSE_REG_RESERVED_72 = 72,
	ARGISENSE_REG_RESERVED_73 = 73,
	ARGISENSE_REG_RESERVED_74 = 74,
	ARGISENSE_REG_PH_ADC_STATUS = 75,
	ARGISENSE_REG_BIAS_DAC_STATUS = 76,
	ARGISENSE_REG_PH_RAW_UV_HI = 80,
	ARGISENSE_REG_PH_RAW_UV_LO = 81,
	ARGISENSE_REG_VGS_ISFET_UV_HI = 82,
	ARGISENSE_REG_VGS_ISFET_UV_LO = 83,
	ARGISENSE_REG_VGS_REFET_UV_HI = 84,
	ARGISENSE_REG_VGS_REFET_UV_LO = 85,
	ARGISENSE_REG_VS_ISFET_UV_HI = 86,
	ARGISENSE_REG_VS_ISFET_UV_LO = 87,
	ARGISENSE_REG_VS_REFET_UV_HI = 88,
	ARGISENSE_REG_VS_REFET_UV_LO = 89,
	ARGISENSE_REG_GATE_UV_HI = 90,
	ARGISENSE_REG_GATE_UV_LO = 91,
	ARGISENSE_REG_ISFET_DRAIN_UV_HI = 92,
	ARGISENSE_REG_ISFET_DRAIN_UV_LO = 93,
	ARGISENSE_REG_REFET_DRAIN_UV_HI = 94,
	ARGISENSE_REG_REFET_DRAIN_UV_LO = 95,
	ARGISENSE_REG_ISFET_BULK_UV_HI = 96,
	ARGISENSE_REG_ISFET_BULK_UV_LO = 97,
	ARGISENSE_REG_ISFET_SUBS_UV_HI = 98,
	ARGISENSE_REG_ISFET_SUBS_UV_LO = 99,
	ARGISENSE_REG_REFET_BULK_UV_HI = 100,
	ARGISENSE_REG_REFET_BULK_UV_LO = 101,
	ARGISENSE_REG_REFET_SUBS_UV_HI = 102,
	ARGISENSE_REG_REFET_SUBS_UV_LO = 103,
	ARGISENSE_REG_PT1000_UV_HI = 104,
	ARGISENSE_REG_PT1000_UV_LO = 105,
	ARGISENSE_REG_PH_OPERATING_STATUS = 106,
};

#define ARGISENSE_PH_DEVICE_ID 0xA652U
#define ARGISENSE_BAUD_PRESET_CUSTOM 0xFFFFU
#define ARGISENSE_ACTIVE_SLOT_UNKNOWN 0xFFFFU

#define ARGISENSE_REGISTER_COMMAND_NONE           0x0000U
#define ARGISENSE_REGISTER_COMMAND_REBOOT         0xA551U
#define ARGISENSE_REGISTER_COMMAND_RESET_DEFAULTS 0xA552U
#define ARGISENSE_REGISTER_COMMAND_CONFIRM_IMAGE  0xA553U

#define ARGISENSE_STATUS_PH_VALID BIT(0)
#define ARGISENSE_STATUS_TEMP_VALID BIT(1)
#define ARGISENSE_STATUS_SAMPLE_READY BIT(2)
#define ARGISENSE_STATUS_TERMINATION_ENABLED BIT(3)
#define ARGISENSE_STATUS_PH_CALIBRATION_VALID BIT(6)
#define ARGISENSE_STATUS_PH_OPERATING_POINT_OK BIT(7)

#define ARGISENSE_BOOT_FLAG_MCUBOOT_ENABLED BIT(0)
#define ARGISENSE_BOOT_FLAG_IMAGE_CONFIRMED BIT(1)
#define ARGISENSE_BOOT_FLAG_INFO_VALID BIT(2)
#define ARGISENSE_BOOT_FLAG_REBOOT_REQUIRED BIT(3)

enum argisense_i32_config_field {
	ARGISENSE_I32_CONFIG_NONE = 0,
	ARGISENSE_I32_CONFIG_PH_RANGE_LOW,
	ARGISENSE_I32_CONFIG_PH_RANGE_HIGH,
	ARGISENSE_I32_CONFIG_TEMP_RANGE_LOW,
	ARGISENSE_I32_CONFIG_TEMP_RANGE_HIGH,
	ARGISENSE_I32_CONFIG_PH_SLOPE,
	ARGISENSE_I32_CONFIG_PH_OFFSET,
	ARGISENSE_I32_CONFIG_PH7_RAW_UV,
	ARGISENSE_I32_CONFIG_PH_SLOPE_UV_PER_PH,
	ARGISENSE_I32_CONFIG_PH_TEMP_REFERENCE,
	ARGISENSE_I32_CONFIG_PH_TEMP_COEFF,
	ARGISENSE_I32_CONFIG_DAC0_4MA_TRIM,
	ARGISENSE_I32_CONFIG_DAC0_20MA_TRIM,
	ARGISENSE_I32_CONFIG_DAC1_4MA_TRIM,
	ARGISENSE_I32_CONFIG_DAC1_20MA_TRIM,
};

struct argisense_firmware_info {
	uint16_t major;
	uint16_t minor;
	uint16_t patch;
	uint32_t build;
	uint16_t boot_flags;
	uint16_t active_slot;
};

struct argisense_pending_i32_write {
	enum argisense_i32_config_field field_id;
	uint16_t high_word;
	bool valid;
};

static struct argisense_ph_measurement_sample sample_snapshot;
static struct k_spinlock sample_lock;
static K_MUTEX_DEFINE(register_write_lock);
static struct argisense_pending_i32_write pending_i32_write;
static bool reboot_required;
static uint16_t last_command;
static int16_t last_command_result;
static const struct gpio_dt_spec rs485_termination =
	GPIO_DT_SPEC_GET(USER_NODE, rs485_termination_gpios);
static bool rs485_termination_configured;

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

static int32_t reg_i32_from_words(uint16_t hi, uint16_t lo)
{
	return (int32_t)((((uint32_t)hi) << 16) | lo);
}

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Rebooting pH firmware by RS485 command");
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

static struct argisense_ph_measurement_sample sample_get(void)
{
	struct argisense_ph_measurement_sample snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&sample_lock);
	snapshot = sample_snapshot;
	k_spin_unlock(&sample_lock, key);

	return snapshot;
}

static uint16_t status_flags(
	const struct argisense_ph_measurement_sample *snapshot,
	const struct argisense_runtime_config *config)
{
	uint16_t flags = 0U;

	if (snapshot->ph_valid) {
		flags |= ARGISENSE_STATUS_PH_VALID;
	}

	if (snapshot->temperature_valid) {
		flags |= ARGISENSE_STATUS_TEMP_VALID;
	}

	if (snapshot->sample_ready) {
		flags |= ARGISENSE_STATUS_SAMPLE_READY;
	}

	if (config->rs485_termination_enabled != 0U) {
		flags |= ARGISENSE_STATUS_TERMINATION_ENABLED;
	}

	if (config->ph_calibration_valid != 0U) {
		flags |= ARGISENSE_STATUS_PH_CALIBRATION_VALID;
	}

	if ((snapshot->ph_operating_status &
	     ARGISENSE_PH_OPERATING_STATUS_PH_VALID) != 0U) {
		flags |= ARGISENSE_STATUS_PH_OPERATING_POINT_OK;
	}

	return flags;
}

int argisense_register_read_holding(uint16_t addr, uint16_t *reg)
{
	struct argisense_runtime_config config;
	const struct argisense_ph_measurement_sample snapshot = sample_get();
	struct argisense_firmware_info firmware_info;

	if (reg == NULL) {
		return -EINVAL;
	}

	if (argisense_dfu_register_is_supported(addr)) {
		return argisense_dfu_register_read(addr, reg);
	}

	argisense_settings_get_copy(&config);
	firmware_info_get(&firmware_info);

	switch (addr) {
	case ARGISENSE_REG_DEVICE_ID:
		*reg = ARGISENSE_PH_DEVICE_ID;
		return 0;
	case ARGISENSE_REG_MAP_VERSION:
		*reg = ARGISENSE_REGISTER_MAP_VERSION;
		return 0;
	case ARGISENSE_REG_STATUS_FLAGS:
		*reg = status_flags(&snapshot, &config);
		return 0;
	case ARGISENSE_REG_MODBUS_ADDRESS:
		*reg = config.modbus_address;
		return 0;
	case ARGISENSE_REG_BAUDRATE_HI:
		*reg = reg_u32_hi(config.rs485_baudrate);
		return 0;
	case ARGISENSE_REG_BAUDRATE_LO:
		*reg = reg_u32_lo(config.rs485_baudrate);
		return 0;
	case ARGISENSE_REG_MEASUREMENT_PERIOD_SECONDS:
		*reg = (uint16_t)MIN(config.measurement_period_seconds,
				      UINT16_MAX);
		return 0;
	case ARGISENSE_REG_MEASUREMENT_WINDOW_MS:
		*reg = (uint16_t)MIN(config.measurement_window_ms,
				      UINT16_MAX);
		return 0;
	case ARGISENSE_REG_BAUD_PRESET:
		*reg = baud_preset_from_baud(config.rs485_baudrate);
		return 0;
	case ARGISENSE_REG_TERMINATION_ENABLED:
		*reg = config.rs485_termination_enabled != 0U ? 1U : 0U;
		return 0;
	case ARGISENSE_REG_PH_X1000_HI:
		*reg = reg_i32_hi(snapshot.ph_x1000);
		return 0;
	case ARGISENSE_REG_PH_X1000_LO:
		*reg = reg_i32_lo(snapshot.ph_x1000);
		return 0;
	case ARGISENSE_REG_TEMP_CENTI_C_HI:
		*reg = reg_i32_hi(snapshot.temperature_centi_c);
		return 0;
	case ARGISENSE_REG_TEMP_CENTI_C_LO:
		*reg = reg_i32_lo(snapshot.temperature_centi_c);
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
		*reg = (uint16_t)config.dac_min_current_ua;
		return 0;
	case ARGISENSE_REG_DAC_MAX_CURRENT_UA:
		*reg = (uint16_t)config.dac_max_current_ua;
		return 0;
	case ARGISENSE_REG_DAC_FAULT_CURRENT_UA:
		*reg = (uint16_t)config.dac_fault_current_ua;
		return 0;
	case ARGISENSE_REG_COMMAND:
		*reg = ARGISENSE_REGISTER_COMMAND_NONE;
		return 0;
	case ARGISENSE_REG_RS485_PARITY:
		*reg = config.rs485_parity;
		return 0;
	case ARGISENSE_REG_RS485_STOP_BITS:
		*reg = config.rs485_stop_bits;
		return 0;
	case ARGISENSE_REG_RS485_DATA_BITS:
		*reg = config.rs485_data_bits;
		return 0;
	case ARGISENSE_REG_PH_CAL_MODE:
		*reg = config.ph_calibration_mode;
		return 0;
	case ARGISENSE_REG_PH_CAL_VALID:
		*reg = config.ph_calibration_valid;
		return 0;
	case ARGISENSE_REG_PH_RANGE_LOW_HI:
		*reg = reg_i32_hi(config.ph_range_low_x1000);
		return 0;
	case ARGISENSE_REG_PH_RANGE_LOW_LO:
		*reg = reg_i32_lo(config.ph_range_low_x1000);
		return 0;
	case ARGISENSE_REG_PH_RANGE_HIGH_HI:
		*reg = reg_i32_hi(config.ph_range_high_x1000);
		return 0;
	case ARGISENSE_REG_PH_RANGE_HIGH_LO:
		*reg = reg_i32_lo(config.ph_range_high_x1000);
		return 0;
	case ARGISENSE_REG_TEMP_RANGE_LOW_HI:
		*reg = reg_i32_hi(config.temperature_range_low_centi_c);
		return 0;
	case ARGISENSE_REG_TEMP_RANGE_LOW_LO:
		*reg = reg_i32_lo(config.temperature_range_low_centi_c);
		return 0;
	case ARGISENSE_REG_TEMP_RANGE_HIGH_HI:
		*reg = reg_i32_hi(config.temperature_range_high_centi_c);
		return 0;
	case ARGISENSE_REG_TEMP_RANGE_HIGH_LO:
		*reg = reg_i32_lo(config.temperature_range_high_centi_c);
		return 0;
	case ARGISENSE_REG_PH_SLOPE_HI:
		*reg = reg_i32_hi(config.ph_slope_x1000);
		return 0;
	case ARGISENSE_REG_PH_SLOPE_LO:
		*reg = reg_i32_lo(config.ph_slope_x1000);
		return 0;
	case ARGISENSE_REG_PH_OFFSET_HI:
		*reg = reg_i32_hi(config.ph_offset_x1000);
		return 0;
	case ARGISENSE_REG_PH_OFFSET_LO:
		*reg = reg_i32_lo(config.ph_offset_x1000);
		return 0;
	case ARGISENSE_REG_DAC0_4MA_TRIM_HI:
		*reg = reg_i32_hi(config.dac0_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC0_4MA_TRIM_LO:
		*reg = reg_i32_lo(config.dac0_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC0_20MA_TRIM_HI:
		*reg = reg_i32_hi(config.dac0_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC0_20MA_TRIM_LO:
		*reg = reg_i32_lo(config.dac0_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_4MA_TRIM_HI:
		*reg = reg_i32_hi(config.dac1_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_4MA_TRIM_LO:
		*reg = reg_i32_lo(config.dac1_4ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_20MA_TRIM_HI:
		*reg = reg_i32_hi(config.dac1_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_DAC1_20MA_TRIM_LO:
		*reg = reg_i32_lo(config.dac1_20ma_trim_ua);
		return 0;
	case ARGISENSE_REG_PH7_RAW_UV_HI:
		*reg = reg_i32_hi(config.ph7_raw_uv);
		return 0;
	case ARGISENSE_REG_PH7_RAW_UV_LO:
		*reg = reg_i32_lo(config.ph7_raw_uv);
		return 0;
	case ARGISENSE_REG_PH_SLOPE_UV_PER_PH_HI:
		*reg = reg_i32_hi(config.ph_slope_uv_per_ph);
		return 0;
	case ARGISENSE_REG_PH_SLOPE_UV_PER_PH_LO:
		*reg = reg_i32_lo(config.ph_slope_uv_per_ph);
		return 0;
	case ARGISENSE_REG_PH_TEMP_REF_CENTI_C_HI:
		*reg = reg_i32_hi(config.ph_temp_reference_centi_c);
		return 0;
	case ARGISENSE_REG_PH_TEMP_REF_CENTI_C_LO:
		*reg = reg_i32_lo(config.ph_temp_reference_centi_c);
		return 0;
	case ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_HI:
		*reg = reg_i32_hi(config.ph_temp_coeff_ppm_per_c);
		return 0;
	case ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_LO:
		*reg = reg_i32_lo(config.ph_temp_coeff_ppm_per_c);
		return 0;
	case ARGISENSE_REG_PH_LAST_ERROR:
		*reg = (uint16_t)snapshot.ph_last_error;
		return 0;
	case ARGISENSE_REG_TEMP_LAST_ERROR:
		*reg = (uint16_t)snapshot.temperature_last_error;
		return 0;
	case ARGISENSE_REG_RESERVED_72:
		*reg = 0U;
		return 0;
	case ARGISENSE_REG_RESERVED_73:
		*reg = 0U;
		return 0;
	case ARGISENSE_REG_RESERVED_74:
		*reg = 0U;
		return 0;
	case ARGISENSE_REG_PH_ADC_STATUS:
		*reg = snapshot.ph_adc_status;
		return 0;
	case ARGISENSE_REG_BIAS_DAC_STATUS:
		*reg = snapshot.bias_dac_status;
		return 0;
	case ARGISENSE_REG_PH_RAW_UV_HI:
		*reg = reg_i32_hi(snapshot.ph_raw_uv);
		return 0;
	case ARGISENSE_REG_PH_RAW_UV_LO:
		*reg = reg_i32_lo(snapshot.ph_raw_uv);
		return 0;
	case ARGISENSE_REG_VGS_ISFET_UV_HI:
		*reg = reg_i32_hi(snapshot.vgs_isfet_uv);
		return 0;
	case ARGISENSE_REG_VGS_ISFET_UV_LO:
		*reg = reg_i32_lo(snapshot.vgs_isfet_uv);
		return 0;
	case ARGISENSE_REG_VGS_REFET_UV_HI:
		*reg = reg_i32_hi(snapshot.vgs_refet_uv);
		return 0;
	case ARGISENSE_REG_VGS_REFET_UV_LO:
		*reg = reg_i32_lo(snapshot.vgs_refet_uv);
		return 0;
	case ARGISENSE_REG_VS_ISFET_UV_HI:
		*reg = reg_i32_hi(snapshot.vs_isfet_uv);
		return 0;
	case ARGISENSE_REG_VS_ISFET_UV_LO:
		*reg = reg_i32_lo(snapshot.vs_isfet_uv);
		return 0;
	case ARGISENSE_REG_VS_REFET_UV_HI:
		*reg = reg_i32_hi(snapshot.vs_refet_uv);
		return 0;
	case ARGISENSE_REG_VS_REFET_UV_LO:
		*reg = reg_i32_lo(snapshot.vs_refet_uv);
		return 0;
	case ARGISENSE_REG_GATE_UV_HI:
		*reg = reg_i32_hi(snapshot.gate_uv);
		return 0;
	case ARGISENSE_REG_GATE_UV_LO:
		*reg = reg_i32_lo(snapshot.gate_uv);
		return 0;
	case ARGISENSE_REG_ISFET_DRAIN_UV_HI:
		*reg = reg_i32_hi(snapshot.isfet_drain_uv);
		return 0;
	case ARGISENSE_REG_ISFET_DRAIN_UV_LO:
		*reg = reg_i32_lo(snapshot.isfet_drain_uv);
		return 0;
	case ARGISENSE_REG_REFET_DRAIN_UV_HI:
		*reg = reg_i32_hi(snapshot.refet_drain_uv);
		return 0;
	case ARGISENSE_REG_REFET_DRAIN_UV_LO:
		*reg = reg_i32_lo(snapshot.refet_drain_uv);
		return 0;
	case ARGISENSE_REG_ISFET_BULK_UV_HI:
		*reg = reg_i32_hi(snapshot.isfet_bulk_uv);
		return 0;
	case ARGISENSE_REG_ISFET_BULK_UV_LO:
		*reg = reg_i32_lo(snapshot.isfet_bulk_uv);
		return 0;
	case ARGISENSE_REG_ISFET_SUBS_UV_HI:
		*reg = reg_i32_hi(snapshot.isfet_subs_uv);
		return 0;
	case ARGISENSE_REG_ISFET_SUBS_UV_LO:
		*reg = reg_i32_lo(snapshot.isfet_subs_uv);
		return 0;
	case ARGISENSE_REG_REFET_BULK_UV_HI:
		*reg = reg_i32_hi(snapshot.refet_bulk_uv);
		return 0;
	case ARGISENSE_REG_REFET_BULK_UV_LO:
		*reg = reg_i32_lo(snapshot.refet_bulk_uv);
		return 0;
	case ARGISENSE_REG_REFET_SUBS_UV_HI:
		*reg = reg_i32_hi(snapshot.refet_subs_uv);
		return 0;
	case ARGISENSE_REG_REFET_SUBS_UV_LO:
		*reg = reg_i32_lo(snapshot.refet_subs_uv);
		return 0;
	case ARGISENSE_REG_PT1000_UV_HI:
		*reg = reg_i32_hi(snapshot.pt1000_uv);
		return 0;
	case ARGISENSE_REG_PT1000_UV_LO:
		*reg = reg_i32_lo(snapshot.pt1000_uv);
		return 0;
	case ARGISENSE_REG_PH_OPERATING_STATUS:
		*reg = snapshot.ph_operating_status;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int save_config(const struct argisense_runtime_config *config,
		       const char *field_name)
{
	int ret = argisense_settings_save(config);

	if (ret < 0) {
		LOG_WRN("Rejected pH register write to %s: %d", field_name,
			ret);
		return ret;
	}

	LOG_INF("Updated persistent pH setting from register write: %s",
		field_name);

	return 0;
}

static int apply_rs485_termination_gpio(bool enabled)
{
	int ret;

	if (!gpio_is_ready_dt(&rs485_termination)) {
		LOG_WRN("RS485 termination GPIO controller is not ready");
		return -ENODEV;
	}

	if (!rs485_termination_configured) {
		ret = gpio_pin_configure_dt(&rs485_termination,
					    GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("RS485 termination GPIO configure failed: %d",
				ret);
			return ret;
		}

		rs485_termination_configured = true;
	}

	ret = gpio_pin_set_dt(&rs485_termination, enabled);
	if (ret < 0) {
		LOG_ERR("RS485 termination GPIO set failed: %d", ret);
		return ret;
	}

	LOG_INF("RS485 termination %s", enabled ? "enabled" : "disabled");
	return 0;
}

int argisense_register_apply_runtime_outputs(void)
{
	struct argisense_runtime_config config;

	argisense_settings_get_copy(&config);

	return apply_rs485_termination_gpio(
		config.rs485_termination_enabled != 0U);
}

static int save_i32_config_word(struct argisense_runtime_config *config,
				int32_t *field,
				enum argisense_i32_config_field field_id,
				uint16_t reg, bool high_word,
				const char *field_name)
{
	uint16_t staged_high_word;

	if (high_word) {
		pending_i32_write.field_id = field_id;
		pending_i32_write.high_word = reg;
		pending_i32_write.valid = true;
		return 0;
	}

	staged_high_word = reg_i32_hi(*field);
	if (pending_i32_write.valid &&
	    pending_i32_write.field_id == field_id) {
		staged_high_word = pending_i32_write.high_word;
	}

	pending_i32_write.valid = false;
	*field = reg_i32_from_words(staged_high_word, reg);

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
			LOG_INF("pH settings reset to defaults by RS485 command");
		}
		break;
	case ARGISENSE_REGISTER_COMMAND_CONFIRM_IMAGE:
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
		ret = boot_write_img_confirmed();
		if (ret == 0) {
			LOG_INF("MCUboot image confirmed by pH RS485 command");
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

static int argisense_register_write_holding_locked(uint16_t addr, uint16_t reg)
{
	struct argisense_runtime_config config;
	uint32_t baudrate;
	int ret;

	if (argisense_dfu_register_is_supported(addr)) {
		return argisense_dfu_register_write(addr, reg);
	}

	argisense_settings_get_copy(&config);

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
		}
		return ret;
	case ARGISENSE_REG_TERMINATION_ENABLED:
		if (reg > 1U) {
			return -EINVAL;
		}
		config.rs485_termination_enabled = (uint8_t)reg;
		ret = save_config(&config, "rs485-termination-enabled");
		if (ret == 0) {
			ret = apply_rs485_termination_gpio(
				config.rs485_termination_enabled != 0U);
		}
		return ret;
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
		}
		return ret;
	case ARGISENSE_REG_PH_CAL_MODE:
		if (reg > ARGISENSE_PH_CAL_MODE_REFERENCE_ELECTRODE) {
			return -EINVAL;
		}
		config.ph_calibration_mode = (uint8_t)reg;
		if (reg == ARGISENSE_PH_CAL_MODE_DISABLED) {
			config.ph_calibration_valid = 0U;
		}
		return save_config(&config, "ph-calibration-mode");
	case ARGISENSE_REG_PH_CAL_VALID:
		if (reg > 1U) {
			return -EINVAL;
		}
		config.ph_calibration_valid = (uint8_t)reg;
		return save_config(&config, "ph-calibration-valid");
	case ARGISENSE_REG_PH_RANGE_LOW_HI:
		return save_i32_config_word(&config, &config.ph_range_low_x1000,
					    ARGISENSE_I32_CONFIG_PH_RANGE_LOW,
					    reg, true, "ph-range-low-x1000");
	case ARGISENSE_REG_PH_RANGE_LOW_LO:
		return save_i32_config_word(&config, &config.ph_range_low_x1000,
					    ARGISENSE_I32_CONFIG_PH_RANGE_LOW,
					    reg, false, "ph-range-low-x1000");
	case ARGISENSE_REG_PH_RANGE_HIGH_HI:
		return save_i32_config_word(&config, &config.ph_range_high_x1000,
					    ARGISENSE_I32_CONFIG_PH_RANGE_HIGH,
					    reg, true, "ph-range-high-x1000");
	case ARGISENSE_REG_PH_RANGE_HIGH_LO:
		return save_i32_config_word(&config, &config.ph_range_high_x1000,
					    ARGISENSE_I32_CONFIG_PH_RANGE_HIGH,
					    reg, false, "ph-range-high-x1000");
	case ARGISENSE_REG_TEMP_RANGE_LOW_HI:
		return save_i32_config_word(&config,
					    &config.temperature_range_low_centi_c,
					    ARGISENSE_I32_CONFIG_TEMP_RANGE_LOW,
					    reg, true, "temp-range-low-centi-c");
	case ARGISENSE_REG_TEMP_RANGE_LOW_LO:
		return save_i32_config_word(&config,
					    &config.temperature_range_low_centi_c,
					    ARGISENSE_I32_CONFIG_TEMP_RANGE_LOW,
					    reg, false, "temp-range-low-centi-c");
	case ARGISENSE_REG_TEMP_RANGE_HIGH_HI:
		return save_i32_config_word(&config,
					    &config.temperature_range_high_centi_c,
					    ARGISENSE_I32_CONFIG_TEMP_RANGE_HIGH,
					    reg, true, "temp-range-high-centi-c");
	case ARGISENSE_REG_TEMP_RANGE_HIGH_LO:
		return save_i32_config_word(&config,
					    &config.temperature_range_high_centi_c,
					    ARGISENSE_I32_CONFIG_TEMP_RANGE_HIGH,
					    reg, false, "temp-range-high-centi-c");
	case ARGISENSE_REG_PH_SLOPE_HI:
		return save_i32_config_word(&config, &config.ph_slope_x1000,
					    ARGISENSE_I32_CONFIG_PH_SLOPE,
					    reg, true, "ph-slope-x1000");
	case ARGISENSE_REG_PH_SLOPE_LO:
		return save_i32_config_word(&config, &config.ph_slope_x1000,
					    ARGISENSE_I32_CONFIG_PH_SLOPE,
					    reg, false, "ph-slope-x1000");
	case ARGISENSE_REG_PH_OFFSET_HI:
		return save_i32_config_word(&config, &config.ph_offset_x1000,
					    ARGISENSE_I32_CONFIG_PH_OFFSET,
					    reg, true, "ph-offset-x1000");
	case ARGISENSE_REG_PH_OFFSET_LO:
		return save_i32_config_word(&config, &config.ph_offset_x1000,
					    ARGISENSE_I32_CONFIG_PH_OFFSET,
					    reg, false, "ph-offset-x1000");
	case ARGISENSE_REG_PH7_RAW_UV_HI:
		return save_i32_config_word(&config, &config.ph7_raw_uv,
					    ARGISENSE_I32_CONFIG_PH7_RAW_UV,
					    reg, true, "ph7-raw-uv");
	case ARGISENSE_REG_PH7_RAW_UV_LO:
		return save_i32_config_word(&config, &config.ph7_raw_uv,
					    ARGISENSE_I32_CONFIG_PH7_RAW_UV,
					    reg, false, "ph7-raw-uv");
	case ARGISENSE_REG_PH_SLOPE_UV_PER_PH_HI:
		return save_i32_config_word(&config,
					    &config.ph_slope_uv_per_ph,
					    ARGISENSE_I32_CONFIG_PH_SLOPE_UV_PER_PH,
					    reg, true, "ph-slope-uv-per-ph");
	case ARGISENSE_REG_PH_SLOPE_UV_PER_PH_LO:
		return save_i32_config_word(&config,
					    &config.ph_slope_uv_per_ph,
					    ARGISENSE_I32_CONFIG_PH_SLOPE_UV_PER_PH,
					    reg, false, "ph-slope-uv-per-ph");
	case ARGISENSE_REG_PH_TEMP_REF_CENTI_C_HI:
		return save_i32_config_word(&config,
					    &config.ph_temp_reference_centi_c,
					    ARGISENSE_I32_CONFIG_PH_TEMP_REFERENCE,
					    reg, true, "ph-temp-reference-centi-c");
	case ARGISENSE_REG_PH_TEMP_REF_CENTI_C_LO:
		return save_i32_config_word(&config,
					    &config.ph_temp_reference_centi_c,
					    ARGISENSE_I32_CONFIG_PH_TEMP_REFERENCE,
					    reg, false, "ph-temp-reference-centi-c");
	case ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_HI:
		return save_i32_config_word(&config,
					    &config.ph_temp_coeff_ppm_per_c,
					    ARGISENSE_I32_CONFIG_PH_TEMP_COEFF,
					    reg, true, "ph-temp-coeff-ppm-per-c");
	case ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_LO:
		return save_i32_config_word(&config,
					    &config.ph_temp_coeff_ppm_per_c,
					    ARGISENSE_I32_CONFIG_PH_TEMP_COEFF,
					    reg, false, "ph-temp-coeff-ppm-per-c");
	case ARGISENSE_REG_DAC0_4MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac0_4ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC0_4MA_TRIM,
					    reg, true, "dac0-4ma-trim-ua");
	case ARGISENSE_REG_DAC0_4MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac0_4ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC0_4MA_TRIM,
					    reg, false, "dac0-4ma-trim-ua");
	case ARGISENSE_REG_DAC0_20MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac0_20ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC0_20MA_TRIM,
					    reg, true, "dac0-20ma-trim-ua");
	case ARGISENSE_REG_DAC0_20MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac0_20ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC0_20MA_TRIM,
					    reg, false, "dac0-20ma-trim-ua");
	case ARGISENSE_REG_DAC1_4MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac1_4ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC1_4MA_TRIM,
					    reg, true, "dac1-4ma-trim-ua");
	case ARGISENSE_REG_DAC1_4MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac1_4ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC1_4MA_TRIM,
					    reg, false, "dac1-4ma-trim-ua");
	case ARGISENSE_REG_DAC1_20MA_TRIM_HI:
		return save_i32_config_word(&config, &config.dac1_20ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC1_20MA_TRIM,
					    reg, true, "dac1-20ma-trim-ua");
	case ARGISENSE_REG_DAC1_20MA_TRIM_LO:
		return save_i32_config_word(&config, &config.dac1_20ma_trim_ua,
					    ARGISENSE_I32_CONFIG_DAC1_20MA_TRIM,
					    reg, false, "dac1-20ma-trim-ua");
	default:
		return -ENOTSUP;
	}
}

int argisense_register_write_holding(uint16_t addr, uint16_t reg)
{
	int ret;

	k_mutex_lock(&register_write_lock, K_FOREVER);
	ret = argisense_register_write_holding_locked(addr, reg);
	k_mutex_unlock(&register_write_lock);

	return ret;
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
	case ARGISENSE_REG_PH_X1000_HI:
		return "ph_x1000_hi";
	case ARGISENSE_REG_PH_X1000_LO:
		return "ph_x1000_lo";
	case ARGISENSE_REG_TEMP_CENTI_C_HI:
		return "temperature_centi_c_hi";
	case ARGISENSE_REG_TEMP_CENTI_C_LO:
		return "temperature_centi_c_lo";
	case ARGISENSE_REG_DAC0_CURRENT_UA:
		return "dac0_ph_current_ua";
	case ARGISENSE_REG_DAC1_CURRENT_UA:
		return "dac1_temp_current_ua";
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
	case ARGISENSE_REG_PH_CAL_MODE:
		return "ph_calibration_mode";
	case ARGISENSE_REG_PH_CAL_VALID:
		return "ph_calibration_valid";
	case ARGISENSE_REG_PH_RANGE_LOW_HI:
		return "ph_range_low_hi";
	case ARGISENSE_REG_PH_RANGE_LOW_LO:
		return "ph_range_low_lo";
	case ARGISENSE_REG_PH_RANGE_HIGH_HI:
		return "ph_range_high_hi";
	case ARGISENSE_REG_PH_RANGE_HIGH_LO:
		return "ph_range_high_lo";
	case ARGISENSE_REG_TEMP_RANGE_LOW_HI:
		return "temp_range_low_hi";
	case ARGISENSE_REG_TEMP_RANGE_LOW_LO:
		return "temp_range_low_lo";
	case ARGISENSE_REG_TEMP_RANGE_HIGH_HI:
		return "temp_range_high_hi";
	case ARGISENSE_REG_TEMP_RANGE_HIGH_LO:
		return "temp_range_high_lo";
	case ARGISENSE_REG_PH_SLOPE_HI:
		return "ph_slope_hi";
	case ARGISENSE_REG_PH_SLOPE_LO:
		return "ph_slope_lo";
	case ARGISENSE_REG_PH_OFFSET_HI:
		return "ph_offset_hi";
	case ARGISENSE_REG_PH_OFFSET_LO:
		return "ph_offset_lo";
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
	case ARGISENSE_REG_PH7_RAW_UV_HI:
		return "ph7_raw_uv_hi";
	case ARGISENSE_REG_PH7_RAW_UV_LO:
		return "ph7_raw_uv_lo";
	case ARGISENSE_REG_PH_SLOPE_UV_PER_PH_HI:
		return "ph_slope_uv_per_ph_hi";
	case ARGISENSE_REG_PH_SLOPE_UV_PER_PH_LO:
		return "ph_slope_uv_per_ph_lo";
	case ARGISENSE_REG_PH_TEMP_REF_CENTI_C_HI:
		return "ph_temp_ref_centi_c_hi";
	case ARGISENSE_REG_PH_TEMP_REF_CENTI_C_LO:
		return "ph_temp_ref_centi_c_lo";
	case ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_HI:
		return "ph_temp_coeff_ppm_per_c_hi";
	case ARGISENSE_REG_PH_TEMP_COEFF_PPM_PER_C_LO:
		return "ph_temp_coeff_ppm_per_c_lo";
	case ARGISENSE_REG_PH_LAST_ERROR:
		return "ph_last_error";
	case ARGISENSE_REG_TEMP_LAST_ERROR:
		return "temperature_last_error";
	case ARGISENSE_REG_RESERVED_72:
		return "reserved_not_used_72";
	case ARGISENSE_REG_RESERVED_73:
		return "reserved_not_used_73";
	case ARGISENSE_REG_RESERVED_74:
		return "reserved_not_used_74";
	case ARGISENSE_REG_PH_ADC_STATUS:
		return "ph_adc_status";
	case ARGISENSE_REG_BIAS_DAC_STATUS:
		return "bias_dac_status";
	case ARGISENSE_REG_PH_RAW_UV_HI:
		return "ph_raw_uv_hi";
	case ARGISENSE_REG_PH_RAW_UV_LO:
		return "ph_raw_uv_lo";
	case ARGISENSE_REG_VGS_ISFET_UV_HI:
		return "vgs_isfet_uv_hi";
	case ARGISENSE_REG_VGS_ISFET_UV_LO:
		return "vgs_isfet_uv_lo";
	case ARGISENSE_REG_VGS_REFET_UV_HI:
		return "vgs_refet_uv_hi";
	case ARGISENSE_REG_VGS_REFET_UV_LO:
		return "vgs_refet_uv_lo";
	case ARGISENSE_REG_VS_ISFET_UV_HI:
		return "vs_isfet_uv_hi";
	case ARGISENSE_REG_VS_ISFET_UV_LO:
		return "vs_isfet_uv_lo";
	case ARGISENSE_REG_VS_REFET_UV_HI:
		return "vs_refet_uv_hi";
	case ARGISENSE_REG_VS_REFET_UV_LO:
		return "vs_refet_uv_lo";
	case ARGISENSE_REG_GATE_UV_HI:
		return "gate_uv_hi";
	case ARGISENSE_REG_GATE_UV_LO:
		return "gate_uv_lo";
	case ARGISENSE_REG_ISFET_DRAIN_UV_HI:
		return "isfet_drain_uv_hi";
	case ARGISENSE_REG_ISFET_DRAIN_UV_LO:
		return "isfet_drain_uv_lo";
	case ARGISENSE_REG_REFET_DRAIN_UV_HI:
		return "refet_drain_uv_hi";
	case ARGISENSE_REG_REFET_DRAIN_UV_LO:
		return "refet_drain_uv_lo";
	case ARGISENSE_REG_ISFET_BULK_UV_HI:
		return "isfet_bulk_uv_hi";
	case ARGISENSE_REG_ISFET_BULK_UV_LO:
		return "isfet_bulk_uv_lo";
	case ARGISENSE_REG_ISFET_SUBS_UV_HI:
		return "isfet_subs_uv_hi";
	case ARGISENSE_REG_ISFET_SUBS_UV_LO:
		return "isfet_subs_uv_lo";
	case ARGISENSE_REG_REFET_BULK_UV_HI:
		return "refet_bulk_uv_hi";
	case ARGISENSE_REG_REFET_BULK_UV_LO:
		return "refet_bulk_uv_lo";
	case ARGISENSE_REG_REFET_SUBS_UV_HI:
		return "refet_subs_uv_hi";
	case ARGISENSE_REG_REFET_SUBS_UV_LO:
		return "refet_subs_uv_lo";
	case ARGISENSE_REG_PT1000_UV_HI:
		return "pt1000_uv_hi";
	case ARGISENSE_REG_PT1000_UV_LO:
		return "pt1000_uv_lo";
	case ARGISENSE_REG_PH_OPERATING_STATUS:
		return "ph_operating_status";
	default:
		return NULL;
	}
}

void argisense_register_update_sample(
	const struct argisense_ph_measurement_sample *sample)
{
	uint32_t next_sequence;
	k_spinlock_key_t key;

	if (sample == NULL) {
		return;
	}

	key = k_spin_lock(&sample_lock);
	next_sequence = sample_snapshot.sequence + 1U;
	sample_snapshot = *sample;
	sample_snapshot.sequence = next_sequence;
	sample_snapshot.uptime_seconds =
		(uint32_t)(k_uptime_get() / MSEC_PER_SEC);
	sample_snapshot.sample_ready = true;
	k_spin_unlock(&sample_lock, key);
}
