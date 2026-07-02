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

LOG_MODULE_REGISTER(argisense_ph_rs485, LOG_LEVEL_INF);

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

static enum uart_config_parity rs485_uart_parity(uint8_t parity)
{
	switch (parity) {
	case ARGISENSE_RS485_PARITY_ODD:
		return UART_CFG_PARITY_ODD;
	case ARGISENSE_RS485_PARITY_EVEN:
		return UART_CFG_PARITY_EVEN;
	case ARGISENSE_RS485_PARITY_NONE:
	default:
		return UART_CFG_PARITY_NONE;
	}
}

static enum uart_config_stop_bits rs485_uart_stop_bits(uint8_t stop_bits)
{
	switch (stop_bits) {
	case ARGISENSE_RS485_STOP_BITS_1:
		return UART_CFG_STOP_BITS_1;
	case ARGISENSE_RS485_STOP_BITS_2:
	default:
		return UART_CFG_STOP_BITS_2;
	}
}

static const char *rs485_parity_name(uint8_t parity)
{
	switch (parity) {
	case ARGISENSE_RS485_PARITY_NONE:
		return "none";
	case ARGISENSE_RS485_PARITY_ODD:
		return "odd";
	case ARGISENSE_RS485_PARITY_EVEN:
		return "even";
	default:
		return "invalid";
	}
}

int argisense_rs485_init(void)
{
#if DT_NODE_HAS_STATUS(ARGISENSE_RS485_MODBUS_NODE, okay)
	struct argisense_runtime_config config;
	const char iface_name[] = { DEVICE_DT_NAME(ARGISENSE_RS485_MODBUS_NODE) };
	struct modbus_iface_param server_param = {
		.mode = MODBUS_MODE_RTU,
		.server = {
			.user_cb = &argisense_rs485_callbacks,
		},
	};
	int ret;

	argisense_settings_get_copy(&config);

	server_param.server.unit_id = config.modbus_address;
	server_param.serial.baud = config.rs485_baudrate;
	server_param.serial.parity = rs485_uart_parity(config.rs485_parity);
	server_param.serial.stop_bits = rs485_uart_stop_bits(config.rs485_stop_bits);

	rs485_iface = modbus_iface_get_by_name(iface_name);
	if (rs485_iface < 0) {
		LOG_ERR("Failed to get pH Modbus interface %s: %d", iface_name,
			rs485_iface);
		return rs485_iface;
	}

	ret = modbus_init_server(rs485_iface, server_param);
	if (ret < 0) {
		LOG_ERR("Failed to start pH Modbus RTU server on %s: %d",
			iface_name, ret);
		return ret;
	}

	LOG_INF("pH RS485 Modbus RTU server ready on %s unit=%u baud=%u data-bits=%u parity=%s stop-bits=%u",
		iface_name, config.modbus_address, config.rs485_baudrate,
		config.rs485_data_bits, rs485_parity_name(config.rs485_parity),
		config.rs485_stop_bits);
	LOG_INF("pH register map v%u: live 0..36, config 40..59, diagnostics 70..76",
		ARGISENSE_REGISTER_MAP_VERSION);

	return 0;
#else
	ARG_UNUSED(rs485_iface);
	LOG_ERR("pH board must define an okay rs485_modbus node");
	return -ENODEV;
#endif
}
