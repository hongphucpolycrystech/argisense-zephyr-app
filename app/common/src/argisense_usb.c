/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(argisense_usb, LOG_LEVEL_INF);

USBD_DEVICE_DEFINE(argisense_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_ARGISENSE_USB_VID, CONFIG_ARGISENSE_USB_PID);

USBD_DESC_LANG_DEFINE(argisense_usbd_lang);
USBD_DESC_MANUFACTURER_DEFINE(argisense_usbd_mfr,
			      CONFIG_ARGISENSE_USB_MANUFACTURER_STRING);
USBD_DESC_PRODUCT_DEFINE(argisense_usbd_product,
			 CONFIG_ARGISENSE_USB_PRODUCT_STRING);
IF_ENABLED(CONFIG_HWINFO,
	   (USBD_DESC_SERIAL_NUMBER_DEFINE(argisense_usbd_sn)));

USBD_DESC_CONFIG_DEFINE(argisense_usbd_fs_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(argisense_usbd_hs_desc, "HS Configuration");

static const uint8_t argisense_usbd_attributes =
	IS_ENABLED(CONFIG_ARGISENSE_USB_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0;

USBD_CONFIGURATION_DEFINE(argisense_usbd_fs_config,
			  argisense_usbd_attributes,
			  CONFIG_ARGISENSE_USB_MAX_POWER,
			  &argisense_usbd_fs_desc);

USBD_CONFIGURATION_DEFINE(argisense_usbd_hs_config,
			  argisense_usbd_attributes,
			  CONFIG_ARGISENSE_USB_MAX_POWER,
			  &argisense_usbd_hs_desc);

static void argisense_usbd_msg_cb(struct usbd_context *const ctx,
				  const struct usbd_msg *msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			int err = usbd_enable(ctx);

			if (err != 0) {
				LOG_ERR("USB enable on VBUS ready failed: %d", err);
			} else {
				LOG_INF("USB VBUS ready; device enabled");
			}
		} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
			int err = usbd_disable(ctx);

			if (err != 0) {
				LOG_ERR("USB disable on VBUS removed failed: %d", err);
			} else {
				LOG_INF("USB VBUS removed; device disabled");
			}
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		LOG_DBG("USB CDC ACM control-line-state on %s", msg->dev->name);
	} else if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
		LOG_DBG("USB CDC ACM line-coding on %s", msg->dev->name);
	}
}

static int argisense_usbd_add_descriptors(void)
{
	int err;

	err = usbd_add_descriptor(&argisense_usbd, &argisense_usbd_lang);
	if (err != 0) {
		LOG_ERR("USB language descriptor add failed: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&argisense_usbd, &argisense_usbd_mfr);
	if (err != 0) {
		LOG_ERR("USB manufacturer descriptor add failed: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&argisense_usbd, &argisense_usbd_product);
	if (err != 0) {
		LOG_ERR("USB product descriptor add failed: %d", err);
		return err;
	}

	IF_ENABLED(CONFIG_HWINFO, (
		err = usbd_add_descriptor(&argisense_usbd, &argisense_usbd_sn);
	))
	if (err != 0) {
		LOG_ERR("USB serial-number descriptor add failed: %d", err);
		return err;
	}

	return 0;
}

static int argisense_usbd_add_configuration(enum usbd_speed speed)
{
	struct usbd_config_node *cfg =
		(speed == USBD_SPEED_HS) ? &argisense_usbd_hs_config :
					   &argisense_usbd_fs_config;
	int err;

	err = usbd_add_configuration(&argisense_usbd, speed, cfg);
	if (err != 0) {
		LOG_ERR("USB configuration add failed for speed %u: %d",
			(unsigned int)speed, err);
		return err;
	}

	err = usbd_register_all_classes(&argisense_usbd, speed, 1, NULL);
	if (err != 0) {
		LOG_ERR("USB class registration failed for speed %u: %d",
			(unsigned int)speed, err);
		return err;
	}

	return usbd_device_set_code_triple(&argisense_usbd, speed,
					   USB_BCC_MISCELLANEOUS, 0x02, 0x01);
}

static int argisense_usbd_init_device(void)
{
	int err;

	err = argisense_usbd_add_descriptors();
	if (err != 0) {
		return err;
	}

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&argisense_usbd) == USBD_SPEED_HS) {
		err = argisense_usbd_add_configuration(USBD_SPEED_HS);
		if (err != 0) {
			return err;
		}
	}

	err = argisense_usbd_add_configuration(USBD_SPEED_FS);
	if (err != 0) {
		return err;
	}

	usbd_self_powered(&argisense_usbd,
			  argisense_usbd_attributes & USB_SCD_SELF_POWERED);

	err = usbd_msg_register_cb(&argisense_usbd, argisense_usbd_msg_cb);
	if (err != 0) {
		LOG_ERR("USB message callback registration failed: %d", err);
		return err;
	}

	err = usbd_init(&argisense_usbd);
	if (err != 0) {
		LOG_ERR("USB device init failed: %d", err);
		return err;
	}

	if (!usbd_can_detect_vbus(&argisense_usbd)) {
		err = usbd_enable(&argisense_usbd);
		if (err != 0) {
			LOG_ERR("USB device enable failed: %d", err);
			return err;
		}
	}

	LOG_INF("USB-C device initialized: product='%s' pid=0x%04x",
		CONFIG_ARGISENSE_USB_PRODUCT_STRING,
		CONFIG_ARGISENSE_USB_PID);

	return 0;
}

SYS_INIT(argisense_usbd_init_device, APPLICATION,
	 CONFIG_APPLICATION_INIT_PRIORITY);
