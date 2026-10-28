/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * QEMU vexpress-a9 board — network via SMC911x Ethernet.
 */

#include "claw_board.h"
#include <stddef.h>

void board_early_init(void)
{
    /* Network is initialized by RT-Thread BSP drivers */
}

const shell_cmd_t *board_platform_commands(int *count)
{
    *count = 0;
    return NULL;
}
