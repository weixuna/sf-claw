/*
 * SPDX-License-Identifier: MIT
 * Unit tests for OTA service logic (version compare, JSON parse).
 *
 * Platform OTA functions (flash ops) are stubbed — only pure logic
 * is tested here.
 */

#include "framework/test.h"
#include "osal/claw_os.h"

#ifdef CONFIG_RTCLAW_OTA_ENABLE

#include "claw_ota.h"
#include "claw/services/ota/ota_service.h"
#include "claw/core/claw_tool.h"
#include "claw/tools/claw_tools.h"
#include "claw/tools/claw_tools.h"

#include <string.h>

/* ---- Platform stubs (no real flash on test platform) ---- */

static const char *s_stub_version = "0.1.0";

int claw_ota_platform_init(void)
{
    return CLAW_OK;
}

int claw_ota_do_update(const char *url, claw_ota_progress_cb progress)
{
    (void)url;
    (void)progress;
    return CLAW_OK;
}

int claw_ota_rollback(void)
{
    return CLAW_OK;
}

int claw_ota_mark_valid(void)
{
    return CLAW_OK;
}

const char *claw_ota_running_version(void)
{
    return s_stub_version;
}

/* ---- version_compare tests ---- */

static void test_version_compare_equal(void)
{
    TEST_ASSERT_EQ(ota_version_compare("1.0.0", "1.0.0"), 0);
    TEST_ASSERT_EQ(ota_version_compare("0.1.0", "0.1.0"), 0);
    TEST_ASSERT_EQ(ota_version_compare("10.20.30", "10.20.30"), 0);
}

static void test_version_compare_major(void)
{
    TEST_ASSERT(ota_version_compare("2.0.0", "1.0.0") > 0);
    TEST_ASSERT(ota_version_compare("1.0.0", "2.0.0") < 0);
    TEST_ASSERT(ota_version_compare("10.0.0", "9.9.9") > 0);
}

static void test_version_compare_minor(void)
{
    TEST_ASSERT(ota_version_compare("1.2.0", "1.1.0") > 0);
    TEST_ASSERT(ota_version_compare("1.1.0", "1.2.0") < 0);
    TEST_ASSERT(ota_version_compare("0.10.0", "0.9.0") > 0);
}

static void test_version_compare_patch(void)
{
    TEST_ASSERT(ota_version_compare("1.0.2", "1.0.1") > 0);
    TEST_ASSERT(ota_version_compare("1.0.1", "1.0.2") < 0);
    TEST_ASSERT(ota_version_compare("0.0.10", "0.0.9") > 0);
}

static void test_version_compare_mixed(void)
{
    /* Major takes precedence over minor/patch */
    TEST_ASSERT(ota_version_compare("2.0.0", "1.9.9") > 0);
    /* Minor takes precedence over patch */
    TEST_ASSERT(ota_version_compare("1.2.0", "1.1.9") > 0);
}

/* ---- parse_version_json tests ---- */

static void test_parse_json_full(void)
{
    const char *json =
        "{\"version\":\"0.2.0\","
        "\"url\":\"https://example.com/fw.bin\","
        "\"size\":123456,"
        "\"sha256\":\"abcdef0123456789\"}";

    claw_ota_info_t info;
    TEST_ASSERT_EQ(ota_parse_version_json(json, &info), CLAW_OK);
    TEST_ASSERT_STR_EQ(info.version, "0.2.0");
    TEST_ASSERT_STR_EQ(info.url, "https://example.com/fw.bin");
    TEST_ASSERT_EQ(info.size, 123456);
    TEST_ASSERT_STR_EQ(info.sha256, "abcdef0123456789");
}

static void test_parse_json_minimal(void)
{
    /* Only version and url required */
    const char *json =
        "{\"version\":\"1.0.0\","
        "\"url\":\"http://ota.local/fw.bin\"}";

    claw_ota_info_t info;
    TEST_ASSERT_EQ(ota_parse_version_json(json, &info), CLAW_OK);
    TEST_ASSERT_STR_EQ(info.version, "1.0.0");
    TEST_ASSERT_STR_EQ(info.url, "http://ota.local/fw.bin");
    TEST_ASSERT_EQ(info.size, 0);
    TEST_ASSERT_STR_EQ(info.sha256, "");
}

static void test_parse_json_with_spaces(void)
{
    const char *json =
        "{ \"version\" : \"0.3.1\" , "
        "  \"url\" : \"http://host/fw.bin\" , "
        "  \"size\" : 99999 }";

    claw_ota_info_t info;
    TEST_ASSERT_EQ(ota_parse_version_json(json, &info), CLAW_OK);
    TEST_ASSERT_STR_EQ(info.version, "0.3.1");
    TEST_ASSERT_STR_EQ(info.url, "http://host/fw.bin");
    TEST_ASSERT_EQ(info.size, 99999);
}

static void test_parse_json_missing_version(void)
{
    const char *json =
        "{\"url\":\"http://example.com/fw.bin\","
        "\"size\":100}";

    claw_ota_info_t info;
    TEST_ASSERT(ota_parse_version_json(json, &info) != CLAW_OK);
}

static void test_parse_json_missing_url(void)
{
    const char *json = "{\"version\":\"1.0.0\",\"size\":100}";

    claw_ota_info_t info;
    TEST_ASSERT(ota_parse_version_json(json, &info) != CLAW_OK);
}

static void test_parse_json_empty(void)
{
    claw_ota_info_t info;
    TEST_ASSERT(ota_parse_version_json("", &info) != CLAW_OK);
    TEST_ASSERT(ota_parse_version_json("{}", &info) != CLAW_OK);
}

static void test_parse_json_long_version(void)
{
    /* Version field should be truncated, not overflow */
    const char *json =
        "{\"version\":\"1.2.3.4.5.6.7.8.9.0.very.long\","
        "\"url\":\"http://x/fw.bin\"}";

    claw_ota_info_t info;
    TEST_ASSERT_EQ(ota_parse_version_json(json, &info), CLAW_OK);
    TEST_ASSERT(strlen(info.version) < sizeof(info.version));
    TEST_ASSERT(strlen(info.url) > 0);
}

/* ---- trigger_update input validation ---- */

static void test_trigger_null_url(void)
{
    TEST_ASSERT(ota_trigger_update(NULL) != CLAW_OK);
}

static void test_trigger_empty_url(void)
{
    TEST_ASSERT(ota_trigger_update("") != CLAW_OK);
}

/* ---- OTA tool registration ---- */

static void test_ota_tools_registered(void)
{
    /* Collect tools from linker sections (mirrors claw_init path) */
    claw_tool_core_collect_from_section();
    TEST_ASSERT_EQ(claw_tools_init(), CLAW_OK);

    TEST_ASSERT_NOT_NULL(claw_tool_find("ota_check"));
    TEST_ASSERT_NOT_NULL(claw_tool_find("ota_update"));
    TEST_ASSERT_NOT_NULL(claw_tool_find("ota_version"));
    TEST_ASSERT_NOT_NULL(claw_tool_find("ota_rollback"));
}

/* ---- Suite ---- */

int test_ota_suite(void)
{
    printf("=== test_ota ===\n");
    TEST_BEGIN();

    /* version compare */
    RUN_TEST(test_version_compare_equal);
    RUN_TEST(test_version_compare_major);
    RUN_TEST(test_version_compare_minor);
    RUN_TEST(test_version_compare_patch);
    RUN_TEST(test_version_compare_mixed);

    /* JSON parse */
    RUN_TEST(test_parse_json_full);
    RUN_TEST(test_parse_json_minimal);
    RUN_TEST(test_parse_json_with_spaces);
    RUN_TEST(test_parse_json_missing_version);
    RUN_TEST(test_parse_json_missing_url);
    RUN_TEST(test_parse_json_empty);
    RUN_TEST(test_parse_json_long_version);

    /* trigger validation */
    RUN_TEST(test_trigger_null_url);
    RUN_TEST(test_trigger_empty_url);

    /* tool registration */
    RUN_TEST(test_ota_tools_registered);

    TEST_END();
}

#else /* !CONFIG_RTCLAW_OTA_ENABLE */

int test_ota_suite(void)
{
    printf("=== test_ota (skipped — OTA disabled) ===\n");
    return 0;
}

#endif /* CONFIG_RTCLAW_OTA_ENABLE */
