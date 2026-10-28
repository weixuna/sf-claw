/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Tool core — registration and linker section collection.
 */

#include "claw/core/claw_tool.h"
#include "osal/claw_os.h"

#include <string.h>

#define TAG "tool_core"

static CLAW_LIST_HEAD(s_tools);

claw_err_t claw_tool_core_register(struct claw_tool *tool)
{
    if (!tool || !tool->name) {
        return CLAW_ERR_INVALID;
    }

    CLAW_OPS_VALIDATE(tool->ops, execute);

    claw_list_init(&tool->node);
    claw_list_add_tail(&tool->node, &s_tools);

    CLAW_LOGD(TAG, "registered: %s", tool->name);
    return CLAW_OK;
}

claw_err_t claw_tool_core_collect_from_section(void)
{
    static int s_collected;
    const struct claw_tool **p;

    if (s_collected) {
        return CLAW_OK;
    }

    if (!__start_claw_tools || !__stop_claw_tools) {
        return CLAW_OK;
    }

    s_collected = 1;

    claw_for_each_registered(p, __start_claw_tools,
                             __stop_claw_tools) {
        claw_err_t err = claw_tool_core_register(
            (struct claw_tool *)*p);
        if (err != CLAW_OK) {
            CLAW_LOGE(TAG, "failed to register %s: %s",
                      (*p)->name, claw_strerror(err));
        }
    }

    return CLAW_OK;
}

struct claw_tool *claw_tool_core_find(const char *name)
{
    claw_list_node_t *pos;
    struct claw_tool *t;

    if (!name) {
        return NULL;
    }

    claw_list_for_each(pos, &s_tools) {
        t = claw_list_entry(pos, struct claw_tool, node);
        if (strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

int claw_tool_core_count(void)
{
    int count = 0;
    claw_list_node_t *pos;

    claw_list_for_each(pos, &s_tools) {
        count++;
    }
    return count;
}

claw_list_node_t *claw_tool_core_list(void)
{
    return &s_tools;
}

claw_err_t claw_tool_invoke(struct claw_tool *tool,
                             const struct cJSON *params,
                             struct cJSON *result)
{
    if (!tool || !tool->ops || !tool->ops->execute) {
        return CLAW_ERR_INVALID;
    }

    if (tool->ops->validate_params) {
        claw_err_t err = tool->ops->validate_params(tool, params);
        if (err != CLAW_OK) {
            return err;
        }
    }

    return tool->ops->execute(tool, params, result);
}

claw_err_t claw_tool_core_init_all(void)
{
    claw_list_node_t *pos;

    claw_list_for_each(pos, &s_tools) {
        struct claw_tool *t =
            claw_list_entry(pos, struct claw_tool, node);
        if (t->ops && t->ops->init) {
            claw_err_t err = t->ops->init(t);
            if (err != CLAW_OK) {
                CLAW_LOGE(TAG, "%s init failed: %s",
                          t->name, claw_strerror(err));
                /*
                 * Unwind: walk backward and cleanup only tools
                 * whose init() was called in this pass (i.e.,
                 * those that have ops->init). Tools with only
                 * cleanup but no init were never initialized
                 * and must not be cleaned up here.
                 */
                claw_list_node_t *rpos;
                for (rpos = pos->prev; rpos != &s_tools;
                     rpos = rpos->prev) {
                    struct claw_tool *rt =
                        claw_list_entry(rpos, struct claw_tool, node);
                    if (rt->ops && rt->ops->init &&
                        rt->ops->cleanup) {
                        rt->ops->cleanup(rt);
                    }
                }
                return err;
            }
        }
    }
    return CLAW_OK;
}

void claw_tool_core_cleanup_all(void)
{
    claw_list_node_t *pos;

    claw_list_for_each_reverse(pos, &s_tools) {
        struct claw_tool *t =
            claw_list_entry(pos, struct claw_tool, node);
        if (t->ops && t->ops->cleanup) {
            t->ops->cleanup(t);
        }
    }
}
