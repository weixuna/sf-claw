/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef CLAW_SERVICES_AI_ENGINE_H
#define CLAW_SERVICES_AI_ENGINE_H

#include "osal/claw_os.h"

/* Channel IDs for conversation memory isolation */
#define AI_CHANNEL_SHELL      0
#define AI_CHANNEL_FEISHU     1
#define AI_CHANNEL_TELEGRAM   2
#define AI_CHANNEL_SCHED      3
#define AI_CHANNEL_MAX        4

/* Status phases for the progress callback */
#define AI_STATUS_THINKING   0  /* waiting for API response */
#define AI_STATUS_TOOL_CALL  1  /* executing a tool (detail = name) */
#define AI_STATUS_DONE       2  /* request complete */

typedef void (*ai_status_cb_t)(int status, const char *detail);

void ai_set_status_cb(ai_status_cb_t cb);

int  ai_engine_init(void);
void ai_engine_stop(void);

/**
 * Runtime configuration — update API credentials without recompiling.
 * New values take effect on the next ai_chat() call.
 * Caller is responsible for NVS persistence (platform-specific).
 */
void ai_set_api_key(const char *key);
void ai_set_api_url(const char *url);
void ai_set_model(const char *model);

const char *ai_get_api_key(void);
const char *ai_get_api_url(void);
const char *ai_get_model(void);

/**
 * Set channel hint appended to the system prompt.
 * Call before ai_chat() to tell the model which channel is active
 * (e.g. "Feishu IM", "serial console").  Pass NULL to clear.
 * The string is copied internally; caller may free after return.
 */
void ai_set_channel_hint(const char *hint);

/**
 * Set the active channel ID for conversation memory isolation.
 * Each channel maintains separate conversation history so messages
 * from different sources (shell, Feishu, Telegram) don't mix.
 * Use AI_CHANNEL_* constants from ai_memory.h.
 */
void ai_set_channel(int channel_id);
int  ai_get_channel(void);

/**
 * Send a user message to the LLM and receive a reply.
 * Stores user/assistant messages in conversation memory.
 *
 * Thread-safe: requests from multiple channels (shell, Feishu,
 * Telegram, scheduler) are serialized through an internal queue
 * and processed by a dedicated AI worker thread. If the queue is
 * full, returns immediately with "[AI is busy, please retry]".
 */
int ai_chat(const char *user_msg, char *reply, size_t reply_size);

/**
 * One-shot LLM call without conversation memory.
 * Used by skill system to avoid polluting main history.
 * Same queue-based concurrency as ai_chat().
 */
int ai_chat_raw(const char *prompt, char *reply, size_t reply_size);

/**
 * Lightweight API connectivity check.
 * Sends a minimal request (max_tokens=1) without acquiring the API
 * lock, so it never blocks interactive ai_chat() calls.
 * Returns CLAW_OK if the API responds (any HTTP status > 0),
 * CLAW_ERROR on network failure or missing credentials.
 */
int ai_ping(void);

#endif /* CLAW_SERVICES_AI_ENGINE_H */
