/**
 * @file hot_ui_tool.c
 * @brief 给 AI 使用的 ui_hot_reload 工具。
 *
 * 工具运行在 AI worker 线程，只负责校验参数、投递脚本和等待结果，
 * 不会直接调用 LVGL 或创建 QuickJS 上下文。
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "claw/tools/claw_tools.h"

#include "hot_ui.h"

static const char hot_ui_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\","
    "\"description\":\"短标识，只允许字母、数字、下划线和连字符\"},"
    "\"title\":{\"type\":\"string\","
    "\"description\":\"显示给用户的标题\"},"
    "\"code\":{\"type\":\"string\","
    "\"description\":\"使用 ui.* API 的 JavaScript，最多 16384 字节\"}},"
    "\"required\":[\"name\",\"code\"]}";

static claw_err_t hot_ui_tool_execute(struct claw_tool *tool,
                                      const cJSON *params,
                                      cJSON *result)
{
    cJSON *name_json;
    cJSON *title_json;
    cJSON *code_json;
    const char *name;
    const char *title;
    const char *code;
    char error[HOT_UI_ERROR_MAX] = {0};
    char message[96];
    size_t len;
    int rc;

    (void)tool;

    name_json = cJSON_GetObjectItemCaseSensitive(params, "name");
    title_json = cJSON_GetObjectItemCaseSensitive(params, "title");
    code_json = cJSON_GetObjectItemCaseSensitive(params, "code");
    if (!cJSON_IsString(name_json) || !name_json->valuestring[0] ||
        !cJSON_IsString(code_json) || !code_json->valuestring[0]) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "缺少 name 或 code");
        return CLAW_ERROR;
    }

    name = name_json->valuestring;
    title = cJSON_IsString(title_json) ? title_json->valuestring : name;
    code = code_json->valuestring;
    len = strlen(code);
    if (len > HOT_UI_SCRIPT_MAX) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error",
                                "脚本超过 16384 字节，请精简后重试");
        return CLAW_ERROR;
    }

    rc = hot_ui_submit(name, title, code, len, 3000,
                       error, sizeof(error));
    if (rc == 0) {
        cJSON_AddBoolToObject(result, "ok", true);
        snprintf(message, sizeof(message), "running: %s", name);
        cJSON_AddStringToObject(result, "message", message);
    } else if (rc == 1) {
        /* 脚本已经排队，界面稍后出现；不要让 AI worker 继续阻塞。 */
        cJSON_AddBoolToObject(result, "ok", true);
        snprintf(message, sizeof(message), "queued: %s", name);
        cJSON_AddStringToObject(result, "message", message);
    } else {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error",
                                error[0] ? error : "HotUI 执行失败");
    }
    return CLAW_OK;
}

static const struct claw_tool_ops hot_ui_tool_ops = {
    .execute = hot_ui_tool_execute,
};

static struct claw_tool hot_ui_tool = {
    .name = "ui_hot_reload",
    .description =
        "Create or hot-reload the watch UI from JavaScript. Use this for "
        "any request for a screen, tool, game, animation, dashboard or "
        "control panel. Always call this tool; never paste JavaScript into "
        "the chat reply. Only the ui.* API is available. Canvas is 466x466; "
        "keep content inside the round safe area x=60..406, y=100..380. "
        "Define function on_start() to build the UI; optional on_tick(dt_ms) "
        "for animation, on_touch(x,y,down) for touch and on_swipe(dx,dy) "
        "for swipes. on_touch receives down=1 while pressed and down=0 on "
        "release; prefer ui.onSwipe(cb) for games. API: "
        "ui.clear(color); ui.rect(x,y,w,h,color,radius); "
        "ui.circle(x,y,r,color) with center coordinates; "
        "ui.line(x1,y1,x2,y2,color,width) with absolute points; "
        "ui.arc(x,y,r,start,end,color,width,bgcolor) with center coordinates; "
        "ui.text(x,y,text,color,size,width,align) with size in pixels, "
        "optional width and align 0=left/1=center/2=right; "
        "ui.button(x,y,w,h,text,cb); ui.move(id,x,y); "
        "ui.setSize(id,w,h); ui.setColor(id,color); ui.setText(id,text); "
        "ui.setLine(id,x1,y1,x2,y2); ui.setArc(id,start,end); "
        "ui.setRadius(id,r); ui.remove(id); ui.onTouch(cb); ui.onSwipe(cb). "
        "Arc angles: 0=right, 90=down, clockwise. At most 64 live objects. "
        "Plan the layout before drawing: put title/score around y=80..130, "
        "main content around y=140..380, and do not place controls below "
        "y=390. A 4x4 game board can use cell=56, gap=6, board 258x258 at "
        "x=104,y=134. Keep 40px between unrelated elements and reuse objects "
        "with setColor/setText instead of clear/recreating every frame. "
        "For a polished look use a dark background, rounded cards, accent "
        "colors, large primary values and thin dividers. A watch face can "
        "use ui.circle/ui.arc for the dial and ui.line for the hour, minute "
        "and second hands updated in on_tick. Colors use 0xRRGGBB. "
        "Code is synchronous: do not use async, setTimeout, console, lv.*, "
        "std, os, files or network.",
    .input_schema_json = hot_ui_schema,
    .ops = &hot_ui_tool_ops,
    .flags = CLAW_TOOL_LOCAL_ONLY,
};

CLAW_TOOL_REGISTER(ui_hot_reload, &hot_ui_tool);
