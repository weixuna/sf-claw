/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Tool class — OOP base class for LLM-callable tools.
 */

#ifndef CLAW_CORE_CLAW_TOOL_H
#define CLAW_CORE_CLAW_TOOL_H

#include "claw/core/claw_errno.h"
#include "claw/core/claw_class.h"
#include <stdint.h>

struct cJSON;

/* ------------------------------------------------------------------ */
/* Tool ops vtable                                                    */
/* ------------------------------------------------------------------ */

struct claw_tool;

struct claw_tool_ops {
    claw_err_t (*execute)(struct claw_tool *tool,
                          const struct cJSON *params,
                          struct cJSON *result);
    claw_err_t (*validate_params)(struct claw_tool *tool,
                                  const struct cJSON *params);
    claw_err_t (*init)(struct claw_tool *tool);     /* NULL = no-op */
    void       (*cleanup)(struct claw_tool *tool);  /* NULL = no-op */
};

/* ------------------------------------------------------------------ */
/* Tool base class                                                    */
/* ------------------------------------------------------------------ */

struct claw_tool {
    const char                *name;
    const char                *description;
    const char                *input_schema_json;
    const struct claw_tool_ops *ops;
    uint8_t                    required_caps;
    uint8_t                    flags;
    claw_list_node_t           node;
};

/* Tool flags */
#define CLAW_TOOL_LOCAL_ONLY  (1 << 0)

/* ------------------------------------------------------------------ */
/* Tool core API                                                      */
/* ------------------------------------------------------------------ */

claw_err_t claw_tool_core_register(struct claw_tool *tool);
claw_err_t claw_tool_core_collect_from_section(void);

struct claw_tool *claw_tool_core_find(const char *name);
int               claw_tool_core_count(void);
claw_list_node_t *claw_tool_core_list(void);

/**
 * Invoke a tool: validate_params (if present) then execute.
 * Returns CLAW_ERR_INVALID if validation fails, skipping execute.
 */
claw_err_t claw_tool_invoke(struct claw_tool *tool,
                             const struct cJSON *params,
                             struct cJSON *result);

/**
 * Call ops->init() on all registered tools. Stops on first failure.
 */
claw_err_t claw_tool_core_init_all(void);

/**
 * Call ops->cleanup() on all registered tools (reverse order).
 */
void claw_tool_core_cleanup_all(void);

#endif /* CLAW_CORE_CLAW_TOOL_H */
