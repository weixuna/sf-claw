/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Shell command history — fixed-size ring buffer.
 */

#include "claw/shell/shell_history.h"

#include <string.h>

static char  s_ring[SHELL_HISTORY_MAX][SHELL_HISTORY_LINE];
static int   s_head;    /* next write slot */
static int   s_count;   /* entries stored (max SHELL_HISTORY_MAX) */

/* Navigation state — reset on each new prompt */
static int   s_nav_pos; /* -1 = not navigating, 0..count-1 = history idx */
static char  s_scratch[SHELL_HISTORY_LINE]; /* unsaved current input */

void shell_history_add(const char *line)
{
    if (!line || line[0] == '\0') {
        return;
    }

    /* Skip duplicate of the most recent entry */
    if (s_count > 0) {
        int last = (s_head - 1 + SHELL_HISTORY_MAX) % SHELL_HISTORY_MAX;
        if (strcmp(s_ring[last], line) == 0) {
            return;
        }
    }

    strncpy(s_ring[s_head], line, SHELL_HISTORY_LINE - 1);
    s_ring[s_head][SHELL_HISTORY_LINE - 1] = '\0';
    s_head = (s_head + 1) % SHELL_HISTORY_MAX;
    if (s_count < SHELL_HISTORY_MAX) {
        s_count++;
    }
}

const char *shell_history_get(int index)
{
    if (index < 0 || index >= s_count) {
        return NULL;
    }
    int slot = (s_head - 1 - index + SHELL_HISTORY_MAX * 2)
               % SHELL_HISTORY_MAX;
    return s_ring[slot];
}

int shell_history_count(void)
{
    return s_count;
}

void shell_history_reset_nav(void)
{
    s_nav_pos = -1;
    s_scratch[0] = '\0';
}

const char *shell_history_navigate(int dir, const char *current)
{
    if (dir == -1) {
        /* Up — go older */
        int next = s_nav_pos + 1;
        if (next >= s_count) {
            return NULL; /* already at oldest */
        }
        /* Save current input on first up-press */
        if (s_nav_pos == -1 && current) {
            strncpy(s_scratch, current, SHELL_HISTORY_LINE - 1);
            s_scratch[SHELL_HISTORY_LINE - 1] = '\0';
        }
        s_nav_pos = next;
        return shell_history_get(s_nav_pos);
    }

    if (dir == 1) {
        /* Down — go newer */
        if (s_nav_pos <= 0) {
            if (s_nav_pos == 0) {
                /* Return to scratch (unsaved input) */
                s_nav_pos = -1;
                return s_scratch;
            }
            return NULL; /* not navigating */
        }
        s_nav_pos--;
        return shell_history_get(s_nav_pos);
    }

    return NULL;
}
