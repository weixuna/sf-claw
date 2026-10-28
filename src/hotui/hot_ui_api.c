/**
 * @file hot_ui_api.c
 * @brief HotUI 的 QuickJS 桥接层：ui.* API、对象表和触摸/按钮回调。
 *
 * 所有函数都只在 GUI 线程由 QuickJS 或 LVGL 事件调用，因此这里不做
 * 额外的线程同步。C 侧只把整数 ID 暴露给脚本，LVGL 对象指针始终留在
 * 本文件内。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rtthread.h>

#include "lvgl.h"
#include "lv_ext_resource_manager.h"
#include "lvsf_font.h"

#include "hot_ui_internal.h"

/* 对象类型决定 setText/setColor/setSize 的行为。 */
typedef enum {
    HOT_UI_OBJ_RECT = 1,
    HOT_UI_OBJ_CIRCLE,
    HOT_UI_OBJ_TEXT,
    HOT_UI_OBJ_BUTTON,
    HOT_UI_OBJ_LINE,
    HOT_UI_OBJ_ARC,
} hot_ui_obj_type_t;

typedef struct {
    lv_obj_t *obj;       /* LVGL 对象指针，仅 C 侧可见 */
    JSValue   cb;        /* 按钮回调；非按钮对象为 undefined */
    lv_point_t points[2];/* line 的两个端点，lv_line 只保存指针 */
    uint8_t   used;
    uint8_t   type;      /* hot_ui_obj_type_t */
    uint8_t   centered;  /* 圆/弧的坐标是否表示圆心 */
} hot_ui_obj_t;

static JSContext *s_ctx;
static lv_obj_t *s_root;
static hot_ui_obj_t s_objs[HOT_UI_OBJ_MAX];

static JSValue s_on_start = JS_UNDEFINED;
static JSValue s_on_tick = JS_UNDEFINED;
static JSValue s_on_touch = JS_UNDEFINED;
static JSValue s_on_swipe = JS_UNDEFINED;
static JSValue s_touch_cb = JS_UNDEFINED;   /* ui.onTouch() 注册的回调 */
static JSValue s_swipe_cb = JS_UNDEFINED;   /* ui.onSwipe() 注册的回调 */

static char s_error[HOT_UI_ERROR_MAX];
static hot_ui_error_cb_t s_error_cb;
static volatile uint32_t s_deadline;

/* ------------------------------------------------------------------ */
/*  调试与超时                                                         */
/* ------------------------------------------------------------------ */

void hot_ui_js_set_deadline(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = 1;
    }
    s_deadline = rt_tick_get() + rt_tick_from_millisecond(timeout_ms);
}

void hot_ui_js_clear_deadline(void)
{
    s_deadline = 0;
}

bool hot_ui_js_deadline_expired(void)
{
    if (s_deadline == 0) {
        return false;
    }
    return (int32_t)(rt_tick_get() - s_deadline) >= 0;
}

void hot_ui_api_set_error_cb(hot_ui_error_cb_t cb)
{
    s_error_cb = cb;
}

const char *hot_ui_api_last_error(void)
{
    return s_error;
}

/* 从 QuickJS 异常对象提取可读文本，优先使用 stack 以保留行号。 */
void hot_ui_js_exception_text(JSContext *ctx, char *buf, size_t size)
{
    JSValue exc;
    JSValue stack = JS_UNDEFINED;
    const char *text = NULL;
    bool copied = false;

    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';
    if (!ctx) {
        snprintf(buf, size, "未知脚本错误");
        return;
    }

    exc = JS_GetException(ctx);
    if (JS_IsObject(exc)) {
        stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsException(stack)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            stack = JS_UNDEFINED;
        }
    }

    if (!JS_IsUndefined(stack) && !JS_IsNull(stack)) {
        text = JS_ToCString(ctx, stack);
    }
    if (!text) {
        text = JS_ToCString(ctx, exc);
    }
    if (text) {
        snprintf(buf, size, "%s", text);
        copied = true;
        JS_FreeCString(ctx, text);
    }

    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);

    if (!copied || !buf[0]) {
        snprintf(buf, size, "未知脚本错误");
    }
    /* QuickJS 中断时的默认文本对用户不友好，统一成中文提示。 */
    if (strstr(buf, "interrupted")) {
        snprintf(buf, size, "脚本执行超时");
    }
}

/* 记录异常；notify 为 true 时通知宿主停止 tick。 */
static int hot_ui_api_check_result(JSContext *ctx, JSValue ret,
                                   const char *where, bool notify)
{
    char detail[HOT_UI_ERROR_MAX];

    if (!JS_IsException(ret)) {
        return 0;
    }

    hot_ui_js_exception_text(ctx, s_error, sizeof(s_error));
    if (notify) {
        snprintf(detail, sizeof(detail), "%s",
                 s_error[0] ? s_error : "未知脚本错误");
        snprintf(s_error, sizeof(s_error), "%s: %s", where, detail);
        if (s_error_cb) {
            s_error_cb(s_error);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  对象表                                                             */
/* ------------------------------------------------------------------ */

static hot_ui_obj_t *hot_ui_api_obj_get(int32_t id)
{
    if (id <= 0 || id > HOT_UI_OBJ_MAX) {
        return NULL;
    }
    return s_objs[id - 1].used ? &s_objs[id - 1] : NULL;
}

static int hot_ui_api_obj_add(lv_obj_t *obj, hot_ui_obj_type_t type,
                              bool centered)
{
    int i;

    for (i = 0; i < HOT_UI_OBJ_MAX; i++) {
        if (!s_objs[i].used) {
            s_objs[i].obj = obj;
            s_objs[i].cb = JS_UNDEFINED;
            s_objs[i].used = 1;
            s_objs[i].type = (uint8_t)type;
            s_objs[i].centered = centered ? 1 : 0;
            return i + 1;
        }
    }

    /* 对象表已满，丢弃刚创建的对象，避免泄漏。 */
    lv_obj_del(obj);
    return -1;
}

static void hot_ui_api_obj_release(hot_ui_obj_t *slot)
{
    if (!slot || !slot->used) {
        return;
    }
    if (s_ctx && !JS_IsUndefined(slot->cb)) {
        JS_FreeValue(s_ctx, slot->cb);
    }
    slot->obj = NULL;
    slot->cb = JS_UNDEFINED;
    slot->used = 0;
    slot->type = 0;
    slot->centered = 0;
}

static void hot_ui_api_clear_objects(void)
{
    int i;

    for (i = 0; i < HOT_UI_OBJ_MAX; i++) {
        hot_ui_api_obj_release(&s_objs[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  JS 参数辅助                                                        */
/* ------------------------------------------------------------------ */

static int hot_ui_arg_int(JSContext *ctx, int argc, JSValueConst *argv,
                          int index, int def, int *out)
{
    int32_t value;

    if (index >= argc || JS_IsUndefined(argv[index])) {
        *out = def;
        return 0;
    }
    if (JS_ToInt32(ctx, &value, argv[index]) < 0) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int hot_ui_arg_color(JSContext *ctx, int argc, JSValueConst *argv,
                            int index, uint32_t def, uint32_t *out)
{
    if (index >= argc || JS_IsUndefined(argv[index])) {
        *out = def;
        return 0;
    }
    return JS_ToUint32(ctx, out, argv[index]);
}

static const char *hot_ui_arg_string(JSContext *ctx, int argc,
                                     JSValueConst *argv, int index)
{
    if (index >= argc || !JS_IsString(argv[index])) {
        JS_ThrowTypeError(ctx, "参数 %d 需要字符串", index + 1);
        return NULL;
    }
    return JS_ToCString(ctx, argv[index]);
}

/* 根据像素字号选择工程里已经加载的字体，保证中文可以正常显示。 */
const lv_font_t *hot_ui_font_for_size(int size)
{
    const lv_font_t *font;

    if (size < 8) {
        size = 8;
    } else if (size > 72) {
        size = 72;
    }

    if (size <= 16) {
        font = LV_EXT_FONT_GET(LVSF_FONT_SMALL);
    } else if (size <= 20) {
        font = LV_EXT_FONT_GET(LVSF_FONT_NORMAL);
    } else if (size <= 24) {
        font = LV_EXT_FONT_GET(LVSF_FONT_SUBTITLE);
    } else if (size <= 28) {
        font = LV_EXT_FONT_GET(LVSF_FONT_TITLE);
    } else if (size <= 36) {
        font = LV_EXT_FONT_GET(LVSF_FONT_BIG);
    } else if (size <= 56) {
        font = LV_EXT_FONT_GET(LVSF_FONT_HUGE);
    } else {
        font = LV_EXT_FONT_GET(LVSF_FONT_SUPER);
    }

    return font ? font : LV_FONT_DEFAULT;
}

static lv_text_align_t hot_ui_text_align(int align)
{
    if (align == 1) {
        return LV_TEXT_ALIGN_CENTER;
    }
    if (align == 2) {
        return LV_TEXT_ALIGN_RIGHT;
    }
    return LV_TEXT_ALIGN_LEFT;
}

/* ------------------------------------------------------------------ */
/*  ui.* 实现                                                          */
/* ------------------------------------------------------------------ */

static JSValue js_ui_width(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, lv_disp_get_hor_res(NULL));
}

static JSValue js_ui_height(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, lv_disp_get_ver_res(NULL));
}

static JSValue js_ui_clear(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    uint32_t color;

    (void)this_val;
    if (hot_ui_arg_color(ctx, argc, argv, 0, 0x000000, &color) < 0) {
        return JS_EXCEPTION;
    }
    if (!s_root) {
        return JS_NewInt32(ctx, -1);
    }

    lv_obj_clean(s_root);
    hot_ui_api_clear_objects();
    lv_obj_set_style_bg_color(s_root, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_rect(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint32_t color;
    int x, y, w, h, radius;
    int id;
    lv_obj_t *obj;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &x) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &y) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 10, &w) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 3, 10, &h) < 0 ||
        hot_ui_arg_color(ctx, argc, argv, 4, 0xFFFFFF, &color) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 5, 0, &radius) < 0) {
        return JS_EXCEPTION;
    }
    if (w <= 0 || h <= 0 || !s_root) {
        return JS_NewInt32(ctx, -1);
    }
    if (radius < 0) {
        radius = 0;
    }

    obj = lv_obj_create(s_root);
    if (!obj) {
        return JS_NewInt32(ctx, -1);
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    id = hot_ui_api_obj_add(obj, HOT_UI_OBJ_RECT, false);
    return JS_NewInt32(ctx, id);
}

static JSValue js_ui_circle(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    uint32_t color;
    int x, y, r, diameter;
    int id;
    lv_obj_t *obj;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &x) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &y) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 10, &r) < 0 ||
        hot_ui_arg_color(ctx, argc, argv, 3, 0xFFFFFF, &color) < 0) {
        return JS_EXCEPTION;
    }
    if (r <= 0 || !s_root) {
        return JS_NewInt32(ctx, -1);
    }

    diameter = r * 2;
    obj = lv_obj_create(s_root);
    if (!obj) {
        return JS_NewInt32(ctx, -1);
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x - r, y - r);
    lv_obj_set_size(obj, diameter, diameter);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    id = hot_ui_api_obj_add(obj, HOT_UI_OBJ_CIRCLE, true);
    return JS_NewInt32(ctx, id);
}

static JSValue js_ui_text(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    const char *text;
    uint32_t color;
    int x, y, size, width, align;
    int id;
    lv_obj_t *obj;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &x) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &y) < 0 ||
        hot_ui_arg_color(ctx, argc, argv, 3, 0xFFFFFF, &color) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 4, 20, &size) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 5, 0, &width) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 6, 0, &align) < 0) {
        return JS_EXCEPTION;
    }
    text = hot_ui_arg_string(ctx, argc, argv, 2);
    if (!text) {
        return JS_EXCEPTION;
    }
    if (!s_root) {
        JS_FreeCString(ctx, text);
        return JS_NewInt32(ctx, -1);
    }

    obj = lv_label_create(s_root);
    if (!obj) {
        JS_FreeCString(ctx, text);
        return JS_NewInt32(ctx, -1);
    }
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(obj, hot_ui_font_for_size(size), 0);
    if (width > 0) {
        lv_obj_set_width(obj, width);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(obj, hot_ui_text_align(align), 0);
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    JS_FreeCString(ctx, text);

    id = hot_ui_api_obj_add(obj, HOT_UI_OBJ_TEXT, false);
    return JS_NewInt32(ctx, id);
}

static JSValue js_ui_line(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    uint32_t color;
    int x1, y1, x2, y2, width;
    int id;
    lv_obj_t *obj;
    hot_ui_obj_t *slot;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &x1) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &y1) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 0, &x2) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 3, 0, &y2) < 0 ||
        hot_ui_arg_color(ctx, argc, argv, 4, 0xFFFFFF, &color) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 5, 2, &width) < 0) {
        return JS_EXCEPTION;
    }
    if (!s_root || width <= 0) {
        return JS_NewInt32(ctx, -1);
    }

    obj = lv_line_create(s_root);
    if (!obj) {
        return JS_NewInt32(ctx, -1);
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_style_line_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(obj, width, 0);
    lv_obj_set_style_line_rounded(obj, true, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    id = hot_ui_api_obj_add(obj, HOT_UI_OBJ_LINE, false);
    if (id < 0) {
        return JS_NewInt32(ctx, -1);
    }

    slot = &s_objs[id - 1];
    slot->points[0].x = x1;
    slot->points[0].y = y1;
    slot->points[1].x = x2;
    slot->points[1].y = y2;
    lv_line_set_points(obj, slot->points, 2);
    return JS_NewInt32(ctx, id);
}

static JSValue js_ui_arc(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    uint32_t color;
    uint32_t bg_color;
    int x, y, r, start, end, width;
    int id;
    lv_obj_t *obj;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &x) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &y) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 50, &r) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 3, 0, &start) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 4, 360, &end) < 0 ||
        hot_ui_arg_color(ctx, argc, argv, 5, 0xFFFFFF, &color) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 6, 8, &width) < 0 ||
        hot_ui_arg_color(ctx, argc, argv, 7, 0x1C1C1E, &bg_color) < 0) {
        return JS_EXCEPTION;
    }
    if (!s_root || r <= 0 || width <= 0) {
        return JS_NewInt32(ctx, -1);
    }
    if (start < 0) {
        start = 0;
    } else if (start > 360) {
        start = 360;
    }
    if (end < 0) {
        end = 0;
    } else if (end > 360) {
        end = 360;
    }

    obj = lv_arc_create(s_root);
    if (!obj) {
        return JS_NewInt32(ctx, -1);
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x - r, y - r);
    lv_obj_set_size(obj, r * 2, r * 2);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_arc_width(obj, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(obj, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_arc_width(obj, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(obj, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(obj, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(obj, 0, LV_PART_KNOB);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    lv_arc_set_rotation(obj, 0);
    lv_arc_set_bg_angles(obj, 0, 360);
    lv_arc_set_angles(obj, (uint16_t)start, (uint16_t)end);

    id = hot_ui_api_obj_add(obj, HOT_UI_OBJ_ARC, true);
    return JS_NewInt32(ctx, id);
}

static void hot_ui_button_event_cb(lv_event_t *event)
{
    int id = (int)(uintptr_t)lv_event_get_user_data(event);
    hot_ui_obj_t *slot = hot_ui_api_obj_get(id);
    JSValue fn;
    JSValue ret;

    if (!slot || !s_ctx || JS_IsUndefined(slot->cb)) {
        return;
    }

    /* 复制一份引用，回调里如果 remove 自己也不会释放正在执行的对象。 */
    fn = JS_DupValue(s_ctx, slot->cb);
    hot_ui_js_set_deadline(HOT_UI_CALL_TIMEOUT_MS);
    ret = JS_Call(s_ctx, fn, JS_UNDEFINED, 0, NULL);
    hot_ui_js_clear_deadline();
    hot_ui_api_check_result(s_ctx, ret, "按钮回调", true);
    JS_FreeValue(s_ctx, ret);
    JS_FreeValue(s_ctx, fn);
}

static JSValue js_ui_button(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const char *text;
    uint32_t color;
    int x, y, w, h;
    int id;
    lv_obj_t *btn;
    lv_obj_t *label;
    JSValue cb = JS_UNDEFINED;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &x) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &y) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 100, &w) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 3, 44, &h) < 0) {
        return JS_EXCEPTION;
    }
    text = hot_ui_arg_string(ctx, argc, argv, 4);
    if (!text) {
        return JS_EXCEPTION;
    }
    color = 0x2C2C2E;
    if (argc > 5 && !JS_IsUndefined(argv[5]) && !JS_IsNull(argv[5])) {
        if (!JS_IsFunction(ctx, argv[5])) {
            JS_FreeCString(ctx, text);
            return JS_ThrowTypeError(ctx, "按钮回调必须是函数");
        }
        cb = JS_DupValue(ctx, argv[5]);
    }
    if (w <= 0 || h <= 0 || !s_root) {
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, cb);
        return JS_NewInt32(ctx, -1);
    }

    btn = lv_btn_create(s_root);
    if (!btn) {
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, cb);
        return JS_NewInt32(ctx, -1);
    }
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    label = lv_label_create(btn);
    if (label) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, hot_ui_font_for_size(20), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
    JS_FreeCString(ctx, text);

    id = hot_ui_api_obj_add(btn, HOT_UI_OBJ_BUTTON, false);
    if (id < 0) {
        JS_FreeValue(ctx, cb);
        return JS_NewInt32(ctx, -1);
    }
    if (!JS_IsUndefined(cb)) {
        s_objs[id - 1].cb = cb;
        lv_obj_add_event_cb(btn, hot_ui_button_event_cb,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    } else {
        JS_FreeValue(ctx, cb);
    }

    return JS_NewInt32(ctx, id);
}

static JSValue js_ui_move(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    int id, x, y;
    hot_ui_obj_t *slot;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &x) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 0, &y) < 0) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot) {
        return JS_NewInt32(ctx, -1);
    }

    if (slot->centered) {
        lv_obj_set_pos(slot->obj,
                       x - lv_obj_get_width(slot->obj) / 2,
                       y - lv_obj_get_height(slot->obj) / 2);
    } else {
        lv_obj_set_pos(slot->obj, x, y);
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_set_size(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    int id, w, h;
    hot_ui_obj_t *slot;
    lv_obj_t *label;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &w) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 0, &h) < 0) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot || w <= 0 || h <= 0 ||
        slot->type == HOT_UI_OBJ_TEXT ||
        slot->type == HOT_UI_OBJ_LINE) {
        return JS_NewInt32(ctx, -1);
    }

    lv_obj_set_size(slot->obj, w, h);
    if (slot->type == HOT_UI_OBJ_CIRCLE) {
        lv_obj_set_style_radius(slot->obj, LV_RADIUS_CIRCLE, 0);
    } else if (slot->type == HOT_UI_OBJ_BUTTON) {
        /* 按钮尺寸变化后重新居中文字。 */
        label = lv_obj_get_child(slot->obj, 0);
        if (label) {
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        }
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_set_color(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    uint32_t color;
    int id;
    hot_ui_obj_t *slot;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0 ||
        hot_ui_arg_color(ctx, argc, argv, 1, 0xFFFFFF, &color) < 0) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot) {
        return JS_NewInt32(ctx, -1);
    }

    if (slot->type == HOT_UI_OBJ_TEXT) {
        lv_obj_set_style_text_color(slot->obj, lv_color_hex(color), 0);
    } else if (slot->type == HOT_UI_OBJ_LINE) {
        lv_obj_set_style_line_color(slot->obj, lv_color_hex(color), 0);
    } else if (slot->type == HOT_UI_OBJ_ARC) {
        lv_obj_set_style_arc_color(slot->obj, lv_color_hex(color),
                                   LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_bg_color(slot->obj, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(slot->obj, LV_OPA_COVER, 0);
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_set_text(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *text;
    int id;
    hot_ui_obj_t *slot;
    lv_obj_t *label;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0) {
        return JS_EXCEPTION;
    }
    text = hot_ui_arg_string(ctx, argc, argv, 1);
    if (!text) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot) {
        JS_FreeCString(ctx, text);
        return JS_NewInt32(ctx, -1);
    }

    if (slot->type == HOT_UI_OBJ_TEXT) {
        lv_label_set_text(slot->obj, text);
    } else if (slot->type == HOT_UI_OBJ_BUTTON) {
        label = lv_obj_get_child(slot->obj, 0);
        if (label) {
            lv_label_set_text(label, text);
        }
    } else {
        JS_FreeCString(ctx, text);
        return JS_NewInt32(ctx, -1);
    }
    JS_FreeCString(ctx, text);
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_remove(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    int id;
    hot_ui_obj_t *slot;
    lv_obj_t *obj;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot) {
        return JS_NewInt32(ctx, -1);
    }

    obj = slot->obj;
    hot_ui_api_obj_release(slot);
    lv_obj_del(obj);
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_set_line(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    int id, x1, y1, x2, y2;
    hot_ui_obj_t *slot;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &x1) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 0, &y1) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 3, 0, &x2) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 4, 0, &y2) < 0) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot || slot->type != HOT_UI_OBJ_LINE) {
        return JS_NewInt32(ctx, -1);
    }

    slot->points[0].x = x1;
    slot->points[0].y = y1;
    slot->points[1].x = x2;
    slot->points[1].y = y2;
    lv_line_set_points(slot->obj, slot->points, 2);
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_set_arc(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    int id, start, end;
    hot_ui_obj_t *slot;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &start) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 2, 360, &end) < 0) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot || slot->type != HOT_UI_OBJ_ARC) {
        return JS_NewInt32(ctx, -1);
    }
    if (start < 0) {
        start = 0;
    } else if (start > 360) {
        start = 360;
    }
    if (end < 0) {
        end = 0;
    } else if (end > 360) {
        end = 360;
    }
    lv_arc_set_angles(slot->obj, (uint16_t)start, (uint16_t)end);
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_set_radius(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    int id, radius;
    hot_ui_obj_t *slot;

    (void)this_val;
    if (hot_ui_arg_int(ctx, argc, argv, 0, 0, &id) < 0 ||
        hot_ui_arg_int(ctx, argc, argv, 1, 0, &radius) < 0) {
        return JS_EXCEPTION;
    }
    slot = hot_ui_api_obj_get(id);
    if (!slot || (slot->type != HOT_UI_OBJ_RECT &&
                  slot->type != HOT_UI_OBJ_BUTTON)) {
        return JS_NewInt32(ctx, -1);
    }
    if (radius < 0) {
        radius = 0;
    }
    lv_obj_set_style_radius(slot->obj, radius, 0);
    return JS_NewInt32(ctx, 0);
}

static JSValue js_ui_on_touch(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue cb = (argc > 0) ? argv[0] : JS_UNDEFINED;

    (void)this_val;
    if (!JS_IsUndefined(cb) && !JS_IsNull(cb) && !JS_IsFunction(ctx, cb)) {
        return JS_ThrowTypeError(ctx, "onTouch 需要函数参数");
    }

    if (!JS_IsUndefined(s_touch_cb)) {
        JS_FreeValue(ctx, s_touch_cb);
    }
    s_touch_cb = (JS_IsUndefined(cb) || JS_IsNull(cb))
                     ? JS_UNDEFINED
                     : JS_DupValue(ctx, cb);
    return JS_UNDEFINED;
}

static JSValue js_ui_on_swipe(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue cb = (argc > 0) ? argv[0] : JS_UNDEFINED;

    (void)this_val;
    if (!JS_IsUndefined(cb) && !JS_IsNull(cb) && !JS_IsFunction(ctx, cb)) {
        return JS_ThrowTypeError(ctx, "onSwipe 需要函数参数");
    }

    if (!JS_IsUndefined(s_swipe_cb)) {
        JS_FreeValue(ctx, s_swipe_cb);
    }
    s_swipe_cb = (JS_IsUndefined(cb) || JS_IsNull(cb))
                     ? JS_UNDEFINED
                     : JS_DupValue(ctx, cb);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry hot_ui_ui_funcs[] = {
    JS_CFUNC_DEF("width",    0, js_ui_width),
    JS_CFUNC_DEF("height",   0, js_ui_height),
    JS_CFUNC_DEF("clear",    1, js_ui_clear),
    JS_CFUNC_DEF("rect",     6, js_ui_rect),
    JS_CFUNC_DEF("circle",   4, js_ui_circle),
    JS_CFUNC_DEF("line",     6, js_ui_line),
    JS_CFUNC_DEF("arc",      8, js_ui_arc),
    JS_CFUNC_DEF("text",     7, js_ui_text),
    JS_CFUNC_DEF("button",   6, js_ui_button),
    JS_CFUNC_DEF("move",     3, js_ui_move),
    JS_CFUNC_DEF("setSize",  3, js_ui_set_size),
    JS_CFUNC_DEF("setColor", 2, js_ui_set_color),
    JS_CFUNC_DEF("setText",  2, js_ui_set_text),
    JS_CFUNC_DEF("setLine",  5, js_ui_set_line),
    JS_CFUNC_DEF("setArc",   3, js_ui_set_arc),
    JS_CFUNC_DEF("setRadius", 2, js_ui_set_radius),
    JS_CFUNC_DEF("remove",   1, js_ui_remove),
    JS_CFUNC_DEF("onTouch",  1, js_ui_on_touch),
    JS_CFUNC_DEF("onSwipe",  1, js_ui_on_swipe),
};

/* ------------------------------------------------------------------ */
/*  生命周期回调                                                       */
/* ------------------------------------------------------------------ */

static JSValue hot_ui_api_global_func(const char *name)
{
    JSValue global;
    JSValue fn;

    global = JS_GetGlobalObject(s_ctx);
    if (JS_IsException(global)) {
        JS_FreeValue(s_ctx, JS_GetException(s_ctx));
        return JS_UNDEFINED;
    }

    fn = JS_GetPropertyStr(s_ctx, global, name);
    JS_FreeValue(s_ctx, global);
    if (JS_IsException(fn)) {
        JS_FreeValue(s_ctx, JS_GetException(s_ctx));
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(s_ctx, fn)) {
        JS_FreeValue(s_ctx, fn);
        return JS_UNDEFINED;
    }
    return fn;
}

void hot_ui_api_bind_lifecycle(void)
{
    if (!s_ctx) {
        return;
    }
    s_on_start = hot_ui_api_global_func("on_start");
    s_on_tick = hot_ui_api_global_func("on_tick");
    s_on_touch = hot_ui_api_global_func("on_touch");
    s_on_swipe = hot_ui_api_global_func("on_swipe");
}

bool hot_ui_api_has_tick(void)
{
    return s_ctx && JS_IsFunction(s_ctx, s_on_tick);
}

bool hot_ui_api_has_touch(void)
{
    return s_ctx && (JS_IsFunction(s_ctx, s_on_touch) ||
                     JS_IsFunction(s_ctx, s_on_swipe) ||
                     JS_IsFunction(s_ctx, s_touch_cb) ||
                     JS_IsFunction(s_ctx, s_swipe_cb));
}

int hot_ui_api_call_start(void)
{
    JSValue fn;
    JSValue ret;
    int rc;

    if (!s_ctx || JS_IsUndefined(s_on_start)) {
        return 0;
    }

    fn = JS_DupValue(s_ctx, s_on_start);
    hot_ui_js_set_deadline(HOT_UI_START_TIMEOUT_MS);
    ret = JS_Call(s_ctx, fn, JS_UNDEFINED, 0, NULL);
    hot_ui_js_clear_deadline();
    rc = hot_ui_api_check_result(s_ctx, ret, "on_start", false);
    JS_FreeValue(s_ctx, ret);
    JS_FreeValue(s_ctx, fn);
    return rc;
}

static int hot_ui_api_call_fn(JSValue fn, const char *where,
                              JSValueConst *argv, int argc)
{
    JSValue call_fn;
    JSValue ret;
    int rc;

    if (!s_ctx || JS_IsUndefined(fn)) {
        return 0;
    }

    call_fn = JS_DupValue(s_ctx, fn);
    hot_ui_js_set_deadline(HOT_UI_CALL_TIMEOUT_MS);
    ret = JS_Call(s_ctx, call_fn, JS_UNDEFINED, argc, argv);
    hot_ui_js_clear_deadline();
    rc = hot_ui_api_check_result(s_ctx, ret, where, true);
    JS_FreeValue(s_ctx, ret);
    JS_FreeValue(s_ctx, call_fn);
    return rc;
}

void hot_ui_api_call_tick(uint32_t dt_ms)
{
    JSValue argv[1];

    if (!s_ctx || !JS_IsFunction(s_ctx, s_on_tick)) {
        return;
    }
    argv[0] = JS_NewInt32(s_ctx, (int32_t)dt_ms);
    hot_ui_api_call_fn(s_on_tick, "on_tick", argv, 1);
    JS_FreeValue(s_ctx, argv[0]);
}

void hot_ui_api_call_touch(int x, int y, int down)
{
    JSValue argv[3];

    if (!s_ctx) {
        return;
    }

    argv[0] = JS_NewInt32(s_ctx, x);
    argv[1] = JS_NewInt32(s_ctx, y);
    argv[2] = JS_NewInt32(s_ctx, down ? 1 : 0);

    if (hot_ui_api_call_fn(s_on_touch, "on_touch", argv, 3) == 0) {
        hot_ui_api_call_fn(s_touch_cb, "ui.onTouch", argv, 3);
    }

    JS_FreeValue(s_ctx, argv[0]);
    JS_FreeValue(s_ctx, argv[1]);
    JS_FreeValue(s_ctx, argv[2]);
}

void hot_ui_api_call_swipe(int dx, int dy)
{
    JSValue argv[2];

    if (!s_ctx) {
        return;
    }

    argv[0] = JS_NewInt32(s_ctx, dx);
    argv[1] = JS_NewInt32(s_ctx, dy);

    if (hot_ui_api_call_fn(s_on_swipe, "on_swipe", argv, 2) == 0) {
        hot_ui_api_call_fn(s_swipe_cb, "ui.onSwipe", argv, 2);
    }

    JS_FreeValue(s_ctx, argv[0]);
    JS_FreeValue(s_ctx, argv[1]);
}

/* ------------------------------------------------------------------ */
/*  绑定的初始化与清理                                                 */
/* ------------------------------------------------------------------ */

int hot_ui_api_register(JSContext *ctx)
{
    JSValue ui;
    JSValue global;
    int count = (int)(sizeof(hot_ui_ui_funcs) / sizeof(hot_ui_ui_funcs[0]));

    ui = JS_NewObject(ctx);
    if (JS_IsException(ui)) {
        return -1;
    }
    JS_SetPropertyFunctionList(ctx, ui, hot_ui_ui_funcs, count);

    global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global)) {
        JS_FreeValue(ctx, ui);
        return -1;
    }
    if (JS_SetPropertyStr(ctx, global, "ui", ui) < 0) {
        JS_FreeValue(ctx, global);
        return -1;
    }
    JS_FreeValue(ctx, global);
    return 0;
}

void hot_ui_api_attach(JSContext *ctx)
{
    s_ctx = ctx;
}

void hot_ui_api_set_root(lv_obj_t *root)
{
    s_root = root;
}

void hot_ui_api_reset(void)
{
    int i;

    if (s_ctx) {
        for (i = 0; i < HOT_UI_OBJ_MAX; i++) {
            hot_ui_api_obj_release(&s_objs[i]);
        }
        if (!JS_IsUndefined(s_on_start)) {
            JS_FreeValue(s_ctx, s_on_start);
        }
        if (!JS_IsUndefined(s_on_tick)) {
            JS_FreeValue(s_ctx, s_on_tick);
        }
        if (!JS_IsUndefined(s_on_touch)) {
            JS_FreeValue(s_ctx, s_on_touch);
        }
        if (!JS_IsUndefined(s_on_swipe)) {
            JS_FreeValue(s_ctx, s_on_swipe);
        }
        if (!JS_IsUndefined(s_touch_cb)) {
            JS_FreeValue(s_ctx, s_touch_cb);
        }
        if (!JS_IsUndefined(s_swipe_cb)) {
            JS_FreeValue(s_ctx, s_swipe_cb);
        }
    }

    s_on_start = JS_UNDEFINED;
    s_on_tick = JS_UNDEFINED;
    s_on_touch = JS_UNDEFINED;
    s_on_swipe = JS_UNDEFINED;
    s_touch_cb = JS_UNDEFINED;
    s_swipe_cb = JS_UNDEFINED;
    for (i = 0; i < HOT_UI_OBJ_MAX; i++) {
        s_objs[i].obj = NULL;
        s_objs[i].cb = JS_UNDEFINED;
        s_objs[i].used = 0;
        s_objs[i].type = 0;
        s_objs[i].centered = 0;
    }

    s_ctx = NULL;
    s_root = NULL;
    s_deadline = 0;
    s_error[0] = '\0';
}
