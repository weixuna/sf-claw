/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Mouse tools — expose USB HID mouse control as LLM-callable tools.
 */

#include "claw/tools/claw_tools.h"
#include "claw/services/swarm/swarm.h"

#include <stdio.h>
#include <string.h>

#define TAG "tool_mouse"

#ifdef CONFIG_RTCLAW_USB_HID_MOUSE

#include "drivers/input/espressif/usb_hid_mouse.h"

/* ---- mouse_move ---- */

static claw_err_t tool_mouse_move(struct claw_tool *tool,
                                  const cJSON *params, cJSON *result)
{
    (void)tool;
    cJSON *dx_j = cJSON_GetObjectItem(params, "dx");
    cJSON *dy_j = cJSON_GetObjectItem(params, "dy");

    if (!dx_j || !cJSON_IsNumber(dx_j) ||
        !dy_j || !cJSON_IsNumber(dy_j)) {
        cJSON_AddStringToObject(result, "error", "missing dx or dy");
        return CLAW_ERR_GENERIC;
    }

    int dx = dx_j->valueint;
    int dy = dy_j->valueint;

    if (dx < -127 || dx > 127 || dy < -127 || dy > 127) {
        cJSON_AddStringToObject(result, "error",
                                "dx/dy must be in range [-127, 127]");
        return CLAW_ERR_GENERIC;
    }

    claw_err_t err = usb_hid_mouse_move((int8_t)dx, (int8_t)dy);
    if (err != CLAW_OK) {
        cJSON_AddStringToObject(result, "error",
                                "USB HID not connected to host");
        return err;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "mouse moved dx=%d dy=%d", dx, dy);
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "message", msg);
    CLAW_LOGI(TAG, "%s", msg);
    return CLAW_OK;
}

/* ---- mouse_click ---- */

static claw_err_t tool_mouse_click(struct claw_tool *tool,
                                   const cJSON *params, cJSON *result)
{
    (void)tool;
    cJSON *btn_j = cJSON_GetObjectItem(params, "button");
    const char *btn_str = "left";
    uint8_t buttons = USB_HID_MOUSE_BTN_LEFT;

    if (btn_j && cJSON_IsString(btn_j)) {
        btn_str = btn_j->valuestring;
        if (strcmp(btn_str, "left") == 0) {
            buttons = USB_HID_MOUSE_BTN_LEFT;
        } else if (strcmp(btn_str, "right") == 0) {
            buttons = USB_HID_MOUSE_BTN_RIGHT;
        } else if (strcmp(btn_str, "middle") == 0) {
            buttons = USB_HID_MOUSE_BTN_MIDDLE;
        } else {
            cJSON_AddStringToObject(result, "error",
                                    "button must be left/right/middle");
            return CLAW_ERR_GENERIC;
        }
    }

    claw_err_t err = usb_hid_mouse_click(buttons);
    if (err != CLAW_OK) {
        cJSON_AddStringToObject(result, "error",
                                "USB HID not connected to host");
        return err;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "mouse %s click", btn_str);
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "message", msg);
    CLAW_LOGI(TAG, "%s", msg);
    return CLAW_OK;
}

/* ---- mouse_scroll ---- */

static claw_err_t tool_mouse_scroll(struct claw_tool *tool,
                                    const cJSON *params, cJSON *result)
{
    (void)tool;
    cJSON *delta_j = cJSON_GetObjectItem(params, "delta");

    if (!delta_j || !cJSON_IsNumber(delta_j)) {
        cJSON_AddStringToObject(result, "error", "missing delta");
        return CLAW_ERR_GENERIC;
    }

    int delta = delta_j->valueint;
    if (delta < -127 || delta > 127) {
        cJSON_AddStringToObject(result, "error",
                                "delta must be in range [-127, 127]");
        return CLAW_ERR_GENERIC;
    }

    claw_err_t err = usb_hid_mouse_scroll((int8_t)delta);
    if (err != CLAW_OK) {
        cJSON_AddStringToObject(result, "error",
                                "USB HID not connected to host");
        return err;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "mouse scroll delta=%d", delta);
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "message", msg);
    CLAW_LOGI(TAG, "%s", msg);
    return CLAW_OK;
}

/* JSON schema strings */

static const char schema_mouse_move[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"dx\":{\"type\":\"integer\","
    "\"description\":\"Horizontal movement (-127 to 127, positive=right)\"},"
    "\"dy\":{\"type\":\"integer\","
    "\"description\":\"Vertical movement (-127 to 127, positive=down)\"}},"
    "\"required\":[\"dx\",\"dy\"]}";

static const char schema_mouse_click[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"button\":{\"type\":\"string\","
    "\"enum\":[\"left\",\"right\",\"middle\"],"
    "\"description\":\"Mouse button (default: left)\"}}}";

static const char schema_mouse_scroll[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"delta\":{\"type\":\"integer\","
    "\"description\":\"Scroll amount (-127 to 127, positive=up)\"}},"
    "\"required\":[\"delta\"]}";

#endif /* CONFIG_RTCLAW_USB_HID_MOUSE */

/* ---- OOP tool registration ---- */

#ifdef CONFIG_RTCLAW_TOOL_MOUSE

#ifndef CONFIG_RTCLAW_USB_HID_MOUSE
static claw_err_t tool_mouse_unsupported(struct claw_tool *tool,
                                         const cJSON *params,
                                         cJSON *result)
{
    (void)tool;
    (void)params;
    cJSON_AddStringToObject(result, "error",
        "USB HID mouse not supported on this platform");
    return CLAW_OK;
}

static const char schema_mouse_move[] =
    "{\"type\":\"object\",\"properties\":{}}";
static const char schema_mouse_click[] =
    "{\"type\":\"object\",\"properties\":{}}";
static const char schema_mouse_scroll[] =
    "{\"type\":\"object\",\"properties\":{}}";

#define tool_mouse_move    tool_mouse_unsupported
#define tool_mouse_click   tool_mouse_unsupported
#define tool_mouse_scroll  tool_mouse_unsupported
#endif /* !CONFIG_RTCLAW_USB_HID_MOUSE */

static const struct claw_tool_ops mouse_move_ops = {
    .execute = tool_mouse_move,
};
static struct claw_tool mouse_move_tool = {
    .name = "mouse_move",
    .description =
        "Move the USB mouse cursor by relative offset. "
        "dx positive moves right, dy positive moves down. "
        "Range: -127 to 127 per axis.",
    .input_schema_json = schema_mouse_move,
    .ops = &mouse_move_ops,
    .required_caps = SWARM_CAP_MOUSE,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};
CLAW_TOOL_REGISTER(mouse_move, &mouse_move_tool);

static const struct claw_tool_ops mouse_click_ops = {
    .execute = tool_mouse_click,
};
static struct claw_tool mouse_click_tool = {
    .name = "mouse_click",
    .description =
        "Click a mouse button. Sends press + release. "
        "Default is left click. Options: left, right, middle.",
    .input_schema_json = schema_mouse_click,
    .ops = &mouse_click_ops,
    .required_caps = SWARM_CAP_MOUSE,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};
CLAW_TOOL_REGISTER(mouse_click, &mouse_click_tool);

static const struct claw_tool_ops mouse_scroll_ops = {
    .execute = tool_mouse_scroll,
};
static struct claw_tool mouse_scroll_tool = {
    .name = "mouse_scroll",
    .description =
        "Scroll the mouse wheel. Positive delta scrolls up, "
        "negative scrolls down. Range: -127 to 127.",
    .input_schema_json = schema_mouse_scroll,
    .ops = &mouse_scroll_ops,
    .required_caps = SWARM_CAP_MOUSE,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};
CLAW_TOOL_REGISTER(mouse_scroll, &mouse_scroll_tool);

#endif /* CONFIG_RTCLAW_TOOL_MOUSE */
