/*
 * AI engine header shim: add UI and WeChat channels without touching
 * upstream.
 *
 * The upstream AI_CHANNEL_MAX is 4 (SHELL/FEISHU/TELEGRAM/SCHED).  The
 * watch needs a separate channel so the chat UI and serial console do
 * not share conversation memory, plus another for WeChat ClawBot.
 */

#ifndef CLAW_PORT_AI_ENGINE_H
#define CLAW_PORT_AI_ENGINE_H

#include_next <claw/services/ai/ai_engine.h>

#undef AI_CHANNEL_MAX
#define AI_CHANNEL_MAX    6
#define AI_CHANNEL_UI     4
#define AI_CHANNEL_WECHAT 5

#endif /* CLAW_PORT_AI_ENGINE_H */
