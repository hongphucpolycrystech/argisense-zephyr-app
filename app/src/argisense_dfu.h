/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARGISENSE_DFU_H_
#define ARGISENSE_DFU_H_

#include <stdbool.h>
#include <stdint.h>

#include <errno.h>
#include <zephyr/sys/util.h>

#define ARGISENSE_DFU_REG_CONTROL              1000U
#define ARGISENSE_DFU_REG_STATUS               1001U
#define ARGISENSE_DFU_REG_ERROR                1002U
#define ARGISENSE_DFU_REG_IMAGE_SIZE_HI        1003U
#define ARGISENSE_DFU_REG_IMAGE_SIZE_LO        1004U
#define ARGISENSE_DFU_REG_IMAGE_CRC32_HI       1005U
#define ARGISENSE_DFU_REG_IMAGE_CRC32_LO       1006U
#define ARGISENSE_DFU_REG_BYTES_WRITTEN_HI     1007U
#define ARGISENSE_DFU_REG_BYTES_WRITTEN_LO     1008U
#define ARGISENSE_DFU_REG_CHUNK_OFFSET_HI      1009U
#define ARGISENSE_DFU_REG_CHUNK_OFFSET_LO      1010U
#define ARGISENSE_DFU_REG_CHUNK_LENGTH         1011U
#define ARGISENSE_DFU_REG_CHUNK_CRC32_HI       1012U
#define ARGISENSE_DFU_REG_CHUNK_CRC32_LO       1013U
#define ARGISENSE_DFU_REG_MAX_CHUNK_BYTES      1014U
#define ARGISENSE_DFU_REG_LAST_COMMAND         1015U
#define ARGISENSE_DFU_REG_UNLOCK_KEY           1016U
#define ARGISENSE_DFU_REG_UNLOCK_REMAINING_S   1017U
#define ARGISENSE_DFU_REG_SHA256_BASE          1020U
#define ARGISENSE_DFU_REG_SHA256_COUNT         16U
#define ARGISENSE_DFU_REG_DATA_BASE            1100U

#if defined(CONFIG_ARGISENSE_RS485_DFU)

#define ARGISENSE_DFU_REG_DATA_COUNT \
	(CONFIG_ARGISENSE_RS485_DFU_MAX_CHUNK_BYTES / 2U)

bool argisense_dfu_register_is_supported(uint16_t addr);
int argisense_dfu_register_read(uint16_t addr, uint16_t *reg);
int argisense_dfu_register_write(uint16_t addr, uint16_t reg);
const char *argisense_dfu_register_name(uint16_t addr);

#else

static inline bool argisense_dfu_register_is_supported(uint16_t addr)
{
	ARG_UNUSED(addr);
	return false;
}

static inline int argisense_dfu_register_read(uint16_t addr, uint16_t *reg)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(reg);
	return -ENOTSUP;
}

static inline int argisense_dfu_register_write(uint16_t addr, uint16_t reg)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(reg);
	return -ENOTSUP;
}

static inline const char *argisense_dfu_register_name(uint16_t addr)
{
	ARG_UNUSED(addr);
	return NULL;
}

#endif

#endif /* ARGISENSE_DFU_H_ */
