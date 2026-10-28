/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * AI skill — predefined + user-created prompt templates.
 * User skills persist across reboots via NVS (ESP-IDF).
 */

#ifndef CLAW_AI_SKILL_H
#define CLAW_AI_SKILL_H

#include "osal/claw_os.h"

#define SKILL_MAX           8
#define SKILL_NAME_MAX      24
#define SKILL_DESC_MAX      64
#define SKILL_PROMPT_MAX    256

int  ai_skill_init(void);
int  ai_skill_register(const char *name, const char *desc,
                       const char *prompt_template);
int  ai_skill_remove(const char *name);
int  ai_skill_execute(const char *name, const char *params,
                      char *reply, size_t reply_size);
const char *ai_skill_find(const char *name);
void ai_skill_list(void);
int  ai_skill_count(void);
const char *ai_skill_get_name(int index);

/**
 * Try to execute a /command as a dynamic skill.
 * Strips '/' prefix, looks up skill, joins remaining argv as params.
 * Returns CLAW_OK if skill found and executed, CLAW_ERROR otherwise.
 */
int  ai_skill_try_command(const char *cmd_name, int argc, char **argv,
                          char *reply, size_t reply_size);

/**
 * Write formatted skill list to buffer (for cross-channel use).
 * Returns number of bytes written (excluding NUL).
 */
int  ai_skill_list_to_buf(char *buf, size_t size);

/**
 * Build a summary of all skills for system prompt injection.
 * Returns heap-allocated string, caller frees. NULL if no skills.
 */
char *ai_skill_build_summary(void);

#endif /* CLAW_AI_SKILL_H */
