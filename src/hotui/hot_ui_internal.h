/**
 * @file hot_ui_internal.h
 * @brief HotUI 内部共享定义：QuickJS 绑定与宿主生命周期之间的接口。
 */

#ifndef HOT_UI_INTERNAL_H
#define HOT_UI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "quickjs.h"

#include "hot_ui.h"

#define HOT_UI_OBJ_MAX        64
#define HOT_UI_NAME_MAX       32
#define HOT_UI_TITLE_MAX      48
#define HOT_UI_TICK_MS        33
#define HOT_UI_CALL_TIMEOUT_MS  100
#define HOT_UI_START_TIMEOUT_MS 1500

typedef void (*hot_ui_error_cb_t)(const char *message);

/* ---------- QuickJS 绑定 ---------- */

int  hot_ui_api_register(JSContext *ctx);
void hot_ui_api_attach(JSContext *ctx);
void hot_ui_api_set_root(lv_obj_t *root);
void hot_ui_api_reset(void);
void hot_ui_api_bind_lifecycle(void);
bool hot_ui_api_has_tick(void);
bool hot_ui_api_has_touch(void);
int  hot_ui_api_call_start(void);
void hot_ui_api_call_tick(uint32_t dt_ms);
void hot_ui_api_call_touch(int x, int y, int down);
void hot_ui_api_call_swipe(int dx, int dy);
void hot_ui_api_set_error_cb(hot_ui_error_cb_t cb);
const char *hot_ui_api_last_error(void);

/* ---------- 执行超时与错误文本 ---------- */

void hot_ui_js_set_deadline(uint32_t timeout_ms);
void hot_ui_js_clear_deadline(void);
bool hot_ui_js_deadline_expired(void);
void hot_ui_js_exception_text(JSContext *ctx, char *buf, size_t size);

/* ---------- 公共 UI 辅助 ---------- */

const lv_font_t *hot_ui_font_for_size(int size);

#endif /* HOT_UI_INTERNAL_H */
