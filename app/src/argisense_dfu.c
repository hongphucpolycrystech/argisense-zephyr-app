/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "argisense_dfu.h"

#include <errno.h>
#include <string.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(argisense_dfu, LOG_LEVEL_INF);

#define ARGISENSE_DFU_CMD_NONE        0x0000U
#define ARGISENSE_DFU_CMD_BEGIN       0xD001U
#define ARGISENSE_DFU_CMD_WRITE       0xD002U
#define ARGISENSE_DFU_CMD_VERIFY      0xD003U
#define ARGISENSE_DFU_CMD_TEST        0xD004U
#define ARGISENSE_DFU_CMD_ABORT       0xD005U
#define ARGISENSE_DFU_CMD_TEST_REBOOT 0xD006U

#define ARGISENSE_DFU_STATUS_IDLE      0U
#define ARGISENSE_DFU_STATUS_RECEIVING 1U
#define ARGISENSE_DFU_STATUS_VERIFYING 2U
#define ARGISENSE_DFU_STATUS_VERIFIED  3U
#define ARGISENSE_DFU_STATUS_PENDING   4U
#define ARGISENSE_DFU_STATUS_ERROR     5U

#define ARGISENSE_DFU_SHA256_BYTES 32U

BUILD_ASSERT((CONFIG_ARGISENSE_RS485_DFU_MAX_CHUNK_BYTES % 2) == 0,
	     "RS485 DFU chunk size must be an even number of bytes");

struct argisense_dfu_state {
	struct flash_img_context flash_ctx;
	struct k_mutex lock;
	uint8_t sha256[ARGISENSE_DFU_SHA256_BYTES];
	uint8_t chunk[CONFIG_ARGISENSE_RS485_DFU_MAX_CHUNK_BYTES];
	uint32_t image_size;
	uint32_t image_crc32_expected;
	uint32_t image_crc32_running;
	uint32_t bytes_received;
	uint32_t chunk_offset;
	uint32_t chunk_crc32_expected;
	uint16_t chunk_length;
	uint16_t status;
	uint16_t last_command;
	int16_t last_error;
	bool context_open;
	bool verified;
};

static struct argisense_dfu_state dfu = {
	.lock = Z_MUTEX_INITIALIZER(dfu.lock),
	.status = ARGISENSE_DFU_STATUS_IDLE,
};

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("Rebooting by RS485 DFU command");
	sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(dfu_reboot_work, reboot_work_handler);

static uint32_t u32_from_words(uint16_t hi, uint16_t lo)
{
	return (((uint32_t)hi) << 16) | lo;
}

static uint16_t u32_hi(uint32_t value)
{
	return (uint16_t)(value >> 16);
}

static uint16_t u32_lo(uint32_t value)
{
	return (uint16_t)value;
}

static void set_error(int error)
{
	dfu.last_error = (int16_t)error;
	dfu.status = ARGISENSE_DFU_STATUS_ERROR;
}

static void clear_error(void)
{
	dfu.last_error = 0;
}

static void close_context_if_open(void)
{
	if (dfu.context_open && dfu.flash_ctx.flash_area != NULL) {
		flash_area_close(dfu.flash_ctx.flash_area);
		dfu.flash_ctx.flash_area = NULL;
	}

	dfu.context_open = false;
}

static int upload_area_max_size(uint32_t *max_size)
{
	const struct flash_area *area;
	const uint8_t area_id = flash_img_get_upload_slot();
	size_t image_start_offset;
	int ret;

	if (max_size == NULL) {
		return -EINVAL;
	}

	ret = flash_area_open(area_id, &area);
	if (ret < 0) {
		return ret;
	}

	image_start_offset = boot_get_image_start_offset(area_id);
	if (image_start_offset >= area->fa_size) {
		flash_area_close(area);
		return -ERANGE;
	}

	*max_size = (uint32_t)(area->fa_size - image_start_offset);
	flash_area_close(area);

	return 0;
}

static int command_begin(void)
{
	uint32_t max_size;
	int ret;

	if (dfu.image_size == 0U) {
		return -EINVAL;
	}

	ret = upload_area_max_size(&max_size);
	if (ret < 0) {
		return ret;
	}

	if (dfu.image_size > max_size) {
		LOG_ERR("RS485 DFU image too large: %u > %u",
			dfu.image_size, max_size);
		return -EFBIG;
	}

	close_context_if_open();

	memset(&dfu.flash_ctx, 0, sizeof(dfu.flash_ctx));
	dfu.bytes_received = 0U;
	dfu.image_crc32_running = 0U;
	dfu.chunk_offset = 0U;
	dfu.chunk_length = 0U;
	dfu.chunk_crc32_expected = 0U;
	dfu.verified = false;

	ret = flash_img_init(&dfu.flash_ctx);
	if (ret < 0) {
		return ret;
	}

	dfu.context_open = true;
	dfu.status = ARGISENSE_DFU_STATUS_RECEIVING;

	LOG_INF("RS485 DFU begin: size=%u crc32=0x%08x upload-area=%u",
		dfu.image_size, dfu.image_crc32_expected,
		flash_img_get_upload_slot());

	return 0;
}

static int command_write_chunk(void)
{
	bool flush;
	uint32_t chunk_crc32;
	int ret;

	if (dfu.status != ARGISENSE_DFU_STATUS_RECEIVING || !dfu.context_open) {
		return -EACCES;
	}

	if (dfu.chunk_length == 0U ||
	    dfu.chunk_length > CONFIG_ARGISENSE_RS485_DFU_MAX_CHUNK_BYTES) {
		return -EINVAL;
	}

	if (dfu.chunk_offset != dfu.bytes_received) {
		LOG_WRN("RS485 DFU offset mismatch: got=%u expected=%u",
			dfu.chunk_offset, dfu.bytes_received);
		return -ESPIPE;
	}

	if ((dfu.chunk_offset + dfu.chunk_length) > dfu.image_size) {
		return -EFBIG;
	}

	chunk_crc32 = crc32_ieee(dfu.chunk, dfu.chunk_length);
	if (chunk_crc32 != dfu.chunk_crc32_expected) {
		LOG_WRN("RS485 DFU chunk CRC mismatch: got=0x%08x expected=0x%08x",
			chunk_crc32, dfu.chunk_crc32_expected);
		return -EBADMSG;
	}

	flush = (dfu.chunk_offset + dfu.chunk_length) == dfu.image_size;
	ret = flash_img_buffered_write(&dfu.flash_ctx, dfu.chunk,
				       dfu.chunk_length, flush);
	if (ret < 0) {
		return ret;
	}

	dfu.image_crc32_running =
		crc32_ieee_update(dfu.image_crc32_running, dfu.chunk,
				  dfu.chunk_length);
	dfu.bytes_received += dfu.chunk_length;

	if (flush) {
		dfu.context_open = false;
		LOG_INF("RS485 DFU upload received: %u bytes crc32=0x%08x",
			dfu.bytes_received, dfu.image_crc32_running);
	}

	return 0;
}

static int command_verify(void)
{
	struct flash_img_check check = {
		.match = dfu.sha256,
		.clen = dfu.image_size,
	};
	int ret;

	if (dfu.bytes_received != dfu.image_size || dfu.image_size == 0U) {
		return -EAGAIN;
	}

	if (dfu.context_open) {
		return -EBUSY;
	}

	if (dfu.image_crc32_running != dfu.image_crc32_expected) {
		LOG_WRN("RS485 DFU image CRC mismatch: got=0x%08x expected=0x%08x",
			dfu.image_crc32_running, dfu.image_crc32_expected);
		return -EBADMSG;
	}

	dfu.status = ARGISENSE_DFU_STATUS_VERIFYING;
	ret = flash_img_check(&dfu.flash_ctx, &check, flash_img_get_upload_slot());
	if (ret < 0) {
		return ret;
	}

	dfu.verified = true;
	dfu.status = ARGISENSE_DFU_STATUS_VERIFIED;
	LOG_INF("RS485 DFU image verified in secondary slot");

	return 0;
}

static int command_test(bool reboot)
{
	int ret;

	if (!dfu.verified) {
		return -EACCES;
	}

	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret < 0) {
		return ret;
	}

	dfu.status = ARGISENSE_DFU_STATUS_PENDING;
	LOG_INF("RS485 DFU image marked pending for MCUboot test swap");

	if (reboot) {
		ret = k_work_schedule(&dfu_reboot_work, K_MSEC(500));
		if (ret >= 0) {
			ret = 0;
		}
	}

	return ret;
}

static int command_abort(void)
{
	close_context_if_open();
	dfu.bytes_received = 0U;
	dfu.image_crc32_running = 0U;
	dfu.verified = false;
	dfu.status = ARGISENSE_DFU_STATUS_IDLE;
	LOG_INF("RS485 DFU aborted");

	return 0;
}

static int execute_command(uint16_t command)
{
	int ret;

	clear_error();
	dfu.last_command = command;

	switch (command) {
	case ARGISENSE_DFU_CMD_NONE:
		ret = 0;
		break;
	case ARGISENSE_DFU_CMD_BEGIN:
		ret = command_begin();
		break;
	case ARGISENSE_DFU_CMD_WRITE:
		ret = command_write_chunk();
		break;
	case ARGISENSE_DFU_CMD_VERIFY:
		ret = command_verify();
		break;
	case ARGISENSE_DFU_CMD_TEST:
		ret = command_test(false);
		break;
	case ARGISENSE_DFU_CMD_TEST_REBOOT:
		ret = command_test(true);
		break;
	case ARGISENSE_DFU_CMD_ABORT:
		ret = command_abort();
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

	if (ret < 0) {
		set_error(ret);
		LOG_WRN("RS485 DFU command 0x%04x failed: %d", command, ret);
	}

	return ret;
}

bool argisense_dfu_register_is_supported(uint16_t addr)
{
	return (addr >= ARGISENSE_DFU_REG_CONTROL &&
		addr <= ARGISENSE_DFU_REG_LAST_COMMAND) ||
	       (addr >= ARGISENSE_DFU_REG_SHA256_BASE &&
		addr < (ARGISENSE_DFU_REG_SHA256_BASE + ARGISENSE_DFU_REG_SHA256_COUNT)) ||
	       (addr >= ARGISENSE_DFU_REG_DATA_BASE &&
		addr < (ARGISENSE_DFU_REG_DATA_BASE + ARGISENSE_DFU_REG_DATA_COUNT));
}

int argisense_dfu_register_read(uint16_t addr, uint16_t *reg)
{
	int ret = 0;

	if (reg == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&dfu.lock, K_FOREVER);

	if (addr >= ARGISENSE_DFU_REG_SHA256_BASE &&
	    addr < (ARGISENSE_DFU_REG_SHA256_BASE + ARGISENSE_DFU_REG_SHA256_COUNT)) {
		const uint16_t idx = addr - ARGISENSE_DFU_REG_SHA256_BASE;

		*reg = ((uint16_t)dfu.sha256[idx * 2U] << 8) |
		       dfu.sha256[(idx * 2U) + 1U];
		goto out;
	}

	if (addr >= ARGISENSE_DFU_REG_DATA_BASE &&
	    addr < (ARGISENSE_DFU_REG_DATA_BASE + ARGISENSE_DFU_REG_DATA_COUNT)) {
		const uint16_t idx = addr - ARGISENSE_DFU_REG_DATA_BASE;

		*reg = ((uint16_t)dfu.chunk[idx * 2U] << 8) |
		       dfu.chunk[(idx * 2U) + 1U];
		goto out;
	}

	switch (addr) {
	case ARGISENSE_DFU_REG_CONTROL:
		*reg = ARGISENSE_DFU_CMD_NONE;
		break;
	case ARGISENSE_DFU_REG_STATUS:
		*reg = dfu.status;
		break;
	case ARGISENSE_DFU_REG_ERROR:
		*reg = (uint16_t)dfu.last_error;
		break;
	case ARGISENSE_DFU_REG_IMAGE_SIZE_HI:
		*reg = u32_hi(dfu.image_size);
		break;
	case ARGISENSE_DFU_REG_IMAGE_SIZE_LO:
		*reg = u32_lo(dfu.image_size);
		break;
	case ARGISENSE_DFU_REG_IMAGE_CRC32_HI:
		*reg = u32_hi(dfu.image_crc32_expected);
		break;
	case ARGISENSE_DFU_REG_IMAGE_CRC32_LO:
		*reg = u32_lo(dfu.image_crc32_expected);
		break;
	case ARGISENSE_DFU_REG_BYTES_WRITTEN_HI:
		*reg = u32_hi(dfu.bytes_received);
		break;
	case ARGISENSE_DFU_REG_BYTES_WRITTEN_LO:
		*reg = u32_lo(dfu.bytes_received);
		break;
	case ARGISENSE_DFU_REG_CHUNK_OFFSET_HI:
		*reg = u32_hi(dfu.chunk_offset);
		break;
	case ARGISENSE_DFU_REG_CHUNK_OFFSET_LO:
		*reg = u32_lo(dfu.chunk_offset);
		break;
	case ARGISENSE_DFU_REG_CHUNK_LENGTH:
		*reg = dfu.chunk_length;
		break;
	case ARGISENSE_DFU_REG_CHUNK_CRC32_HI:
		*reg = u32_hi(dfu.chunk_crc32_expected);
		break;
	case ARGISENSE_DFU_REG_CHUNK_CRC32_LO:
		*reg = u32_lo(dfu.chunk_crc32_expected);
		break;
	case ARGISENSE_DFU_REG_MAX_CHUNK_BYTES:
		*reg = CONFIG_ARGISENSE_RS485_DFU_MAX_CHUNK_BYTES;
		break;
	case ARGISENSE_DFU_REG_LAST_COMMAND:
		*reg = dfu.last_command;
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

out:
	k_mutex_unlock(&dfu.lock);
	return ret;
}

int argisense_dfu_register_write(uint16_t addr, uint16_t reg)
{
	int ret = 0;

	k_mutex_lock(&dfu.lock, K_FOREVER);

	if (addr >= ARGISENSE_DFU_REG_SHA256_BASE &&
	    addr < (ARGISENSE_DFU_REG_SHA256_BASE + ARGISENSE_DFU_REG_SHA256_COUNT)) {
		const uint16_t idx = addr - ARGISENSE_DFU_REG_SHA256_BASE;

		dfu.sha256[idx * 2U] = (uint8_t)(reg >> 8);
		dfu.sha256[(idx * 2U) + 1U] = (uint8_t)reg;
		goto out;
	}

	if (addr >= ARGISENSE_DFU_REG_DATA_BASE &&
	    addr < (ARGISENSE_DFU_REG_DATA_BASE + ARGISENSE_DFU_REG_DATA_COUNT)) {
		const uint16_t idx = addr - ARGISENSE_DFU_REG_DATA_BASE;

		dfu.chunk[idx * 2U] = (uint8_t)(reg >> 8);
		dfu.chunk[(idx * 2U) + 1U] = (uint8_t)reg;
		goto out;
	}

	switch (addr) {
	case ARGISENSE_DFU_REG_CONTROL:
		ret = execute_command(reg);
		break;
	case ARGISENSE_DFU_REG_IMAGE_SIZE_HI:
		dfu.image_size = u32_from_words(reg, u32_lo(dfu.image_size));
		dfu.verified = false;
		break;
	case ARGISENSE_DFU_REG_IMAGE_SIZE_LO:
		dfu.image_size = u32_from_words(u32_hi(dfu.image_size), reg);
		dfu.verified = false;
		break;
	case ARGISENSE_DFU_REG_IMAGE_CRC32_HI:
		dfu.image_crc32_expected =
			u32_from_words(reg, u32_lo(dfu.image_crc32_expected));
		dfu.verified = false;
		break;
	case ARGISENSE_DFU_REG_IMAGE_CRC32_LO:
		dfu.image_crc32_expected =
			u32_from_words(u32_hi(dfu.image_crc32_expected), reg);
		dfu.verified = false;
		break;
	case ARGISENSE_DFU_REG_CHUNK_OFFSET_HI:
		dfu.chunk_offset = u32_from_words(reg, u32_lo(dfu.chunk_offset));
		break;
	case ARGISENSE_DFU_REG_CHUNK_OFFSET_LO:
		dfu.chunk_offset = u32_from_words(u32_hi(dfu.chunk_offset), reg);
		break;
	case ARGISENSE_DFU_REG_CHUNK_LENGTH:
		if (reg > CONFIG_ARGISENSE_RS485_DFU_MAX_CHUNK_BYTES) {
			ret = -EINVAL;
		} else {
			dfu.chunk_length = reg;
		}
		break;
	case ARGISENSE_DFU_REG_CHUNK_CRC32_HI:
		dfu.chunk_crc32_expected =
			u32_from_words(reg, u32_lo(dfu.chunk_crc32_expected));
		break;
	case ARGISENSE_DFU_REG_CHUNK_CRC32_LO:
		dfu.chunk_crc32_expected =
			u32_from_words(u32_hi(dfu.chunk_crc32_expected), reg);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}

out:
	k_mutex_unlock(&dfu.lock);
	return ret;
}

const char *argisense_dfu_register_name(uint16_t addr)
{
	if (addr >= ARGISENSE_DFU_REG_SHA256_BASE &&
	    addr < (ARGISENSE_DFU_REG_SHA256_BASE + ARGISENSE_DFU_REG_SHA256_COUNT)) {
		return "dfu_sha256";
	}

	if (addr >= ARGISENSE_DFU_REG_DATA_BASE &&
	    addr < (ARGISENSE_DFU_REG_DATA_BASE + ARGISENSE_DFU_REG_DATA_COUNT)) {
		return "dfu_chunk_data";
	}

	switch (addr) {
	case ARGISENSE_DFU_REG_CONTROL:
		return "dfu_control";
	case ARGISENSE_DFU_REG_STATUS:
		return "dfu_status";
	case ARGISENSE_DFU_REG_ERROR:
		return "dfu_error";
	case ARGISENSE_DFU_REG_IMAGE_SIZE_HI:
		return "dfu_image_size_hi";
	case ARGISENSE_DFU_REG_IMAGE_SIZE_LO:
		return "dfu_image_size_lo";
	case ARGISENSE_DFU_REG_IMAGE_CRC32_HI:
		return "dfu_image_crc32_hi";
	case ARGISENSE_DFU_REG_IMAGE_CRC32_LO:
		return "dfu_image_crc32_lo";
	case ARGISENSE_DFU_REG_BYTES_WRITTEN_HI:
		return "dfu_bytes_written_hi";
	case ARGISENSE_DFU_REG_BYTES_WRITTEN_LO:
		return "dfu_bytes_written_lo";
	case ARGISENSE_DFU_REG_CHUNK_OFFSET_HI:
		return "dfu_chunk_offset_hi";
	case ARGISENSE_DFU_REG_CHUNK_OFFSET_LO:
		return "dfu_chunk_offset_lo";
	case ARGISENSE_DFU_REG_CHUNK_LENGTH:
		return "dfu_chunk_length";
	case ARGISENSE_DFU_REG_CHUNK_CRC32_HI:
		return "dfu_chunk_crc32_hi";
	case ARGISENSE_DFU_REG_CHUNK_CRC32_LO:
		return "dfu_chunk_crc32_lo";
	case ARGISENSE_DFU_REG_MAX_CHUNK_BYTES:
		return "dfu_max_chunk_bytes";
	case ARGISENSE_DFU_REG_LAST_COMMAND:
		return "dfu_last_command";
	default:
		return NULL;
	}
}
