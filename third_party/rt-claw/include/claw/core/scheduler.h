/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Task scheduler — runs callbacks at fixed intervals.
 */

#ifndef CLAW_SCHEDULER_H
#define CLAW_SCHEDULER_H

#include "osal/claw_os.h"
#include "claw_config.h"

typedef void (*sched_callback_t)(void *arg);

int  sched_init(void);
void sched_stop(void);
int  sched_add(const char *name, uint32_t interval_ms, int32_t count,
               sched_callback_t cb, void *arg);
int  sched_remove(const char *name);
void sched_list(void);
int  sched_task_count(void);

/**
 * Write task list into a text buffer (for LLM tool output).
 * Returns bytes written (excluding null terminator).
 */
int  sched_list_to_buf(char *buf, size_t size);

#endif /* CLAW_SCHEDULER_H */
