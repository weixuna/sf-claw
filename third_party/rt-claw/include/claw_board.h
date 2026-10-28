/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Unified board-level abstraction.
 * Each platform/board variant provides its own implementation.
 */

#ifndef CLAW_BOARD_H
#define CLAW_BOARD_H

#include "claw/shell/shell_commands.h"

/*
 * Board early initialization (called before claw_init).
 * WiFi boards: initialize and connect WiFi.
 * QEMU boards: no-op (Ethernet configured via sdkconfig).
 */
void board_early_init(void);

/* Return board-specific shell commands (NULL if none). */
const shell_cmd_t *board_platform_commands(int *count);

#ifdef CLAW_PLATFORM_ESP_IDF
/*
 * WiFi board helpers — implemented in platform/common/espressif/wifi_board.c.
 * Called by real-hardware board.c files that need WiFi.
 */
void wifi_board_early_init(void);
const shell_cmd_t *wifi_board_platform_commands(int *count);
#endif

#endif /* CLAW_BOARD_H */
