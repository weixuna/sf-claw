/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Platform entry for Linux native.
 */

#include <stdio.h>
#include <signal.h>

#include "osal/claw_os.h"
#include "osal/claw_kv.h"
#include "claw_board.h"
#include "claw/shell/shell_commands.h"

extern int claw_init(void);
extern void claw_deinit(void);
extern void linux_shell_loop(void);

volatile sig_atomic_t g_exit_flag;

static void signal_handler(int sig)
{
    (void)sig;
    g_exit_flag = 1;
}

int main(void)
{
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("rt-claw: Linux native - Real-Time Claw\n");

    claw_kv_init();
    board_early_init();
    shell_nvs_config_load();
    claw_init();
    linux_shell_loop();

    claw_deinit();

    return 0;
}
