/*
 * SPDX-License-Identifier: MIT
 * Unit tests for tool OOP registry (claw_tool_core + claw_tools wrapper).
 */

#include "framework/test.h"
#include "claw/tools/claw_tools.h"
#include "cJSON.h"

#include <string.h>

/* ---- Helpers ---- */

static claw_err_t dummy_exec(struct claw_tool *tool,
                              const cJSON *params, cJSON *result)
{
    (void)tool;
    (void)params;
    cJSON_AddStringToObject(result, "status", "ok");
    return CLAW_OK;
}

static const struct claw_tool_ops dummy_ops = {
    .execute = dummy_exec,
};

/* Validator that rejects missing "required_key" */
static claw_err_t strict_validate(struct claw_tool *tool,
                                   const cJSON *params)
{
    (void)tool;
    if (!cJSON_GetObjectItem(params, "required_key")) {
        return CLAW_ERR_INVALID;
    }
    return CLAW_OK;
}

static claw_err_t strict_exec(struct claw_tool *tool,
                               const cJSON *params, cJSON *result)
{
    (void)tool;
    (void)params;
    cJSON_AddStringToObject(result, "status", "executed");
    return CLAW_OK;
}

static const struct claw_tool_ops strict_ops = {
    .execute = strict_exec,
    .validate_params = strict_validate,
};

/* Init/cleanup tracking */
static int s_init_called;
static int s_cleanup_called;

static claw_err_t tracking_init(struct claw_tool *tool)
{
    (void)tool;
    s_init_called++;
    return CLAW_OK;
}

static void tracking_cleanup(struct claw_tool *tool)
{
    (void)tool;
    s_cleanup_called++;
}

static const struct claw_tool_ops lifecycle_ops = {
    .execute = dummy_exec,
    .init = tracking_init,
    .cleanup = tracking_cleanup,
};

static int s_builtin_count;

/* ---- Tests ---- */

static void test_tools_init(void)
{
    TEST_ASSERT_EQ(claw_tools_init(), CLAW_OK);
    s_builtin_count = claw_tools_count();
    TEST_ASSERT(s_builtin_count >= 0);
}

static void test_tool_register(void)
{
    int base = claw_tools_count();

    static struct claw_tool test_tool = {
        .name = "test_tool",
        .description = "A test tool",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .ops = &dummy_ops,
    };
    TEST_ASSERT_EQ(claw_tool_core_register(&test_tool), CLAW_OK);
    TEST_ASSERT_EQ(claw_tools_count(), base + 1);
}

static void test_tool_find(void)
{
    static struct claw_tool alpha = {
        .name = "alpha",
        .description = "first",
        .input_schema_json = "{}",
        .ops = &dummy_ops,
    };
    static struct claw_tool beta = {
        .name = "beta",
        .description = "second",
        .input_schema_json = "{}",
        .ops = &dummy_ops,
    };
    claw_tool_core_register(&alpha);
    claw_tool_core_register(&beta);

    const struct claw_tool *t = claw_tool_find("beta");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_STR_EQ(t->name, "beta");

    TEST_ASSERT_NULL(claw_tool_find("gamma"));
}

static void test_tools_to_json(void)
{
    int base = claw_tools_count();
    static struct claw_tool ping = {
        .name = "ping",
        .description = "ping tool",
        .input_schema_json = "{}",
        .ops = &dummy_ops,
    };
    static struct claw_tool pong = {
        .name = "pong",
        .description = "pong tool",
        .input_schema_json = "{}",
        .ops = &dummy_ops,
    };
    claw_tool_core_register(&ping);
    claw_tool_core_register(&pong);

    cJSON *arr = claw_tools_to_json();
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQ(cJSON_GetArraySize(arr), base + 2);

    cJSON_Delete(arr);
}

static void test_tools_to_json_exclude(void)
{
    int base = claw_tools_count();
    static struct claw_tool lcd_draw = {
        .name = "lcd_draw_test",
        .description = "draw",
        .input_schema_json = "{}",
        .ops = &dummy_ops,
    };
    static struct claw_tool lcd_clear = {
        .name = "lcd_clear_test",
        .description = "clear",
        .input_schema_json = "{}",
        .ops = &dummy_ops,
    };
    static struct claw_tool http_tool = {
        .name = "http_get_test",
        .description = "get",
        .input_schema_json = "{}",
        .ops = &dummy_ops,
    };
    claw_tool_core_register(&lcd_draw);
    claw_tool_core_register(&lcd_clear);
    claw_tool_core_register(&http_tool);

    cJSON *arr = claw_tools_to_json_exclude("lcd_");
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQ(cJSON_GetArraySize(arr), base + 1);

    int lcd_count = 0;
    for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        const char *n = cJSON_GetObjectItem(item, "name")->valuestring;
        if (strncmp(n, "lcd_", 4) == 0) {
            lcd_count++;
        }
    }
    TEST_ASSERT_EQ(lcd_count, 0);

    cJSON_Delete(arr);
}

static void test_tool_ops_validate(void)
{
    /* NULL ops → reject */
    static struct claw_tool bad_tool = {
        .name = "bad_tool",
        .description = "no ops",
        .input_schema_json = "{}",
        .ops = NULL,
    };
    TEST_ASSERT(claw_tool_core_register(&bad_tool) != CLAW_OK);

    /* ops->execute == NULL → reject */
    static const struct claw_tool_ops no_exec_ops = {
        .execute = NULL,
    };
    static struct claw_tool no_exec_tool = {
        .name = "no_exec",
        .description = "no execute",
        .input_schema_json = "{}",
        .ops = &no_exec_ops,
    };
    TEST_ASSERT(claw_tool_core_register(&no_exec_tool) != CLAW_OK);
}

static void test_tool_invoke_validate_params(void)
{
    static struct claw_tool validated_tool = {
        .name = "validated_tool",
        .description = "requires required_key",
        .input_schema_json = "{}",
        .ops = &strict_ops,
    };
    claw_tool_core_register(&validated_tool);

    struct claw_tool *t = claw_tool_core_find("validated_tool");
    TEST_ASSERT_NOT_NULL(t);

    /* Missing required_key → CLAW_ERR_INVALID, execute skipped */
    cJSON *bad_params = cJSON_CreateObject();
    cJSON *bad_result = cJSON_CreateObject();
    TEST_ASSERT_EQ(claw_tool_invoke(t, bad_params, bad_result),
                   CLAW_ERR_INVALID);
    /* Result should NOT contain "executed" (execute was skipped) */
    TEST_ASSERT_NULL(cJSON_GetObjectItem(bad_result, "status"));
    cJSON_Delete(bad_params);
    cJSON_Delete(bad_result);

    /* With required_key → CLAW_OK, execute runs */
    cJSON *good_params = cJSON_CreateObject();
    cJSON_AddStringToObject(good_params, "required_key", "present");
    cJSON *good_result = cJSON_CreateObject();
    TEST_ASSERT_EQ(claw_tool_invoke(t, good_params, good_result),
                   CLAW_OK);
    TEST_ASSERT_STR_EQ(
        cJSON_GetObjectItem(good_result, "status")->valuestring,
        "executed");
    cJSON_Delete(good_params);
    cJSON_Delete(good_result);
}

/* Self-pointer capture for test_tool_invoke_self_pointer */
static struct claw_tool *s_received_self;

static claw_err_t capture_self_exec(struct claw_tool *tool,
                                     const cJSON *params, cJSON *result)
{
    (void)params;
    (void)result;
    s_received_self = tool;
    return CLAW_OK;
}

static const struct claw_tool_ops capture_ops = {
    .execute = capture_self_exec,
};

static void test_tool_invoke_self_pointer(void)
{
    static struct claw_tool self_tool = {
        .name = "self_test",
        .description = "captures self",
        .input_schema_json = "{}",
        .ops = &capture_ops,
    };
    claw_tool_core_register(&self_tool);

    s_received_self = NULL;
    cJSON *p = cJSON_CreateObject();
    cJSON *r = cJSON_CreateObject();
    claw_tool_invoke(&self_tool, p, r);
    TEST_ASSERT(s_received_self == &self_tool);
    cJSON_Delete(p);
    cJSON_Delete(r);
}

static void test_tool_init_cleanup_lifecycle(void)
{
    s_init_called = 0;
    s_cleanup_called = 0;

    static struct claw_tool lc_tool = {
        .name = "lifecycle_test",
        .description = "tracks init/cleanup",
        .input_schema_json = "{}",
        .ops = &lifecycle_ops,
    };
    claw_tool_core_register(&lc_tool);

    /* init_all should call init on our tool */
    int pre_init = s_init_called;
    claw_tool_core_init_all();
    TEST_ASSERT(s_init_called > pre_init);

    /* cleanup_all should call cleanup on our tool */
    int pre_cleanup = s_cleanup_called;
    claw_tool_core_cleanup_all();
    TEST_ASSERT(s_cleanup_called > pre_cleanup);
}

/* Track cleanup order for reverse-order test */
static int s_cleanup_order[4];
static int s_cleanup_idx;

static void order_cleanup_a(struct claw_tool *tool)
{
    (void)tool;
    s_cleanup_order[s_cleanup_idx++] = 1;
}

static void order_cleanup_b(struct claw_tool *tool)
{
    (void)tool;
    s_cleanup_order[s_cleanup_idx++] = 2;
}

static const struct claw_tool_ops order_ops_a = {
    .execute = dummy_exec,
    .cleanup = order_cleanup_a,
};

static const struct claw_tool_ops order_ops_b = {
    .execute = dummy_exec,
    .cleanup = order_cleanup_b,
};

static void test_cleanup_reverse_order(void)
{
    static struct claw_tool oa = {
        .name = "order_a",
        .description = "first registered",
        .input_schema_json = "{}",
        .ops = &order_ops_a,
    };
    static struct claw_tool ob = {
        .name = "order_b",
        .description = "second registered",
        .input_schema_json = "{}",
        .ops = &order_ops_b,
    };
    claw_tool_core_register(&oa);
    claw_tool_core_register(&ob);

    s_cleanup_idx = 0;
    claw_tool_core_cleanup_all();

    /* b (last registered) should be cleaned up first */
    TEST_ASSERT_EQ(s_cleanup_order[0], 2);
    TEST_ASSERT_EQ(s_cleanup_order[1], 1);

    /* Remove test tools to avoid polluting later suites */
    claw_list_del(&oa.node);
    claw_list_del(&ob.node);
}

/*
 * Init failure unwind test:
 * - sentinel: has cleanup only (no init) — must NOT be cleaned up
 * - tool A: has init (succeeds) + cleanup — MUST be cleaned up
 * - tool B: has init (fails) — triggers unwind
 */
static int s_unwind_a_cleaned;
static int s_sentinel_cleaned;

static claw_err_t unwind_init_a(struct claw_tool *tool)
{
    (void)tool;
    return CLAW_OK;
}

static void unwind_cleanup_a(struct claw_tool *tool)
{
    (void)tool;
    s_unwind_a_cleaned++;
}

static void sentinel_cleanup(struct claw_tool *tool)
{
    (void)tool;
    s_sentinel_cleaned++;
}

static claw_err_t unwind_init_b_fail(struct claw_tool *tool)
{
    (void)tool;
    return CLAW_ERR_NOMEM;
}

static const struct claw_tool_ops sentinel_ops = {
    .execute = dummy_exec,
    .cleanup = sentinel_cleanup,
    /* no .init — this tool was never initialized */
};

static const struct claw_tool_ops unwind_ops_a = {
    .execute = dummy_exec,
    .init = unwind_init_a,
    .cleanup = unwind_cleanup_a,
};

static const struct claw_tool_ops unwind_ops_b_fail = {
    .execute = dummy_exec,
    .init = unwind_init_b_fail,
};

static void test_init_failure_unwind(void)
{
    static struct claw_tool sentinel = {
        .name = "sentinel",
        .description = "cleanup-only, no init",
        .input_schema_json = "{}",
        .ops = &sentinel_ops,
    };
    static struct claw_tool ua = {
        .name = "unwind_a",
        .description = "succeeds init",
        .input_schema_json = "{}",
        .ops = &unwind_ops_a,
    };
    static struct claw_tool ub = {
        .name = "unwind_b",
        .description = "fails init",
        .input_schema_json = "{}",
        .ops = &unwind_ops_b_fail,
    };
    claw_tool_core_register(&sentinel);
    claw_tool_core_register(&ua);
    claw_tool_core_register(&ub);

    s_unwind_a_cleaned = 0;
    s_sentinel_cleaned = 0;
    claw_err_t err = claw_tool_core_init_all();
    TEST_ASSERT(err != CLAW_OK);

    /* A was successfully inited → its cleanup must be called */
    TEST_ASSERT(s_unwind_a_cleaned > 0);

    /* Sentinel has cleanup but no init → must NOT be cleaned */
    TEST_ASSERT_EQ(s_sentinel_cleaned, 0);

    /* Remove test tools to avoid polluting later suites */
    claw_list_del(&sentinel.node);
    claw_list_del(&ua.node);
    claw_list_del(&ub.node);
}

int test_tools_suite(void)
{
    printf("=== test_tools ===\n");
    TEST_BEGIN();

    RUN_TEST(test_tools_init);
    RUN_TEST(test_tool_register);
    RUN_TEST(test_tool_find);
    RUN_TEST(test_tools_to_json);
    RUN_TEST(test_tools_to_json_exclude);
    RUN_TEST(test_tool_ops_validate);
    RUN_TEST(test_tool_invoke_validate_params);
    RUN_TEST(test_tool_invoke_self_pointer);
    RUN_TEST(test_tool_init_cleanup_lifecycle);
    RUN_TEST(test_cleanup_reverse_order);
    RUN_TEST(test_init_failure_unwind);

    TEST_END();
}
