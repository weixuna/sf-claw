/*
 * SF32 / RT-Thread configuration shim for rt-claw 0.2.0.
 *
 * This file is found before the upstream third_party/rt-claw/claw_config.h
 * in the include path.  It keeps the upstream tree pristine while giving
 * the SF32 port its own defaults and Kconfig-overridable values.
 */

#ifndef CLAW_CONFIG_H
#define CLAW_CONFIG_H

#include <rtconfig.h>

#ifndef RT_CLAW_VERSION
#define RT_CLAW_VERSION         "0.2.0"
#endif

/* ---- Gateway ---- */
#define CLAW_GW_MSG_POOL_SIZE   16
#define CLAW_GW_MSG_MAX_LEN     256
#ifndef CLAW_GW_THREAD_STACK
#define CLAW_GW_THREAD_STACK    4096
#endif
#ifndef CLAW_GW_THREAD_PRIO
#define CLAW_GW_THREAD_PRIO     21   /* RT-Thread: below GUI (19) */
#endif

/* ---- Swarm (disabled on the watch) ---- */
#define CLAW_SWARM_MAX_NODES        32
#define CLAW_SWARM_HEARTBEAT_MS     5000
#define CLAW_SWARM_TIMEOUT_MS       15000
#define CLAW_SWARM_PORT             5300
#define CLAW_SWARM_THREAD_STACK     4096
#define CLAW_SWARM_THREAD_PRIO      12

/* ---- Scheduler (disabled on the watch) ---- */
#define CLAW_SCHED_MAX_TASKS        8
#define CLAW_SCHED_TICK_MS          1000
#define CLAW_SCHED_THREAD_STACK     8192
#define CLAW_SCHED_THREAD_PRIO      10

/* ---- Heartbeat (disabled on the watch) ---- */
#define CLAW_HEARTBEAT_INTERVAL_MS  300000
#define CLAW_HEARTBEAT_MAX_EVENTS   8
#define CLAW_HEARTBEAT_MSG_MAX      128
#define CLAW_HEARTBEAT_PROMPT_MAX   1024
#define CLAW_HEARTBEAT_REPLY_MAX    512
#define CLAW_HEARTBEAT_THREAD_STACK 8192

/* ---- AI Engine (worker thread) ---- */
#define CLAW_AI_QUEUE_DEPTH         4
#ifndef CLAW_AI_WORKER_STACK
#define CLAW_AI_WORKER_STACK        8192
#endif
#ifndef CLAW_AI_WORKER_PRIO
#define CLAW_AI_WORKER_PRIO         23   /* below GUI and ui_ai */
#endif

/* ---- AI Engine (API) ---- */
#ifndef CONFIG_RTCLAW_AI_API_KEY
#define CONFIG_RTCLAW_AI_API_KEY      ""
#endif
#ifndef CONFIG_RTCLAW_AI_API_URL
#define CONFIG_RTCLAW_AI_API_URL      "https://api.deepseek.com/chat/completions"
#endif
#ifndef CONFIG_RTCLAW_AI_MODEL
#define CONFIG_RTCLAW_AI_MODEL        "deepseek-chat"
#endif
#ifndef CONFIG_RTCLAW_AI_MAX_TOKENS
/* HotUI 生成的 JavaScript 会作为 tool_calls 参数返回，需要更大的输出预算。 */
#define CONFIG_RTCLAW_AI_MAX_TOKENS   8192
#endif
#ifndef CONFIG_RTCLAW_AI_CONTEXT_SIZE
#ifdef RTCLAW_AI_CONTEXT_SIZE
#define CONFIG_RTCLAW_AI_CONTEXT_SIZE RTCLAW_AI_CONTEXT_SIZE
#else
#define CONFIG_RTCLAW_AI_CONTEXT_SIZE 8192
#endif
#endif
#ifndef CONFIG_RTCLAW_AI_MEMORY_MAX_MSGS
#ifdef RTCLAW_AI_MEMORY_MAX_MSGS
#define CONFIG_RTCLAW_AI_MEMORY_MAX_MSGS RTCLAW_AI_MEMORY_MAX_MSGS
#else
#define CONFIG_RTCLAW_AI_MEMORY_MAX_MSGS 12
#endif
#endif

/* Feature flags from Kconfig are named without CONFIG_ in rtconfig.h. */
#ifdef RTCLAW_TOOL_SYSTEM
#ifndef CONFIG_RTCLAW_TOOL_SYSTEM
#define CONFIG_RTCLAW_TOOL_SYSTEM 1
#endif
#endif

/* ---- Feishu / Telegram (not built) ---- */
#ifndef CONFIG_RTCLAW_FEISHU_APP_ID
#define CONFIG_RTCLAW_FEISHU_APP_ID   ""
#endif
#ifndef CONFIG_RTCLAW_FEISHU_APP_SECRET
#define CONFIG_RTCLAW_FEISHU_APP_SECRET ""
#endif
#ifndef CONFIG_RTCLAW_TELEGRAM_BOT_TOKEN
#define CONFIG_RTCLAW_TELEGRAM_BOT_TOKEN ""
#endif
#ifndef CONFIG_RTCLAW_TELEGRAM_API_URL
#define CONFIG_RTCLAW_TELEGRAM_API_URL   "https://api.telegram.org"
#endif

/* ---- OTA (not built; use the SiFli DFU flow) ---- */
#define CLAW_OTA_THREAD_STACK       8192
#define CLAW_OTA_THREAD_PRIO        10
#ifndef CONFIG_RTCLAW_OTA_URL
#define CONFIG_RTCLAW_OTA_URL       ""
#endif
#ifndef CONFIG_RTCLAW_OTA_CHECK_INTERVAL_MS
#define CONFIG_RTCLAW_OTA_CHECK_INTERVAL_MS  0
#endif

#endif /* CLAW_CONFIG_H */
