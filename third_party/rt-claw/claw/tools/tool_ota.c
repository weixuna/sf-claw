/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * OTA tools — AI can check, trigger, and manage firmware updates.
 */

#include "claw/tools/claw_tools.h"
#include "claw_ota.h"
#include "claw/services/ota/ota_service.h"

#include <stdio.h>

#ifdef CONFIG_RTCLAW_OTA_ENABLE

static claw_err_t tool_ota_check(struct claw_tool *tool,
                                 const cJSON *params, cJSON *result)
{
    (void)tool;
    (void)params;

    if (!claw_ota_supported()) {
        cJSON_AddStringToObject(result, "error",
            "OTA not supported on this platform");
        return CLAW_OK;
    }

    claw_ota_info_t info;
    int ret = ota_check_update(&info);

    if (ret == 1) {
        cJSON_AddStringToObject(result, "status", "update_available");
        cJSON_AddStringToObject(result, "current_version",
                                claw_ota_running_version());
        cJSON_AddStringToObject(result, "new_version", info.version);
        cJSON_AddStringToObject(result, "url", info.url);
        cJSON_AddNumberToObject(result, "size_bytes", info.size);
    } else if (ret == 0) {
        cJSON_AddStringToObject(result, "status", "up_to_date");
        cJSON_AddStringToObject(result, "version",
                                claw_ota_running_version());
    } else {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error",
                                "version check failed");
        return CLAW_ERROR;
    }
    return CLAW_OK;
}

static claw_err_t tool_ota_update(struct claw_tool *tool,
                                  const cJSON *params, cJSON *result)
{
    (void)tool;
    if (!claw_ota_supported()) {
        cJSON_AddStringToObject(result, "error",
            "OTA not supported on this platform");
        return CLAW_OK;
    }

    cJSON *url_j = cJSON_GetObjectItem(params, "url");

    if (url_j && cJSON_IsString(url_j) &&
        url_j->valuestring[0] != '\0') {
        /* Direct URL provided */
        if (ota_trigger_update(url_j->valuestring) == CLAW_OK) {
            cJSON_AddStringToObject(result, "status", "started");
            cJSON_AddStringToObject(result, "message",
                "OTA update started. Device will reboot "
                "automatically when done.");
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error",
                                    "failed to start OTA update");
            return CLAW_ERROR;
        }
        return CLAW_OK;
    }

    /* No URL: check server first */
    claw_ota_info_t info;
    int ret = ota_check_update(&info);

    if (ret == 1) {
        if (ota_trigger_update(info.url) == CLAW_OK) {
            cJSON_AddStringToObject(result, "status", "started");
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Updating from %s to %s. "
                     "Device will reboot when done.",
                     claw_ota_running_version(), info.version);
            cJSON_AddStringToObject(result, "message", msg);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error",
                                    "failed to start OTA update");
            return CLAW_ERROR;
        }
    } else if (ret == 0) {
        cJSON_AddStringToObject(result, "status", "up_to_date");
        cJSON_AddStringToObject(result, "version",
                                claw_ota_running_version());
    } else {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error",
                                "version check failed");
        return CLAW_ERROR;
    }
    return CLAW_OK;
}

static claw_err_t tool_ota_version(struct claw_tool *tool,
                                   const cJSON *params, cJSON *result)
{
    (void)tool;
    (void)params;

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "version",
                            claw_ota_running_version());
    return CLAW_OK;
}

static claw_err_t tool_ota_rollback(struct claw_tool *tool,
                                    const cJSON *params, cJSON *result)
{
    (void)tool;
    (void)params;

    if (!claw_ota_supported()) {
        cJSON_AddStringToObject(result, "error",
            "OTA not supported on this platform");
        return CLAW_OK;
    }

    if (claw_ota_rollback() != CLAW_OK) {
        cJSON_AddStringToObject(result, "error",
                                "rollback failed");
        return CLAW_OK;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "message",
        "Rolling back firmware and rebooting...");
    claw_thread_delay_ms(1000);
    return CLAW_OK;
}

static const char schema_empty[] =
    "{\"type\":\"object\",\"properties\":{}}";

static const char schema_ota_update[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"url\":{\"type\":\"string\","
    "\"description\":\"Firmware URL. If omitted, checks the "
    "configured OTA server for the latest version.\"}},"
    "\"required\":[]}";

/* ---- OOP tool registration ---- */

static const struct claw_tool_ops ota_check_ops = {
    .execute = tool_ota_check,
};
static struct claw_tool ota_check_tool = {
    .name = "ota_check",
    .description =
        "Check if a firmware update is available. Returns the "
        "current version and new version info if an update exists.",
    .input_schema_json = schema_empty,
    .ops = &ota_check_ops,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};
CLAW_TOOL_REGISTER(ota_check, &ota_check_tool);

static const struct claw_tool_ops ota_update_ops = {
    .execute = tool_ota_update,
};
static struct claw_tool ota_update_tool = {
    .name = "ota_update",
    .description =
        "Trigger an OTA firmware update. Optionally provide a "
        "direct firmware URL. The device will download, flash, "
        "and reboot automatically. Use with caution.",
    .input_schema_json = schema_ota_update,
    .ops = &ota_update_ops,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};
CLAW_TOOL_REGISTER(ota_update, &ota_update_tool);

static const struct claw_tool_ops ota_version_ops = {
    .execute = tool_ota_version,
};
static struct claw_tool ota_version_tool = {
    .name = "ota_version",
    .description =
        "Get the currently running firmware version.",
    .input_schema_json = schema_empty,
    .ops = &ota_version_ops,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};
CLAW_TOOL_REGISTER(ota_version, &ota_version_tool);

static const struct claw_tool_ops ota_rollback_ops = {
    .execute = tool_ota_rollback,
};
static struct claw_tool ota_rollback_tool = {
    .name = "ota_rollback",
    .description =
        "Roll back to the previous firmware version and reboot. "
        "Use this if the current firmware has issues after an OTA "
        "update. WARNING: device will reboot immediately.",
    .input_schema_json = schema_empty,
    .ops = &ota_rollback_ops,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};
CLAW_TOOL_REGISTER(ota_rollback, &ota_rollback_tool);

#endif /* CONFIG_RTCLAW_OTA_ENABLE */
