/**
 * @file hot_ui.c
 * @brief HotUI App：QuickJS 脚本的宿主、热重载状态机和 GUI 命令队列。
 *
 * 线程规则：
 *   - LVGL 和 QuickJS 只在 GUI 线程使用；
 *   - AI 工具线程只负责投递脚本并等待结果；
 *   - 超时不会无限阻塞 AI worker。
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rtthread.h>
#include <finsh.h>

#include "lvgl.h"
#include "gui_app_fwk.h"
#include "lv_ext_resource_manager.h"

#include "osal/claw_os.h"
#include "quickjs.h"

#include "hot_ui.h"
#include "hot_ui_internal.h"

#define TAG "hotui"

#define HOT_UI_APP_ID            "HotUI"
#define HOT_UI_JS_MEM_LIMIT      (192 * 1024)
#define HOT_UI_JS_STACK_LIMIT    (8 * 1024)
#define HOT_UI_COMPILE_TIMEOUT_MS 1500
#define HOT_UI_DISPATCH_MS       50
#define HOT_UI_START_RETRY_MS    5000
#define HOT_UI_REQUEST_TTL_MS    15000

typedef enum {
    HOT_UI_STATE_IDLE = 0,
    HOT_UI_STATE_RUNNING,
    HOT_UI_STATE_ERROR,
} hot_ui_state_t;

typedef struct {
    bool pending;        /* 已排队，等待 GUI 处理 */
    bool done;           /* GUI 已处理，等待工具线程读取 */
    bool waiting;        /* 工具线程仍在等待结果 */
    bool processing;     /* GUI 正在执行脚本 */
    char *code;          /* 请求脚本，GUI 处理完释放 */
    size_t len;
    char name[HOT_UI_NAME_MAX];
    char title[HOT_UI_TITLE_MAX];
    int result;
    char error[HOT_UI_ERROR_MAX];
    uint32_t submit_tick;
} hot_ui_request_t;

typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
    lv_obj_t *root;
    lv_timer_t *tick_timer;
    lv_timer_t *dispatch_timer;
    struct claw_mutex *lock;
    struct claw_sem *done;

    char *script;           /* 当前脚本，PSRAM */
    size_t script_len;
    char *last_good;        /* 最近一次成功运行的脚本 */
    size_t last_good_len;

    char name[HOT_UI_NAME_MAX];
    char title[HOT_UI_TITLE_MAX];
    char error[HOT_UI_ERROR_MAX];

    hot_ui_state_t state;
    bool app_active;
    bool paused;
    bool has_tick;
    bool has_touch;
    bool ready;

    volatile uint32_t last_start_tick;
} hot_ui_ctx_t;

static hot_ui_ctx_t s;
static hot_ui_request_t s_req;

/* 全局触摸起点，用于把一次按下/抬起识别成滑动手势。 */
static lv_point_t s_touch_start;
static bool s_touch_tracking;

static void hot_ui_teardown(void);
static int  hot_ui_install(const char *code, size_t len,
                           const char *name, const char *title,
                           bool allow_rollback,
                           char *error, size_t error_size);
static bool hot_ui_process_request(void);
static void hot_ui_show_idle(void);
static void hot_ui_show_error(const char *text);

/* ------------------------------------------------------------------ */
/*  基础辅助                                                           */
/* ------------------------------------------------------------------ */

static void hot_ui_lock(void)
{
    if (s.lock) {
        claw_mutex_lock(s.lock, CLAW_WAIT_FOREVER);
    }
}

static void hot_ui_unlock(void)
{
    if (s.lock) {
        claw_mutex_unlock(s.lock);
    }
}

static void hot_ui_set_error(char *dst, size_t size, const char *text)
{
    if (dst && size) {
        snprintf(dst, size, "%s", text ? text : "");
    }
}

static bool hot_ui_name_valid(const char *name)
{
    size_t i;
    size_t len;

    if (!name) {
        return false;
    }
    len = strlen(name);
    if (len == 0 || len >= HOT_UI_NAME_MAX) {
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (!isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static const char *hot_ui_state_name(hot_ui_state_t state)
{
    switch (state) {
    case HOT_UI_STATE_RUNNING:
        return "running";
    case HOT_UI_STATE_ERROR:
        return "error";
    default:
        return "idle";
    }
}

/* ------------------------------------------------------------------ */
/*  QuickJS 编译与执行                                                 */
/* ------------------------------------------------------------------ */

/* 单次 JS 执行超时；QuickJS 会在中断回调返回非 0 时抛出异常。 */
static int hot_ui_interrupt_cb(JSRuntime *rt, void *opaque)
{
    (void)rt;
    (void)opaque;
    return hot_ui_js_deadline_expired() ? 1 : 0;
}

/*
 * 只编译、不执行。先拿到可执行函数再拆旧界面，这样语法错误时
 * 旧画面仍然保留。
 */
static int hot_ui_js_compile(const char *code, size_t len,
                             JSRuntime **out_rt, JSContext **out_ctx,
                             JSValue *out_func,
                             char *error, size_t error_size)
{
    JSRuntime *rt = NULL;
    JSContext *ctx = NULL;
    JSValue func = JS_UNDEFINED;

    *out_rt = NULL;
    *out_ctx = NULL;
    *out_func = JS_UNDEFINED;

    rt = JS_NewRuntime();
    if (!rt) {
        hot_ui_set_error(error, error_size, "QuickJS 运行时创建失败");
        return -1;
    }
    JS_SetMemoryLimit(rt, HOT_UI_JS_MEM_LIMIT);
    JS_SetMaxStackSize(rt, HOT_UI_JS_STACK_LIMIT);
    JS_SetInterruptHandler(rt, hot_ui_interrupt_cb, NULL);

    ctx = JS_NewContext(rt);
    if (!ctx) {
        hot_ui_set_error(error, error_size, "QuickJS 上下文创建失败");
        JS_FreeRuntime(rt);
        return -1;
    }

    if (hot_ui_api_register(ctx) != 0) {
        hot_ui_js_exception_text(ctx, error, error_size);
        if (!error[0]) {
            hot_ui_set_error(error, error_size, "ui API 注册失败");
        }
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return -1;
    }

    hot_ui_js_set_deadline(HOT_UI_COMPILE_TIMEOUT_MS);
    func = JS_Eval(ctx, code, len, "<hotui>",
                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    hot_ui_js_clear_deadline();

    if (JS_IsException(func)) {
        hot_ui_js_exception_text(ctx, error, error_size);
        JS_FreeValue(ctx, func);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return -1;
    }

    *out_rt = rt;
    *out_ctx = ctx;
    *out_func = func;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  LVGL 根对象与事件                                                  */
/* ------------------------------------------------------------------ */

static void hot_ui_touch_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev;
    lv_point_t point;
    int down;

    /* 按钮有自己的回调，全局触摸只处理落在画布上的触摸。 */
    if (lv_event_get_target(event) != s.root) {
        s_touch_tracking = false;
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }
    indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        s_touch_start = point;
        s_touch_tracking = true;
    }

    down = (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) ? 0 : 1;
    hot_ui_api_call_touch(point.x, point.y, down);

    if (code == LV_EVENT_RELEASED && s_touch_tracking) {
        int dx = point.x - s_touch_start.x;
        int dy = point.y - s_touch_start.y;

        if (dx > 24 || dx < -24 || dy > 24 || dy < -24) {
            hot_ui_api_call_swipe(dx, dy);
        }
        s_touch_tracking = false;
    } else if (code == LV_EVENT_PRESS_LOST) {
        s_touch_tracking = false;
    }
}

static int hot_ui_create_root(void)
{
    lv_obj_t *root;

    root = lv_obj_create(lv_scr_act());
    if (!root) {
        return -1;
    }

    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, hot_ui_touch_cb, LV_EVENT_ALL, NULL);

    s.root = root;
    hot_ui_api_set_root(root);
    return 0;
}

static void hot_ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s.ctx || !s.has_tick || s.paused) {
        return;
    }
    hot_ui_api_call_tick(HOT_UI_TICK_MS);
}

static void hot_ui_start_tick(void)
{
    if (!s.ctx || !s.has_tick || s.tick_timer) {
        return;
    }
    s.tick_timer = lv_timer_create(hot_ui_tick_cb, HOT_UI_TICK_MS, NULL);
    if (s.tick_timer && s.paused) {
        lv_timer_pause(s.tick_timer);
    }
}

/* 运行期异常：停止 tick，保留最后一帧，等待下一次热重载。 */
static void hot_ui_on_js_error(const char *message)
{
    snprintf(s.error, sizeof(s.error), "%s",
             (message && message[0]) ? message : "脚本运行错误");
    s.state = HOT_UI_STATE_ERROR;
    s.has_tick = false;
    if (s.tick_timer) {
        lv_timer_del(s.tick_timer);
        s.tick_timer = NULL;
    }
    CLAW_LOGE(TAG, "%s", s.error);
}

static void hot_ui_teardown(void)
{
    if (s.tick_timer) {
        lv_timer_del(s.tick_timer);
        s.tick_timer = NULL;
    }
    if (s.root) {
        lv_obj_del(s.root);
        s.root = NULL;
    }

    /* 先释放 JS 回调和对象表，再释放 context。 */
    hot_ui_api_reset();
    if (s.ctx) {
        JS_FreeContext(s.ctx);
        s.ctx = NULL;
    }
    if (s.rt) {
        JS_FreeRuntime(s.rt);
        s.rt = NULL;
    }

    s.has_tick = false;
    s.has_touch = false;
    s.paused = false;
    s_touch_tracking = false;
}

/* ------------------------------------------------------------------ */
/*  脚本安装 / 热重载                                                  */
/* ------------------------------------------------------------------ */

static int hot_ui_install(const char *code, size_t len,
                          const char *name, const char *title,
                          bool allow_rollback,
                          char *error, size_t error_size)
{
    JSRuntime *new_rt = NULL;
    JSContext *new_ctx = NULL;
    JSValue func = JS_UNDEFINED;
    JSValue ret;
    char err[HOT_UI_ERROR_MAX] = {0};

    if (!code || len == 0 || len > HOT_UI_SCRIPT_MAX ||
        !s.script || !s.last_good) {
        hot_ui_set_error(error, error_size, "脚本长度无效");
        return -1;
    }

    /* 编译通过前不动旧界面。 */
    if (hot_ui_js_compile(code, len, &new_rt, &new_ctx, &func,
                          err, sizeof(err)) != 0) {
        snprintf(s.error, sizeof(s.error), "%s", err);
        hot_ui_set_error(error, error_size, err);
        CLAW_LOGW(TAG, "compile failed: %s", err);
        return -1;
    }

    /* 语法通过，开始替换旧 runtime 和旧画面。 */
    hot_ui_teardown();
    s.rt = new_rt;
    s.ctx = new_ctx;
    new_rt = NULL;
    new_ctx = NULL;

    if (hot_ui_create_root() != 0) {
        snprintf(err, sizeof(err), "创建 UI 根对象失败");
        goto fail;
    }
    hot_ui_api_attach(s.ctx);

    hot_ui_js_set_deadline(HOT_UI_START_TIMEOUT_MS);
    ret = JS_EvalFunction(s.ctx, func);
    func = JS_UNDEFINED;    /* JS_EvalFunction 接管所有权 */
    hot_ui_js_clear_deadline();
    if (JS_IsException(ret)) {
        hot_ui_js_exception_text(s.ctx, err, sizeof(err));
        JS_FreeValue(s.ctx, ret);
        goto fail;
    }
    JS_FreeValue(s.ctx, ret);

    hot_ui_api_bind_lifecycle();
    if (hot_ui_api_call_start() != 0) {
        snprintf(err, sizeof(err), "%s",
                 hot_ui_api_last_error()[0] ? hot_ui_api_last_error()
                                            : "on_start 执行失败");
        goto fail;
    }

    /* 完整跑通后才提交脚本，失败时保留旧脚本。 */
    hot_ui_lock();
    memmove(s.script, code, len);
    s.script[len] = '\0';
    s.script_len = len;
    memmove(s.last_good, code, len);
    s.last_good[len] = '\0';
    s.last_good_len = len;
    snprintf(s.name, sizeof(s.name), "%s", name ? name : "app");
    snprintf(s.title, sizeof(s.title), "%s",
             (title && title[0]) ? title : s.name);
    s.error[0] = '\0';
    s.state = HOT_UI_STATE_RUNNING;
    s.has_tick = hot_ui_api_has_tick();
    s.has_touch = hot_ui_api_has_touch();
    hot_ui_unlock();
    hot_ui_start_tick();
    if (!s.tick_timer) {
        s.has_tick = false;
    }
    hot_ui_set_error(error, error_size, "");
    CLAW_LOGI(TAG, "running: %s (%u bytes)", s.name, (unsigned)len);
    return 0;

fail:
    if (!JS_IsUndefined(func) && s.ctx) {
        JS_FreeValue(s.ctx, func);
    }
    hot_ui_teardown();
    if (!err[0]) {
        snprintf(err, sizeof(err), "脚本运行失败");
    }
    snprintf(s.error, sizeof(s.error), "%s", err);
    CLAW_LOGE(TAG, "run failed: %s", err);

    /* 运行期失败时回滚到上一版可用脚本；编译失败不会走到这里。 */
    if (allow_rollback && s.last_good_len > 0 &&
        !(len == s.last_good_len &&
          memcmp(code, s.last_good, len) == 0)) {
        char rollback_err[HOT_UI_ERROR_MAX] = {0};

        if (hot_ui_install(s.last_good, s.last_good_len,
                           s.name, s.title, false,
                           rollback_err, sizeof(rollback_err)) == 0) {
            snprintf(s.error, sizeof(s.error), "%s", err);
            hot_ui_set_error(error, error_size, err);
            return -1;
        }
        {
            char combined[HOT_UI_ERROR_MAX];

            snprintf(combined, sizeof(combined), "%s；回滚失败：%s",
                     err, rollback_err);
            snprintf(err, sizeof(err), "%s", combined);
        }
    }

    hot_ui_show_error(err);
    snprintf(s.error, sizeof(s.error), "%s", err);
    hot_ui_set_error(error, error_size, err);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  命令队列                                                           */
/* ------------------------------------------------------------------ */

static bool hot_ui_process_request(void)
{
    char *code;
    size_t len;
    char name[HOT_UI_NAME_MAX];
    char title[HOT_UI_TITLE_MAX];
    char err[HOT_UI_ERROR_MAX] = {0};
    int rc;

    hot_ui_lock();
    if (!s_req.pending || s_req.processing) {
        hot_ui_unlock();
        return false;
    }
    s_req.processing = true;
    s_req.pending = false;
    code = s_req.code;
    s_req.code = NULL;
    len = s_req.len;
    snprintf(name, sizeof(name), "%s", s_req.name);
    snprintf(title, sizeof(title), "%s", s_req.title);
    hot_ui_unlock();

    if (code && len > 0) {
        rc = hot_ui_install(code, len, name, title, true,
                            err, sizeof(err));
    } else {
        rc = -1;
        snprintf(err, sizeof(err), "请求内容为空");
    }
    claw_free(code);

    /* 首次运行就编译失败时没有旧画面可保留，直接显示错误页。 */
    if (rc != 0 && !s.root && s.state != HOT_UI_STATE_ERROR) {
        hot_ui_show_error(err[0] ? err : "脚本运行失败");
    }

    hot_ui_lock();
    s_req.result = rc;
    snprintf(s_req.error, sizeof(s_req.error), "%s", err);
    s_req.done = true;
    if (s_req.waiting) {
        claw_sem_give(s.done);
    } else {
        s_req.done = false;
    }
    s_req.processing = false;
    hot_ui_unlock();
    return true;
}

static void hot_ui_cancel_pending(const char *reason)
{
    bool wake = false;

    hot_ui_lock();
    if (s_req.pending) {
        claw_free(s_req.code);
        s_req.code = NULL;
        s_req.pending = false;
        s_req.result = -1;
        snprintf(s_req.error, sizeof(s_req.error), "%s",
                 (reason && reason[0]) ? reason : "请求已取消");
        if (s_req.waiting) {
            s_req.done = true;
            wake = true;
        }
    }
    hot_ui_unlock();

    if (wake) {
        claw_sem_give(s.done);
    }
}

static void hot_ui_ensure_app(void)
{
    static char app_id[] = HOT_UI_APP_ID;

    if (!gui_app_is_actived(app_id)) {
        s.last_start_tick = rt_tick_get();
        gui_app_run(HOT_UI_APP_ID);
    }
}

/* 脚本缓冲区延迟到首次提交时分配，确保 rt-claw 的 PSRAM 堆已就绪。 */
static int hot_ui_ensure_buffers(void)
{
    int rc = 0;

    if (s.script && s.last_good) {
        return 0;
    }

    hot_ui_lock();
    if (!s.script) {
        s.script = claw_malloc(HOT_UI_SCRIPT_MAX + 1);
    }
    if (!s.last_good) {
        s.last_good = claw_malloc(HOT_UI_SCRIPT_MAX + 1);
    }
    if (!s.script || !s.last_good) {
        rc = -1;
    }
    hot_ui_unlock();
    return rc;
}

static void hot_ui_dispatch_cb(lv_timer_t *timer)
{
    bool pending;
    uint32_t submit_tick;
    uint32_t now;

    (void)timer;
    if (s.app_active) {
        hot_ui_process_request();
        return;
    }

    hot_ui_lock();
    pending = s_req.pending;
    submit_tick = s_req.submit_tick;
    hot_ui_unlock();
    if (!pending) {
        return;
    }

    now = rt_tick_get();
    if ((uint32_t)(now - submit_tick) >
        rt_tick_from_millisecond(HOT_UI_REQUEST_TTL_MS)) {
        hot_ui_cancel_pending("HotUI 启动超时");
        return;
    }
    if ((uint32_t)(now - s.last_start_tick) >
        rt_tick_from_millisecond(HOT_UI_START_RETRY_MS)) {
        s.last_start_tick = now;
        gui_app_run(HOT_UI_APP_ID);
    }
}

int hot_ui_submit(const char *name, const char *title,
                  const char *code, size_t len,
                  int timeout_ms, char *error, size_t error_size)
{
    char *copy;
    char err[HOT_UI_ERROR_MAX] = {0};
    bool have_result = false;
    int rc;

    hot_ui_set_error(error, error_size, "");
    if (!s.ready) {
        hot_ui_set_error(error, error_size, "HotUI 尚未初始化");
        return -1;
    }
    if (!code || len == 0 || len > HOT_UI_SCRIPT_MAX) {
        hot_ui_set_error(error, error_size,
                         "脚本长度必须在 1..16384 字节之间");
        return -1;
    }
    if (hot_ui_ensure_buffers() != 0) {
        hot_ui_set_error(error, error_size, "脚本缓冲区分配失败");
        return -1;
    }
    if (!hot_ui_name_valid(name)) {
        hot_ui_set_error(error, error_size,
                         "name 只能包含字母、数字、_、-，且不能超过 31 字节");
        return -1;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }

    copy = claw_malloc(len + 1);
    if (!copy) {
        hot_ui_set_error(error, error_size, "内存不足");
        return -1;
    }
    memcpy(copy, code, len);
    copy[len] = '\0';

    hot_ui_lock();
    if (s_req.pending || s_req.done || s_req.processing) {
        hot_ui_unlock();
        claw_free(copy);
        hot_ui_set_error(error, error_size, "HotUI 正忙，请稍后再试");
        return -2;
    }

    /* 清掉可能残留的完成信号，避免本次请求误判为已完成。 */
    while (claw_sem_take(s.done, 0) == CLAW_OK) {
    }
    s_req.code = copy;
    s_req.len = len;
    snprintf(s_req.name, sizeof(s_req.name), "%s", name);
    snprintf(s_req.title, sizeof(s_req.title), "%s",
             (title && title[0]) ? title : name);
    s_req.pending = true;
    s_req.done = false;
    s_req.waiting = true;
    s_req.processing = false;
    s_req.result = -1;
    s_req.error[0] = '\0';
    s_req.submit_tick = rt_tick_get();
    hot_ui_unlock();

    hot_ui_ensure_app();

    if (claw_sem_take(s.done, (uint32_t)timeout_ms) != CLAW_OK) {
        hot_ui_lock();
        s_req.waiting = false;
        if (s_req.done) {
            rc = s_req.result;
            snprintf(err, sizeof(err), "%s", s_req.error);
            s_req.done = false;
            have_result = true;
        }
        hot_ui_unlock();

        if (have_result) {
            while (claw_sem_take(s.done, 0) == CLAW_OK) {
            }
            hot_ui_set_error(error, error_size, err);
            return rc;
        }
        hot_ui_set_error(error, error_size, "queued");
        return 1;
    }

    hot_ui_lock();
    s_req.waiting = false;
    rc = s_req.result;
    snprintf(err, sizeof(err), "%s", s_req.error);
    s_req.done = false;
    hot_ui_unlock();

    hot_ui_set_error(error, error_size, err);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  App 生命周期                                                       */
/* ------------------------------------------------------------------ */

static void hot_ui_show_message(const char *title, const char *body,
                                uint32_t title_color, const char *hint)
{
    lv_obj_t *label;

    hot_ui_teardown();
    if (hot_ui_create_root() != 0) {
        return;
    }

    label = lv_label_create(s.root);
    if (label) {
        lv_label_set_text(label, title);
        lv_obj_set_pos(label, 60, 150);
        lv_obj_set_width(label, 346);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(title_color), 0);
        lv_obj_set_style_text_font(label, hot_ui_font_for_size(28), 0);
    }

    label = lv_label_create(s.root);
    if (label) {
        lv_label_set_text(label, body);
        lv_obj_set_pos(label, 60, 210);
        lv_obj_set_width(label, 346);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, hot_ui_font_for_size(18), 0);
    }

    if (hint && hint[0]) {
        label = lv_label_create(s.root);
        if (label) {
            lv_label_set_text(label, hint);
            lv_obj_set_pos(label, 60, 320);
            lv_obj_set_width(label, 346);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0x8E8E93), 0);
            lv_obj_set_style_text_font(label, hot_ui_font_for_size(16), 0);
        }
    }
}

static void hot_ui_show_idle(void)
{
    hot_ui_show_message("SF-Claw HotUI", "等待 AI 生成界面",
                        0xFFFFFF, "在微信里描述你想要的功能");
    s.state = HOT_UI_STATE_IDLE;
    s.error[0] = '\0';
}

static void hot_ui_show_error(const char *text)
{
    char message[HOT_UI_ERROR_MAX];

    snprintf(message, sizeof(message), "%s",
             (text && text[0]) ? text : "未知脚本错误");
    hot_ui_show_message("脚本运行失败", message, 0xFF453A,
                        "请让 SF-Claw 重新生成");
    s.state = HOT_UI_STATE_ERROR;
    snprintf(s.error, sizeof(s.error), "%s", message);
}

static void hot_ui_app_start(void)
{
    char err[HOT_UI_ERROR_MAX] = {0};

    s.app_active = true;

    /* 微信请求可能先于 App 启动到达，优先处理队列里的脚本。 */
    if (hot_ui_process_request()) {
        return;
    }
    if (s.script_len > 0) {
        if (hot_ui_install(s.script, s.script_len, s.name, s.title,
                           false, err, sizeof(err)) != 0 &&
            !s.root && s.state != HOT_UI_STATE_ERROR) {
            hot_ui_show_error(err[0] ? err : "脚本运行失败");
        }
        return;
    }

    hot_ui_show_idle();
}

static void hot_ui_msg_handler(gui_app_msg_type_t msg, void *param)
{
    (void)param;

    switch (msg) {
    case GUI_APP_MSG_ONSTART:
        hot_ui_app_start();
        break;
    case GUI_APP_MSG_ONRESUME:
        s.paused = false;
        if (s.tick_timer) {
            lv_timer_resume(s.tick_timer);
        }
        break;
    case GUI_APP_MSG_ONPAUSE:
        s.paused = true;
        if (s.tick_timer) {
            lv_timer_pause(s.tick_timer);
        }
        break;
    case GUI_APP_MSG_ONSTOP:
        s.app_active = false;
        hot_ui_teardown();
        s.state = HOT_UI_STATE_IDLE;
        break;
    default:
        break;
    }
}

static int app_hot_ui(intent_t intent)
{
    (void)intent;
    gui_app_regist_msg_handler(HOT_UI_APP_ID, hot_ui_msg_handler);
    return 0;
}

/* HotUI 不在主菜单显示，由 AI 工具或串口命令启动。 */
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(chat), NULL, HOT_UI_APP_ID, app_hot_ui, 1);

/* ------------------------------------------------------------------ */
/*  初始化与串口调试                                                   */
/* ------------------------------------------------------------------ */

int hot_ui_init(void)
{
    if (s.ready) {
        return 0;
    }

    memset(&s, 0, sizeof(s));
    s.lock = claw_mutex_create("hotui_lock");
    s.done = claw_sem_create("hotui_done", 0);
    if (!s.lock || !s.done) {
        goto fail;
    }

    hot_ui_api_set_error_cb(hot_ui_on_js_error);
    hot_ui_api_reset();    /* 把所有 JSValue 初始化为 undefined */

    s.dispatch_timer = lv_timer_create(hot_ui_dispatch_cb,
                                      HOT_UI_DISPATCH_MS, NULL);
    if (!s.dispatch_timer) {
        goto fail;
    }

    s.state = HOT_UI_STATE_IDLE;
    s.ready = true;
    CLAW_LOGI(TAG, "ready");
    return 0;

fail:
    if (s.dispatch_timer) {
        lv_timer_del(s.dispatch_timer);
        s.dispatch_timer = NULL;
    }
    if (s.done) {
        claw_sem_delete(s.done);
        s.done = NULL;
    }
    if (s.lock) {
        claw_mutex_delete(s.lock);
        s.lock = NULL;
    }
    claw_free(s.script);
    s.script = NULL;
    claw_free(s.last_good);
    s.last_good = NULL;
    CLAW_LOGE(TAG, "init failed");
    return -1;
}

static void hot_ui_cmd_status(void)
{
    if (!s.ready) {
        rt_kprintf("hotui: not initialized\n");
        return;
    }

    hot_ui_lock();
    rt_kprintf("hotui: state=%s app=%d script=%u name=%s title=%s "
               "tick=%d touch=%d\n",
               hot_ui_state_name(s.state), s.app_active ? 1 : 0,
               (unsigned)s.script_len, s.name, s.title,
               s.has_tick ? 1 : 0, s.has_touch ? 1 : 0);
    if (s.error[0]) {
        rt_kprintf("hotui: last error: %s\n", s.error);
    }
    hot_ui_unlock();
}

static void hot_ui_cmd_demo(const char *kind)
{
    static const char demo_static[] =
        "function on_start() {\n"
        "  ui.clear(0x101018);\n"
        "  ui.text(120, 100, \"设备状态\", 0xFFFFFF, 28);\n"
        "  ui.rect(80, 160, 306, 64, 0x1C1C22);\n"
        "  ui.text(100, 178, \"电量 82%\", 0x30D158, 22);\n"
        "  ui.text(100, 205, \"步数 3200\", 0xFFFFFF, 18);\n"
        "  ui.rect(80, 240, 306, 64, 0x1C1C22);\n"
        "  ui.text(100, 258, \"心率 72 bpm\", 0xFF9F9F, 22);\n"
        "}\n";
    static const char demo_timer[] =
        "let remain = 25 * 60, running = false, label;\n"
        "function on_start() {\n"
        "  ui.clear(0x000000);\n"
        "  ui.text(150, 100, \"番茄钟\", 0xFFFFFF, 28);\n"
        "  label = ui.text(175, 180, \"25:00\", 0x30D158, 36);\n"
        "  ui.button(160, 300, 146, 48, \"开始\", function () {\n"
        "    running = !running;\n"
        "  });\n"
        "}\n"
        "function on_tick(dt) {\n"
        "  if (!running) return;\n"
        "  remain -= dt / 1000;\n"
        "  if (remain < 0) remain = 0;\n"
        "  let m = Math.floor(remain / 60);\n"
        "  let s = Math.floor(remain % 60);\n"
        "  ui.setText(label, m + \":\" + (s < 10 ? \"0\" : \"\") + s);\n"
        "}\n";
    static const char demo_watch[] =
        "let hourHand, minuteHand, secondHand, secondArc, clockText, dateText;\n"
        "let lastSecond = -1;\n"
        "function pad2(n) { return n < 10 ? \"0\" + n : \"\" + n; }\n"
        "function drawHand(id, degrees, len) {\n"
        "  let a = (degrees - 90) * Math.PI / 180;\n"
        "  ui.setLine(id, 233, 233,\n"
        "    Math.round(233 + Math.cos(a) * len),\n"
        "    Math.round(233 + Math.sin(a) * len));\n"
        "}\n"
        "function on_start() {\n"
        "  ui.clear(0x06080C);\n"
        "  ui.arc(233, 233, 214, 0, 360, 0x1B2A3A, 4, 0x0B1220);\n"
        "  secondArc = ui.arc(233, 233, 202, 0, 1, 0x00E5FF, 3, 0x11202C);\n"
        "  ui.circle(233, 233, 188, 0x0B1220);\n"
        "  ui.circle(233, 233, 186, 0x101826);\n"
        "  for (let i = 0; i < 12; i++) {\n"
        "    let a = (i * 30 - 90) * Math.PI / 180;\n"
        "    let r1 = i % 3 == 0 ? 172 : 178;\n"
        "    ui.line(Math.round(233 + Math.cos(a) * r1),\n"
        "      Math.round(233 + Math.sin(a) * r1),\n"
        "      Math.round(233 + Math.cos(a) * 186),\n"
        "      Math.round(233 + Math.sin(a) * 186),\n"
        "      i % 3 == 0 ? 0xFFFFFF : 0x3A4A5A, i % 3 == 0 ? 3 : 1);\n"
        "  }\n"
        "  ui.text(60, 96, \"SF-CLAW\", 0x8BE9FD, 18, 346, 1);\n"
        "  ui.text(60, 124, \"SMART WATCH\", 0x5A6A7A, 12, 346, 1);\n"
        "  hourHand = ui.line(233, 233, 233, 155, 0xFFFFFF, 8);\n"
        "  minuteHand = ui.line(233, 233, 233, 125, 0x8BE9FD, 5);\n"
        "  secondHand = ui.line(233, 233, 233, 108, 0xFF453A, 2);\n"
        "  ui.circle(233, 233, 7, 0xFF453A);\n"
        "  clockText = ui.text(60, 286, \"00:00:00\", 0xFFFFFF, 36, 346, 1);\n"
        "  dateText = ui.text(60, 340, \"---- -- --\", 0x8E8E93, 16, 346, 1);\n"
        "}\n"
        "function on_tick(dt) {\n"
        "  let d = new Date();\n"
        "  let h = d.getHours() % 12, m = d.getMinutes(), s = d.getSeconds();\n"
        "  drawHand(hourHand, (h + m / 60) * 30, 78);\n"
        "  drawHand(minuteHand, (m + s / 60) * 6, 105);\n"
        "  drawHand(secondHand, s * 6, 122);\n"
        "  ui.setArc(secondArc, 0, s * 6);\n"
        "  if (s != lastSecond) {\n"
        "    lastSecond = s;\n"
        "    ui.setText(clockText, pad2(d.getHours()) + \":\" + pad2(m) + \":\" + pad2(s));\n"
        "    ui.setText(dateText, d.getFullYear() + \"-\" + pad2(d.getMonth() + 1) + \"-\" + pad2(d.getDate()));\n"
        "  }\n"
        "}\n";
    static const char demo_breakout[] =
        "let paddle, ball;\n"
        "let bx = 230, by = 300, vx = 120, vy = -140, px = 180;\n"
        "function on_start() {\n"
        "  ui.clear(0x000000);\n"
        "  paddle = ui.rect(px, 400, 90, 12, 0x30D158);\n"
        "  ball = ui.rect(bx, by, 12, 12, 0xFFFFFF);\n"
        "}\n"
        "function on_tick(dt) {\n"
        "  bx += vx * dt / 1000;\n"
        "  by += vy * dt / 1000;\n"
        "  if (bx < 60 || bx > 394) vx = -vx;\n"
        "  if (by < 100) vy = -vy;\n"
        "  if (by > 430) { by = 300; vy = -140; }\n"
        "  ui.move(ball, bx, by);\n"
        "}\n"
        "function on_touch(x, y, down) {\n"
        "  if (!down) return;\n"
        "  px = x - 45;\n"
        "  if (px < 60) px = 60;\n"
        "  if (px > 316) px = 316;\n"
        "  ui.move(paddle, px, 400);\n"
        "}\n";
    static const char demo_2048[] =
        "let board, cells = [], labels = [], score = 0, scoreLabel;\n"
        "let gameOver = false, overIds = [];\n"
        "const N = 4, CELL = 56, GAP = 6, X = 112, Y = 142;\n"
        "function cellX(c) { return X + c * (CELL + GAP); }\n"
        "function cellY(r) { return Y + r * (CELL + GAP); }\n"
        "function colorFor(v) {\n"
        "  if (v == 0) return 0x1C1C1E;\n"
        "  if (v == 2) return 0x2D6CDF;\n"
        "  if (v == 4) return 0x2F8FEF;\n"
        "  if (v == 8) return 0x18B6A4;\n"
        "  if (v == 16) return 0x21C55D;\n"
        "  if (v == 32) return 0x9ACD32;\n"
        "  if (v == 64) return 0xF5A623;\n"
        "  if (v == 128) return 0xF97316;\n"
        "  if (v == 256) return 0xEAB308;\n"
        "  if (v == 512) return 0xA855F7;\n"
        "  if (v == 1024) return 0xEC4899;\n"
        "  return 0xFFD700;\n"
        "}\n"
        "function render() {\n"
        "  for (let r = 0; r < N; r++) {\n"
        "    for (let c = 0; c < N; c++) {\n"
        "      let v = board[r][c];\n"
        "      let i = r * N + c;\n"
        "      ui.setColor(cells[i], colorFor(v));\n"
        "      ui.setText(labels[i], v ? \"\" + v : \"\");\n"
        "    }\n"
        "  }\n"
        "  ui.setText(scoreLabel, \"分数 \" + score);\n"
        "}\n"
        "function addRandom() {\n"
        "  let empty = [];\n"
        "  for (let r = 0; r < N; r++)\n"
        "    for (let c = 0; c < N; c++)\n"
        "      if (!board[r][c]) empty.push([r, c]);\n"
        "  if (!empty.length) return;\n"
        "  let p = empty[Math.floor(Math.random() * empty.length)];\n"
        "  board[p[0]][p[1]] = Math.random() < 0.9 ? 2 : 4;\n"
        "}\n"
        "function slideRow(row) {\n"
        "  let a = row.filter(function (v) { return v; });\n"
        "  let out = [];\n"
        "  for (let i = 0; i < a.length; i++) {\n"
        "    if (i + 1 < a.length && a[i] == a[i + 1]) {\n"
        "      let v = a[i] * 2;\n"
        "      out.push(v);\n"
        "      score += v;\n"
        "      i++;\n"
        "    } else {\n"
        "      out.push(a[i]);\n"
        "    }\n"
        "  }\n"
        "  while (out.length < N) out.push(0);\n"
        "  return out;\n"
        "}\n"
        "function transpose(b) {\n"
        "  let out = [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]];\n"
        "  for (let r = 0; r < N; r++)\n"
        "    for (let c = 0; c < N; c++) out[c][r] = b[r][c];\n"
        "  return out;\n"
        "}\n"
        "function slideRight(row) {\n"
        "  return slideRow(row.slice().reverse()).reverse();\n"
        "}\n"
        "function canMove() {\n"
        "  for (let r = 0; r < N; r++) {\n"
        "    for (let c = 0; c < N; c++) {\n"
        "      if (!board[r][c]) return true;\n"
        "      if (r + 1 < N && board[r][c] == board[r + 1][c]) return true;\n"
        "      if (c + 1 < N && board[r][c] == board[r][c + 1]) return true;\n"
        "    }\n"
        "  }\n"
        "  return false;\n"
        "}\n"
        "function clearOver() {\n"
        "  for (let i = 0; i < overIds.length; i++) ui.remove(overIds[i]);\n"
        "  overIds = [];\n"
        "}\n"
        "function restart() {\n"
        "  clearOver();\n"
        "  resetBoard();\n"
        "}\n"
        "function showOver() {\n"
        "  gameOver = true;\n"
        "  overIds.push(ui.rect(120, 210, 226, 150, 0x050608, 18));\n"
        "  overIds.push(ui.text(120, 230, \"GAME OVER\", 0xFF453A, 24, 226, 1));\n"
        "  overIds.push(ui.button(160, 300, 146, 44, \"再来一局\", restart));\n"
        "}\n"
        "function move(dir) {\n"
        "  if (gameOver) return;\n"
        "  let before = JSON.stringify(board);\n"
        "  if (dir == \"left\") board = board.map(slideRow);\n"
        "  else if (dir == \"right\") board = board.map(slideRight);\n"
        "  else {\n"
        "    board = transpose(board);\n"
        "    board = dir == \"up\" ? board.map(slideRow) : board.map(slideRight);\n"
        "    board = transpose(board);\n"
        "  }\n"
        "  if (JSON.stringify(board) == before) return;\n"
        "  addRandom();\n"
        "  render();\n"
        "  if (!canMove()) showOver();\n"
        "}\n"
        "function resetBoard() {\n"
        "  board = [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]];\n"
        "  score = 0;\n"
        "  gameOver = false;\n"
        "  addRandom();\n"
        "  addRandom();\n"
        "  render();\n"
        "}\n"
        "function on_start() {\n"
        "  ui.clear(0x080A0F);\n"
        "  ui.text(60, 76, \"2048\", 0x00E5FF, 36, 346, 1);\n"
        "  scoreLabel = ui.text(60, 118, \"分数 0\", 0x8E8E93, 16, 346, 1);\n"
        "  ui.rect(104, 134, 258, 258, 0x141821, 18);\n"
        "  for (let r = 0; r < N; r++) {\n"
        "    for (let c = 0; c < N; c++) {\n"
        "      let x = cellX(c), y = cellY(r);\n"
        "      cells.push(ui.rect(x, y, CELL, CELL, 0x1C1C1E, 10));\n"
        "      labels.push(ui.text(x, y + 17, \"\", 0xFFFFFF, 22, CELL, 1));\n"
        "    }\n"
        "  }\n"
        "  ui.text(133, 402, \"滑动屏幕操作\", 0x5A6A7A, 14, 200, 1);\n"
        "  ui.onSwipe(function (dx, dy) {\n"
        "    if (Math.abs(dx) > Math.abs(dy)) move(dx > 0 ? \"right\" : \"left\");\n"
        "    else move(dy > 0 ? \"down\" : \"up\");\n"
        "  });\n"
        "  resetBoard();\n"
        "}\n";
    const char *code;
    const char *name;
    const char *title;
    char error[HOT_UI_ERROR_MAX] = {0};
    int rc;

    if (!strcmp(kind, "static")) {
        code = demo_static;
        name = "demo_static";
        title = "设备状态";
    } else if (!strcmp(kind, "timer")) {
        code = demo_timer;
        name = "demo_timer";
        title = "番茄钟";
    } else if (!strcmp(kind, "breakout")) {
        code = demo_breakout;
        name = "demo_breakout";
        title = "打砖块";
    } else if (!strcmp(kind, "watch")) {
        code = demo_watch;
        name = "demo_watch";
        title = "表盘";
    } else if (!strcmp(kind, "2048")) {
        code = demo_2048;
        name = "demo_2048";
        title = "2048";
    } else {
        rt_kprintf("hotui: demo 需要 static、timer、breakout、watch 或 2048\n");
        return;
    }

    rc = hot_ui_submit(name, title, code, strlen(code), 3000,
                       error, sizeof(error));
    if (rc == 0) {
        rt_kprintf("hotui: demo %s running\n", kind);
    } else if (rc == 1) {
        rt_kprintf("hotui: demo %s queued\n", kind);
    } else {
        rt_kprintf("hotui: demo %s failed: %s\n", kind,
                   error[0] ? error : "unknown");
    }
}

static int hot_ui_cmd(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "status";

    if (!strcmp(sub, "status")) {
        hot_ui_cmd_status();
    } else if (!strcmp(sub, "stop")) {
        hot_ui_cancel_pending("已停止");
        gui_app_exit(HOT_UI_APP_ID);
        rt_kprintf("hotui: stop requested\n");
    } else if (!strcmp(sub, "run") || !strcmp(sub, "reload")) {
        char *copy;
        size_t len;
        char name[HOT_UI_NAME_MAX];
        char title[HOT_UI_TITLE_MAX];
        char error[HOT_UI_ERROR_MAX] = {0};
        int rc;

        if (!s.ready) {
            rt_kprintf("hotui: not initialized\n");
            return 0;
        }
        hot_ui_lock();
        len = s.script_len;
        snprintf(name, sizeof(name), "%s", s.name);
        snprintf(title, sizeof(title), "%s", s.title);
        copy = len ? claw_malloc(len + 1) : NULL;
        if (copy) {
            memcpy(copy, s.script, len + 1);
        }
        hot_ui_unlock();

        if (!copy) {
            rt_kprintf("hotui: no script to run\n");
            return 0;
        }
        rc = hot_ui_submit(name, title, copy, len, 3000,
                           error, sizeof(error));
        claw_free(copy);
        rt_kprintf("hotui: run -> %d%s%s\n", rc,
                   error[0] ? ", " : "", error);
    } else if (!strcmp(sub, "demo")) {
        hot_ui_cmd_demo((argc > 2) ? argv[2] : "static");
    } else {
        rt_kprintf("usage: hot_ui status|stop|run|reload|"
                   "demo static|timer|breakout|watch|2048\n");
    }
    return 0;
}

MSH_CMD_EXPORT_ALIAS(hot_ui_cmd, hot_ui, "HotUI status/stop/run/demo");
