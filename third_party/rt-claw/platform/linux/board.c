/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Board abstraction for Linux native platform.
 */

#include "claw_board.h"

void board_early_init(void)
{
    /* No hardware init needed on Linux */
}

const shell_cmd_t *board_platform_commands(int *count)
{
    *count = 0;
    return NULL;
}
