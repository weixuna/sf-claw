/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Console I/O driver for Espressif SoCs.
 * Abstracts UART vs USB-JTAG CDC based on sdkconfig.
 */

#ifndef CLAW_DRIVERS_SERIAL_ESPRESSIF_CONSOLE_H
#define CLAW_DRIVERS_SERIAL_ESPRESSIF_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* Install the console driver (UART or USB-JTAG CDC). */
void claw_console_init(void);

/* Read bytes from console. Returns number of bytes read. */
int claw_console_read(void *buf, uint32_t len, uint32_t timeout_ms);

/* Write bytes to console. Returns number of bytes written. */
int claw_console_write(const void *buf, size_t len);

#endif /* CLAW_DRIVERS_SERIAL_ESPRESSIF_CONSOLE_H */
