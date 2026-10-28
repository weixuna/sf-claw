/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Tool framework — register hardware capabilities as LLM tools.
 *
 * Tools self-register via CLAW_TOOL_REGISTER() at link time (or
 * constructor time on ESP-IDF).  claw_tools_init() logs the count;
 * lookup and JSON export iterate the OOP registry.
 */

#ifndef CLAW_TOOLS_H
#define CLAW_TOOLS_H

#include "osal/claw_os.h"
#include "claw/core/claw_tool.h"
#include "cJSON.h"

/**
 * Initialize tool subsystem. Call once at startup.
 */
int claw_tools_init(void);
void claw_tools_stop(void);

/**
 * Get number of registered tools.
 */
int claw_tools_count(void);

/**
 * Find tool by name. Returns NULL if not found.
 */
const struct claw_tool *claw_tool_find(const char *name);

/**
 * Build cJSON array of all tool definitions for Claude API.
 * Caller must cJSON_Delete() the result.
 */
cJSON *claw_tools_to_json(void);

/**
 * Build cJSON array of tool definitions, excluding names that start
 * with the given prefix.  Used by background AI calls (scheduled tasks,
 * skills) to omit slow I/O tools like lcd_*.
 * Caller must cJSON_Delete() the result.
 */
cJSON *claw_tools_to_json_exclude(const char *prefix);

/**
 * Scheduled-task reply callback.
 * @param target  Opaque destination (e.g. Feishu chat_id)
 * @param text    Reply text to deliver
 */
typedef void (*sched_reply_fn_t)(const char *target, const char *text);

/**
 * Set the reply context for the NEXT scheduled task registration.
 * Call before ai_chat(); tool_schedule_task() captures the context
 * so that subsequent scheduled firings route replies accordingly.
 * Pass NULL to reset (replies go to serial console).
 *
 * Thread-safety: protected by an internal mutex.
 */
void sched_set_reply_context(sched_reply_fn_t fn, const char *target);

/**
 * Remove a scheduled AI task by name (scheduler + NVS).
 * Callable from shell commands.
 */
int sched_tool_remove_by_name(const char *name);
void sched_tool_stop(void);

/**
 * Initialize LCD panel (QEMU RGB framebuffer).
 * Call before claw_tools_init() so the panel is ready.
 */
int claw_lcd_init(void);

/**
 * Check whether LCD hardware was initialized successfully.
 */
int claw_lcd_available(void);

/**
 * Show status message on LCD bottom area.
 */
void claw_lcd_status(const char *msg);

/**
 * Draw progress bar on LCD bottom strip (0-100%).
 */
void claw_lcd_progress(int percent);

#endif /* CLAW_TOOLS_H */
