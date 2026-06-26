/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "argisense_rs485.h"

#include "argisense_registers.h"
#include "argisense_settings.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>

LOG_MODULE_REGISTER(argisense_rs485, LOG_LEVEL_INF);

#define ARGISENSE_RS485_MODBUS_NODE DT_NODELABEL(rs485_modbus)

static int rs485_iface = -ENODEV;

static int argisense_rs485_holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	return argisense_register_read_holding(addr, reg);
}

static int argisense_rs485_holding_reg_wr(uint16_t addr, uint16_t reg)
{
	return argisense_register_write_holding(addr, reg);
}

static struct modbus_user_callbacks argisense_rs485_callbacks = {
	.holding_reg_rd = argisense_rs485_holding_reg_rd,
	.holding_reg_wr = argisense_rs485_holding_reg_wr,
};

int argisense_rs485_init(void)
{
#if DT_NODE_HAS_STATUS(ARGISENSE_RS485_MODBUS_NODE, okay)
	const struct argisense_runtime_config *config = argisense_settings_get();
	const char iface_name[] = { DEVICE_DT_NAME(ARGISENSE_RS485_MODBUS_NODE) };
	struct modbus_iface_param server_param = {
		.mode = MODBUS_MODE_RTU,
		.server = {
			.user_cb = &argisense_rs485_callbacks,
			.unit_id = config->modbus_address,
		},
		.serial = {
			.baud = config->rs485_baudrate,
			.parity = UART_CFG_PARITY_NONE,
		},
	};
	int ret;

	rs485_iface = modbus_iface_get_by_name(iface_name);
	if (rs485_iface < 0) {
		LOG_ERR("Failed to get Modbus interface %s: %d", iface_name,
			rs485_iface);
		return rs485_iface;
	}

	ret = modbus_init_server(rs485_iface, server_param);
	if (ret < 0) {
		LOG_ERR("Failed to start Modbus RTU server on %s: %d",
			iface_name, ret);
		return ret;
	}

	LOG_INF("RS485 Modbus RTU server ready on %s unit=%u baud=%u",
		iface_name, config->modbus_address, config->rs485_baudrate);
	LOG_INF("Register map v%u: live 0..33, config 40..59, diagnostics 70..82",
		ARGISENSE_REGISTER_MAP_VERSION);

	return 0;
#else
	ARG_UNUSED(rs485_iface);
	LOG_ERR("Board must define an okay rs485_modbus node");
	return -ENODEV;
#endif
}
