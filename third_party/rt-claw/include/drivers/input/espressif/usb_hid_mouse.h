/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * USB HID Mouse driver — ESP32-S3 USB OTG via TinyUSB.
 */

#ifndef CLAW_DRIVERS_INPUT_ESPRESSIF_USB_HID_MOUSE_H
#define CLAW_DRIVERS_INPUT_ESPRESSIF_USB_HID_MOUSE_H

#include <stdint.h>
#include "claw/core/claw_errno.h"

/* Mouse button bitmask (matches HID standard) */
#define USB_HID_MOUSE_BTN_LEFT    (1 << 0)
#define USB_HID_MOUSE_BTN_RIGHT   (1 << 1)
#define USB_HID_MOUSE_BTN_MIDDLE  (1 << 2)

/**
 * Initialize USB HID mouse device via TinyUSB.
 * Configures USB OTG as a HID mouse (GPIO 19/20).
 *
 * @return CLAW_OK on success, CLAW_ERROR on failure
 */
claw_err_t usb_hid_mouse_init(void);

/**
 * Move the mouse cursor by relative delta.
 *
 * @param dx  Horizontal movement (-127 to 127)
 * @param dy  Vertical movement (-127 to 127)
 * @return CLAW_OK on success
 */
claw_err_t usb_hid_mouse_move(int8_t dx, int8_t dy);

/**
 * Send a mouse button click (press + release).
 *
 * @param buttons  Bitmask of USB_HID_MOUSE_BTN_* values
 * @return CLAW_OK on success
 */
claw_err_t usb_hid_mouse_click(uint8_t buttons);

/**
 * Send a mouse scroll event.
 *
 * @param delta  Scroll amount (-127 to 127, positive = up)
 * @return CLAW_OK on success
 */
claw_err_t usb_hid_mouse_scroll(int8_t delta);

/**
 * Check if USB HID mouse is connected to host.
 *
 * @return 1 if mounted (connected), 0 otherwise
 */
int usb_hid_mouse_is_ready(void);

#endif /* CLAW_DRIVERS_INPUT_ESPRESSIF_USB_HID_MOUSE_H */
