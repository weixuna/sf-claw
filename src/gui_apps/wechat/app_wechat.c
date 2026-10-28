/**
 * @file app_wechat.c
 * @brief WeChat ClawBot screen: renders the login QR and connection state.
 *
 * The iLink service runs in its own thread.  This app only polls a
 * thread-safe status snapshot and never touches the network itself.
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "bf0_hal.h"

#include "lvgl.h"
#include "lv_canvas.h"
#include "qrcodegen.h"
#include "lv_ext_resource_manager.h"
#include "lvsf_font.h"
#include "gui_app_fwk.h"
#include "osal/claw_os.h"

#include "wechat_ilink.h"

#define APP_ID "WeChat"

/* 466x466 round panel: keep everything inside the visible circle. */
#define WX_TITLE_Y       52
#define WX_STATUS_Y      80
#define WX_CARD_Y        100
#define WX_CARD_SIZE     248
#define WX_QR_SIZE       220
#define WX_STATS_Y       358
#define WX_LAST_Y        376
#define WX_BTN_BOTTOM    (-32)
#define WX_BTN_W         106
#define WX_BTN_H         34
#define WX_CONTENT_W     300

LV_IMG_DECLARE(img_messages);

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *title;
    lv_obj_t *status;
    lv_obj_t *card;
    lv_obj_t *qr;
    uint8_t *qr_buf;
    size_t qr_buf_size;
    lv_obj_t *qr_hint;
    lv_obj_t *stats;
    lv_obj_t *last;
    lv_obj_t *primary;
    lv_obj_t *primary_label;
    lv_obj_t *secondary;
    lv_timer_t *timer;

    char last_qr[WECHAT_ILINK_QR_MAX];
    bool qr_visible;
    bool auto_login_done;
    int dbg_state;
    int dbg_qr_len;
} wx_ctx_t;

static wx_ctx_t *s_wx_ui;

static lv_font_t *wx_font(void)
{
    return (lv_font_t *)LV_EXT_FONT_GET(LVSF_FONT_NORMAL);
}

static lv_font_t *wx_font_small(void)
{
    return (lv_font_t *)LV_EXT_FONT_GET(LVSF_FONT_SMALL);
}

static void wx_label_set(lv_obj_t *label, const char *text)
{
    if (label && text && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static bool wx_draw_qr(wx_ctx_t *ctx, const char *payload)
{
    uint8_t *temp;
    uint8_t *qr;
    lv_color_t *pixels;
    int size;
    int scale;
    int total;
    int margin;
    int x;
    int y;

    if (!ctx || !ctx->qr || !ctx->qr_buf || !payload || !payload[0]) {
        return false;
    }

    temp = claw_malloc(qrcodegen_BUFFER_LEN_MAX);
    qr = claw_malloc(qrcodegen_BUFFER_LEN_MAX);
    if (!temp || !qr) {
        claw_free(temp);
        claw_free(qr);
        return false;
    }

    if (!qrcodegen_encodeText(payload, temp, qr, qrcodegen_Ecc_MEDIUM,
                              1, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        claw_free(temp);
        claw_free(qr);
        return false;
    }

    size = qrcodegen_getSize(qr);
    if (size <= 0) {
        claw_free(temp);
        claw_free(qr);
        return false;
    }
    scale = WX_QR_SIZE / size;
    if (scale < 1) {
        claw_free(temp);
        claw_free(qr);
        return false;
    }

    total = size * scale;
    margin = (WX_QR_SIZE - total) / 2;
    pixels = (lv_color_t *)ctx->qr_buf;

    for (x = 0; x < WX_QR_SIZE * WX_QR_SIZE; x++) {
        pixels[x] = lv_color_white();
    }
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            if (qrcodegen_getModule(qr, x, y)) {
                int px = margin + x * scale;
                int py = margin + y * scale;
                int dy;

                for (dy = 0; dy < scale; dy++) {
                    int dx;

                    for (dx = 0; dx < scale; dx++) {
                        pixels[(size_t)(py + dy) * WX_QR_SIZE + px + dx] =
                            lv_color_black();
                    }
                }
            }
        }
    }

    {
        uintptr_t start = (uintptr_t)ctx->qr_buf & ~(uintptr_t)31;
        uintptr_t end = ((uintptr_t)ctx->qr_buf + ctx->qr_buf_size + 31) &
                        ~(uintptr_t)31;

        SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    }
    lv_img_cache_invalidate_src(lv_canvas_get_img(ctx->qr));
    lv_obj_invalidate(ctx->qr);

    claw_free(temp);
    claw_free(qr);
    rt_kprintf("[wechat_ui] QR drawn: modules=%d scale=%d margin=%d\n",
               size, scale, margin);
    return true;
}

static void wx_show_qr(wx_ctx_t *ctx, bool show)
{
    if (!ctx || !ctx->qr) {
        return;
    }
    if (show == ctx->qr_visible) {
        return;
    }
    ctx->qr_visible = show;
    if (show) {
        lv_obj_clear_flag(ctx->qr, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->qr, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wx_set_card_hint(wx_ctx_t *ctx, const char *text,
                             lv_color_t color)
{
    if (!ctx || !ctx->qr_hint) {
        return;
    }
    wx_label_set(ctx->qr_hint, text);
    lv_obj_set_style_text_color(ctx->qr_hint, color, 0);
}

static void wx_refresh(wx_ctx_t *ctx)
{
    wechat_ilink_status_t st;
    bool waiting;
    bool connected;
    char buf[256];

    if (!ctx) {
        return;
    }

    wechat_ilink_get_status(&st);

    if ((int)st.state != ctx->dbg_state ||
        (int)strlen(st.qr) != ctx->dbg_qr_len) {
        ctx->dbg_state = st.state;
        ctx->dbg_qr_len = (int)strlen(st.qr);
        rt_kprintf("[wechat_ui] state=%d ready=%d configured=%d qr_len=%d qr=%p\n",
                   st.state, st.service_ready, st.configured,
                   ctx->dbg_qr_len, (void *)ctx->qr);
    }

    if (!st.service_ready) {
        wx_label_set(ctx->title, "微信连接");
        wx_label_set(ctx->status, "正在启动 SF-Claw…");
        lv_obj_set_style_text_color(ctx->status, lv_color_hex(0x8E8E93), 0);

        ctx->last_qr[0] = '\0';
        wx_show_qr(ctx, false);
        lv_obj_clear_flag(ctx->qr_hint, LV_OBJ_FLAG_HIDDEN);
        wx_set_card_hint(ctx, "SF-Claw\n正在启动…", lv_color_hex(0x555555));
        wx_label_set(ctx->stats, "等待 SF-Claw 初始化");
        wx_label_set(ctx->last, "启动后会自动生成微信二维码");
        wx_label_set(ctx->primary_label, "请稍候");
        lv_obj_add_state(ctx->primary, LV_STATE_DISABLED);
        lv_obj_add_flag(ctx->secondary, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_state(ctx->primary, LV_STATE_DISABLED);
    connected = (st.state == WECHAT_ILINK_CONNECTED);
    waiting = (st.state == WECHAT_ILINK_WAIT_SCAN ||
               st.state == WECHAT_ILINK_SCANNED);

    wx_label_set(ctx->title, connected ? "微信已连接" : "微信连接");
    wx_label_set(ctx->status, st.status[0] ? st.status : "准备中…");
    lv_obj_set_style_text_color(
        ctx->status,
        (st.state == WECHAT_ILINK_ERROR) ? lv_color_hex(0xFF7A7A) :
        !st.online ? lv_color_hex(0xFFD60A) :
        connected ? lv_color_hex(0x30D158) : lv_color_hex(0xB0B0B0),
        0);

    if (waiting && st.qr[0] && ctx->qr) {
        if (strcmp(ctx->last_qr, st.qr) != 0) {
            if (wx_draw_qr(ctx, st.qr)) {
                strncpy(ctx->last_qr, st.qr, sizeof(ctx->last_qr) - 1);
                ctx->last_qr[sizeof(ctx->last_qr) - 1] = '\0';
                wx_show_qr(ctx, true);
                lv_obj_move_foreground(ctx->qr);
                lv_obj_add_flag(ctx->qr_hint, LV_OBJ_FLAG_HIDDEN);
            } else {
                rt_kprintf("[wechat_ui] QR draw failed\n");
                ctx->last_qr[0] = '\0';
                wx_show_qr(ctx, false);
                wx_set_card_hint(ctx, "二维码生成失败\n请点下方重新生成",
                                 lv_color_hex(0xB3261E));
                lv_obj_clear_flag(ctx->qr_hint, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        ctx->last_qr[0] = '\0';
        wx_show_qr(ctx, false);
        lv_obj_clear_flag(ctx->qr_hint, LV_OBJ_FLAG_HIDDEN);

        if (st.state == WECHAT_ILINK_WAIT_SCAN) {
            wx_set_card_hint(ctx, "正在获取\n二维码…", lv_color_hex(0x555555));
        } else if (st.state == WECHAT_ILINK_SCANNED) {
            wx_set_card_hint(ctx, "已扫码\n请在手机上确认",
                             lv_color_hex(0x1B7F3A));
        } else if (connected) {
            wx_set_card_hint(ctx, "已连接\n\n微信消息由\nSF-Claw 自动回复",
                             lv_color_hex(0x1B7F3A));
        } else if (st.state == WECHAT_ILINK_ERROR) {
            wx_set_card_hint(ctx, "连接失败\n\n请检查手机蓝牙共享\n后重新生成",
                             lv_color_hex(0xB3261E));
        } else {
            wx_set_card_hint(ctx, "未连接\n\n点下方按钮\n生成微信二维码",
                             lv_color_hex(0x555555));
        }
    }

    if (waiting && ctx->qr_visible) {
        lv_obj_add_flag(ctx->qr_hint, LV_OBJ_FLAG_HIDDEN);
    }

    if (connected) {
        snprintf(buf, sizeof(buf), "收到 %u 条 · 回复 %u 条",
                 (unsigned)st.rx_count, (unsigned)st.tx_count);
    } else if (st.state == WECHAT_ILINK_ERROR) {
        snprintf(buf, sizeof(buf), "连接失败");
    } else if (!st.online) {
        snprintf(buf, sizeof(buf), "蓝牙 PAN 未联网");
    } else if (waiting) {
        snprintf(buf, sizeof(buf), "等待微信扫码");
    } else {
        snprintf(buf, sizeof(buf), "未连接");
    }
    wx_label_set(ctx->stats, buf);

    if (st.last_rx[0]) {
        snprintf(buf, sizeof(buf), "微信: %s", st.last_rx);
    } else if (st.last_tx[0]) {
        snprintf(buf, sizeof(buf), "SF-Claw: %s", st.last_tx);
    } else if (connected) {
        snprintf(buf, sizeof(buf), "在微信里给 ClawBot 发一条消息试试");
    } else if (st.state == WECHAT_ILINK_ERROR) {
        snprintf(buf, sizeof(buf), "连接失败，请点下方重新生成二维码");
    } else if (waiting) {
        snprintf(buf, sizeof(buf), "扫码后，微信消息会交给 SF-Claw");
    } else {
        snprintf(buf, sizeof(buf), "点下方按钮生成微信二维码");
    }
    wx_label_set(ctx->last, buf);

    if (connected) {
        wx_label_set(ctx->primary_label, "重新扫码");
        lv_obj_clear_flag(ctx->secondary, LV_OBJ_FLAG_HIDDEN);
    } else if (waiting) {
        wx_label_set(ctx->primary_label, "取消登录");
        lv_obj_add_flag(ctx->secondary, LV_OBJ_FLAG_HIDDEN);
    } else {
        wx_label_set(ctx->primary_label, "生成二维码");
        lv_obj_add_flag(ctx->secondary, LV_OBJ_FLAG_HIDDEN);
    }

}

static void wx_primary_event_cb(lv_event_t *e)
{
    wx_ctx_t *ctx = lv_event_get_user_data(e);
    wechat_ilink_status_t st;

    if (!ctx || lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    wechat_ilink_get_status(&st);

    if (st.state == WECHAT_ILINK_CONNECTED) {
        wechat_ilink_login_start(true);
    } else if (st.state == WECHAT_ILINK_WAIT_SCAN ||
               st.state == WECHAT_ILINK_SCANNED) {
        wechat_ilink_login_cancel();
    } else {
        wechat_ilink_login_start(false);
    }

    wx_refresh(ctx);
}

static void wx_secondary_event_cb(lv_event_t *e)
{
    wx_ctx_t *ctx = lv_event_get_user_data(e);

    if (!ctx || lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    wechat_ilink_logout();
    wx_refresh(ctx);
}

static void wx_timer_cb(lv_timer_t *timer)
{
    wx_ctx_t *ctx = timer ? timer->user_data : NULL;

    if (!ctx) {
        return;
    }

    wx_refresh(ctx);

    /*
     * Unlike on_start(), this runs after the AI/KV service has finished
     * loading a persisted token.  Auto-start only when the service is
     * ready and there is no saved login, so opening the app during boot
     * does not throw away a valid session.
     */
    if (!ctx->auto_login_done) {
        wechat_ilink_status_t st;

        wechat_ilink_get_status(&st);
        if (st.service_ready) {
            ctx->auto_login_done = true;
            if (st.state == WECHAT_ILINK_IDLE && !st.configured) {
                (void)wechat_ilink_login_start(false);
                wx_refresh(ctx);
            }
        } else {
            /* Retry if the first start raced with claw_init(). */
            (void)wechat_ilink_start();
        }
    }
}

static lv_obj_t *wx_make_button(lv_obj_t *parent, const char *text,
                                lv_coord_t x_ofs, lv_color_t color,
                                lv_event_cb_t cb, wx_ctx_t *ctx,
                                lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *label;

    lv_obj_set_size(btn, WX_BTN_W, WX_BTN_H);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x_ofs, WX_BTN_BOTTOM);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ctx);

    label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, wx_font_small(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    if (label_out) {
        *label_out = label;
    }
    return btn;
}

static void wx_build_ui(wx_ctx_t *ctx)
{
    lv_obj_t *scr = ctx->scr;

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ctx->title = lv_label_create(scr);
    lv_label_set_text(ctx->title, "微信连接");
    lv_obj_set_style_text_font(ctx->title, wx_font(), 0);
    lv_obj_set_style_text_color(ctx->title, lv_color_white(), 0);
    lv_obj_align(ctx->title, LV_ALIGN_TOP_MID, 0, WX_TITLE_Y);

    ctx->status = lv_label_create(scr);
    lv_label_set_text(ctx->status, "准备中…");
    lv_obj_set_width(ctx->status, 340);
    lv_label_set_long_mode(ctx->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ctx->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->status, wx_font_small(), 0);
    lv_obj_set_style_text_color(ctx->status, lv_color_hex(0xB0B0B0), 0);
    lv_obj_align(ctx->status, LV_ALIGN_TOP_MID, 0, WX_STATUS_Y);

    /* White card; the QR canvas is 1-bit so it is cheap in PSRAM/SRAM. */
    ctx->card = lv_obj_create(scr);
    lv_obj_set_size(ctx->card, WX_CARD_SIZE, WX_CARD_SIZE);
    lv_obj_align(ctx->card, LV_ALIGN_TOP_MID, 0, WX_CARD_Y);
    lv_obj_set_style_bg_color(ctx->card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ctx->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ctx->card, 0, 0);
    lv_obj_set_style_radius(ctx->card, 18, 0);
    lv_obj_set_style_pad_all(ctx->card, 0, 0);
    lv_obj_clear_flag(ctx->card, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * The Sifli EPIC GPU does not support LVGL's 1-bit indexed QR canvas
     * (lv_qrcode).  The L8 palette path also produced blue/broken output on
     * this panel, so use a plain RGB565 true-color canvas, the same format
     * used by every other image on the watch.
     */
    ctx->qr_buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(WX_QR_SIZE,
                                                     WX_QR_SIZE);
    ctx->qr_buf = claw_malloc(ctx->qr_buf_size);
    if (!ctx->qr_buf) {
        rt_kprintf("[wechat_ui] QR buffer alloc failed (%u bytes)\n",
                   (unsigned)ctx->qr_buf_size);
    } else {
        ctx->qr = lv_canvas_create(ctx->card);
        lv_canvas_set_buffer(ctx->qr, ctx->qr_buf, WX_QR_SIZE, WX_QR_SIZE,
                             LV_IMG_CF_TRUE_COLOR);
        memset(ctx->qr_buf, 0xFF, ctx->qr_buf_size);
        lv_obj_set_size(ctx->qr, WX_QR_SIZE, WX_QR_SIZE);
        lv_obj_center(ctx->qr);
        lv_obj_set_style_img_opa(ctx->qr, LV_OPA_COVER, 0);
        lv_obj_set_style_img_recolor_opa(ctx->qr, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(ctx->qr, LV_OBJ_FLAG_HIDDEN);
    }

    ctx->qr_hint = lv_label_create(ctx->card);
    lv_obj_set_width(ctx->qr_hint, 200);
    lv_label_set_long_mode(ctx->qr_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ctx->qr_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->qr_hint, wx_font(), 0);
    lv_obj_set_style_text_color(ctx->qr_hint, lv_color_hex(0x555555), 0);
    lv_label_set_text(ctx->qr_hint, "未连接\n\n点下方按钮\n生成微信二维码");
    lv_obj_center(ctx->qr_hint);

    ctx->stats = lv_label_create(scr);
    lv_obj_set_width(ctx->stats, WX_CONTENT_W);
    lv_label_set_long_mode(ctx->stats, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ctx->stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->stats, wx_font_small(), 0);
    lv_obj_set_style_text_color(ctx->stats, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(ctx->stats, LV_ALIGN_TOP_MID, 0, WX_STATS_Y);

    ctx->last = lv_label_create(scr);
    lv_obj_set_width(ctx->last, WX_CONTENT_W);
    lv_label_set_long_mode(ctx->last, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ctx->last, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->last, wx_font_small(), 0);
    lv_obj_set_style_text_color(ctx->last, lv_color_hex(0xB0B0B0), 0);
    lv_obj_align(ctx->last, LV_ALIGN_TOP_MID, 0, WX_LAST_Y);

    ctx->primary = wx_make_button(scr, "生成二维码", -54,
                                  lv_color_hex(0x0A84FF),
                                  wx_primary_event_cb, ctx,
                                  &ctx->primary_label);
    ctx->secondary = wx_make_button(scr, "断开", 54, lv_color_hex(0x3A3A3C),
                                    wx_secondary_event_cb, ctx, NULL);
    lv_obj_add_flag(ctx->secondary, LV_OBJ_FLAG_HIDDEN);
}

static void on_start(void)
{
    wx_ctx_t *ctx = rt_malloc(sizeof(*ctx));

    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->scr = lv_scr_act();
    ctx->auto_login_done = false;
    s_wx_ui = ctx;

    wx_build_ui(ctx);
    ctx->timer = lv_timer_create(wx_timer_cb, 500, ctx);

    (void)wechat_ilink_start();
    wx_refresh(ctx);
}

static void on_stop(void)
{
    if (s_wx_ui) {
        if (s_wx_ui->timer) {
            lv_timer_del(s_wx_ui->timer);
            s_wx_ui->timer = NULL;
        }
        if (s_wx_ui->qr_buf) {
            claw_free(s_wx_ui->qr_buf);
            s_wx_ui->qr_buf = NULL;
        }
        rt_free(s_wx_ui);
        s_wx_ui = NULL;
    }
}

static void msg_handler(gui_app_msg_type_t msg, void *param)
{
    (void)param;

    switch (msg) {
    case GUI_APP_MSG_ONSTART:
        on_start();
        break;
    case GUI_APP_MSG_ONRESUME:
        if (s_wx_ui) {
            wx_refresh(s_wx_ui);
        }
        break;
    case GUI_APP_MSG_ONPAUSE:
        break;
    case GUI_APP_MSG_ONSTOP:
        on_stop();
        break;
    default:
        break;
    }
}

static int app_wechat(intent_t i)
{
    (void)i;
    gui_app_regist_msg_handler(APP_ID, msg_handler);
    return 0;
}

BUILTIN_APP_EXPORT(LV_EXT_STR_ID(wechat), LV_EXT_IMG_GET(img_messages),
                   APP_ID, app_wechat, 1);
