/**
 * @file app_chat.c
 * @brief Minimal Chat app for rt-claw on the 466x466 AMOLED watch.
 *
 * LVGL is only touched from the GUI thread.  AI work is done by
 * ui_bridge, which posts events consumed by ui_bridge_poll().
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "lvgl.h"
#include "lv_ext_resource_manager.h"
#include "lvsf_font.h"
#include "gui_app_fwk.h"

#include "ui_bridge.h"
#include "wechat_ilink.h"
#include "bt_pan.h"
#include "claw_port_cfg.h"
#include "claw_port_net.h"
#include "claw/services/ai/ai_engine.h"
#include "claw/services/ai/ai_memory.h"

#define APP_ID "Chat"

/* 控制同时存在的 LVGL 气泡数量，避免长对话反复布局导致卡顿。 */
#define CHAT_MAX_MSGS 20
#define CHAT_BUBBLE_TEXT_MAX 768

/* Round 466x466 safe-area layout.  Content is kept inside the circle;
 * the top/bottom 60px of a round panel are not usable for full-width UI. */
#define CHAT_STATUS_Y       64
#define CHAT_MSG_Y          104
#define CHAT_MSG_H          196
#define CHAT_QUICK_Y        304
#define CHAT_INPUT_Y        344
#define CHAT_INPUT_BOTTOM   (-78)
#define CHAT_KB_Y           150
#define CHAT_KB_W           320
#define CHAT_KB_H           250
#define CHAT_CONTENT_W      360
#define CHAT_INPUT_W        340
#define CHAT_STATUS_W       310

LV_IMG_DECLARE(img_messages);

typedef enum {
    BUBBLE_ASSISTANT = 0,
    BUBBLE_USER,
    BUBBLE_SYSTEM,
    BUBBLE_WECHAT_USER,
    BUBBLE_WECHAT_BOT,
} bubble_role_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *status_bar;
    lv_obj_t *net_icon;
    lv_obj_t *net_bars[4];
    lv_obj_t *net_label;
    lv_obj_t *status_label;
    lv_obj_t *msg_list;
    lv_obj_t *quick_row;
    lv_obj_t *input_panel;
    lv_obj_t *textarea;
    lv_obj_t *send_btn;
    lv_obj_t *kb;
    lv_obj_t *settings;
    lv_obj_t *settings_hint;
    lv_obj_t *set_url_ta;
    lv_obj_t *set_key_ta;
    lv_obj_t *set_model_ta;
    lv_obj_t *active_ta;
    lv_timer_t *timer;
    lv_obj_t *rows[CHAT_MAX_MSGS];
    int row_count;
    int net_state;          /* -1=未刷新，0=离线，1=PAN，2=联网 */
    uint32_t wechat_cursor; /* 微信聊天同步游标 */
    bool kb_visible;
} chat_ctx_t;

static chat_ctx_t *s_ctx;

static lv_font_t *chat_font(void)
{
    return (lv_font_t *)LV_EXT_FONT_GET(LVSF_FONT_NORMAL);
}

static lv_font_t *chat_font_small(void)
{
    return (lv_font_t *)LV_EXT_FONT_GET(LVSF_FONT_SMALL);
}

static void chat_set_status(chat_ctx_t *ctx, const char *text,
                            lv_color_t color)
{
    if (!ctx || !ctx->status_label) {
        return;
    }
    lv_label_set_text(ctx->status_label, text);
    lv_obj_set_style_text_color(ctx->status_label, color, 0);
}

static void chat_kb_hide(chat_ctx_t *ctx);
static void chat_update_net_ui(chat_ctx_t *ctx);

static void chat_set_signal(chat_ctx_t *ctx, int level, lv_color_t color)
{
    int i;

    if (!ctx) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (!ctx->net_bars[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(ctx->net_bars[i],
                                  (i < level) ? color
                                              : lv_color_hex(0x3A3A3C), 0);
    }
}

static void chat_update_net_ui(chat_ctx_t *ctx)
{
    bool pan;
    bool online;
    int state;

    if (!ctx) {
        return;
    }

    pan = bt_pan_is_connected();
    online = claw_port_net_is_online();
    state = pan ? (online ? 2 : 1) : 0;

    /* 网络状态没变化时不要重复改样式，减少无意义重绘。 */
    if (state == ctx->net_state) {
        return;
    }
    ctx->net_state = state;

    if (state == 2) {
        chat_set_signal(ctx, 4, lv_color_hex(0x30D158));
        lv_label_set_text(ctx->net_label, "已联网");
        lv_obj_set_style_text_color(ctx->net_label,
                                    lv_color_hex(0x30D158), 0);
    } else if (state == 1) {
        chat_set_signal(ctx, 2, lv_color_hex(0xFFD60A));
        lv_label_set_text(ctx->net_label, "PAN");
        lv_obj_set_style_text_color(ctx->net_label,
                                    lv_color_hex(0xFFD60A), 0);
    } else {
        chat_set_signal(ctx, 0, lv_color_hex(0x8E8E93));
        lv_label_set_text(ctx->net_label, "未连接");
        lv_obj_set_style_text_color(ctx->net_label,
                                    lv_color_hex(0xFF9F9F), 0);
    }
}

/* ---- On-screen keyboard -------------------------------------------------
 *
 * The stock keyboard is 12 keys wide and aligns to the very bottom of the
 * round panel, so its left/right/bottom keys fall outside the visible
 * circle.  Use a compact 7-column map instead, with explicit 删除/完成
 * keys.  The chat text row stays above the keyboard and keeps a large
 * 发送 button visible.
 */
#define KB_CTRL(w) (LV_BTNMATRIX_CTRL_CLICK_TRIG | (w))

static const char *kb_map_letters[] = {
    "q", "w", "e", "r", "t", "y", "u", "\n",
    "i", "o", "p", "a", "s", "d", "f", "\n",
    "g", "h", "j", "k", "l", "z", "x", "\n",
    "c", "v", "b", "n", "m", ".", ",", "\n",
    "aA", "123", "空格", "删除", "完成", ""
};

static const char *kb_map_letters_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "\n",
    "I", "O", "P", "A", "S", "D", "F", "\n",
    "G", "H", "J", "K", "L", "Z", "X", "\n",
    "C", "V", "B", "N", "M", ".", ",", "\n",
    "aA", "123", "空格", "删除", "完成", ""
};

static const lv_btnmatrix_ctrl_t kb_ctrl_letters[] = {
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(2), KB_CTRL(1), KB_CTRL(2)
};

static const char *kb_map_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "\n",
    "8", "9", "0", "-", "_", "@", ":", "\n",
    "/", ".", ",", "?", "!", "#", "%", "\n",
    "+", "=", "*", "(", ")", "[", "]", "\n",
    "ABC", "空格", "删除", "完成", ""
};

static const lv_btnmatrix_ctrl_t kb_ctrl_num[] = {
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1), KB_CTRL(1),
    KB_CTRL(1), KB_CTRL(3), KB_CTRL(2), KB_CTRL(2)
};

static void chat_kb_hide(chat_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->kb) {
        lv_keyboard_set_textarea(ctx->kb, NULL);
        lv_obj_add_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    }

    ctx->active_ta = NULL;
    ctx->kb_visible = false;

    if (ctx->status_bar) {
        lv_obj_clear_flag(ctx->status_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->quick_row) {
        lv_obj_clear_flag(ctx->quick_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->msg_list) {
        lv_obj_clear_flag(ctx->msg_list, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->input_panel) {
        lv_obj_clear_flag(ctx->input_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(ctx->input_panel, LV_ALIGN_BOTTOM_MID, 0,
                     CHAT_INPUT_BOTTOM);
        lv_obj_set_size(ctx->input_panel, CHAT_INPUT_W, 44);
    }
    if (ctx->settings) {
        lv_obj_set_style_pad_bottom(ctx->settings, 64, 0);
        lv_obj_update_layout(ctx->settings);
    }

    chat_update_net_ui(ctx);
}

static void chat_kb_show(chat_ctx_t *ctx, lv_obj_t *ta)
{
    if (!ctx || !ctx->kb || !ta) {
        return;
    }

    ctx->active_ta = ta;
    lv_keyboard_set_textarea(ctx->kb, ta);

    if (ctx->status_bar) {
        lv_obj_add_flag(ctx->status_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->quick_row) {
        lv_obj_add_flag(ctx->quick_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->msg_list) {
        lv_obj_add_flag(ctx->msg_list, LV_OBJ_FLAG_HIDDEN);
    }

    if (ta == ctx->textarea) {
        lv_obj_clear_flag(ctx->input_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(ctx->input_panel, LV_ALIGN_TOP_MID, 0, 96);
        lv_obj_set_size(ctx->input_panel, CHAT_INPUT_W, 44);
    } else {
        lv_obj_add_flag(ctx->input_panel, LV_OBJ_FLAG_HIDDEN);
        if (ctx->settings) {
            lv_obj_set_style_pad_bottom(ctx->settings, 320, 0);
            lv_obj_update_layout(ctx->settings);
            lv_obj_scroll_to_y(ctx->settings, lv_obj_get_y(ta) - 4,
                               LV_ANIM_ON);
        }
    }

    lv_obj_clear_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->kb);
    ctx->kb_visible = true;
}

static void kb_event_cb(lv_event_t *e)
{
    chat_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *kb = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta;
    uint16_t id;
    const char *txt;

    if (!ctx || !ctx->kb || kb != ctx->kb) {
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        id = lv_btnmatrix_get_selected_btn(kb);
        if (id == LV_BTNMATRIX_BTN_NONE) {
            return;
        }
        txt = lv_btnmatrix_get_btn_text(kb, id);
        if (!txt) {
            return;
        }
        ta = lv_keyboard_get_textarea(kb);

        if (strcmp(txt, "删除") == 0) {
            if (ta) {
                lv_textarea_del_char(ta);
            }
            return;
        }
        if (strcmp(txt, "空格") == 0) {
            if (ta) {
                lv_textarea_add_char(ta, ' ');
            }
            return;
        }
        if (strcmp(txt, "完成") == 0) {
            chat_kb_hide(ctx);
            return;
        }
        if (strcmp(txt, "aA") == 0) {
            lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
            lv_keyboard_set_mode(kb,
                                 (mode == LV_KEYBOARD_MODE_TEXT_UPPER)
                                     ? LV_KEYBOARD_MODE_TEXT_LOWER
                                     : LV_KEYBOARD_MODE_TEXT_UPPER);
            return;
        }
        if (strcmp(txt, "123") == 0) {
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
            return;
        }
        if (strcmp(txt, "ABC") == 0) {
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
            return;
        }

        lv_keyboard_def_event_cb(e);
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        chat_kb_hide(ctx);
    }
}

static void chat_trim_rows(chat_ctx_t *ctx)
{
    while (ctx->row_count >= CHAT_MAX_MSGS) {
        lv_obj_del(ctx->rows[0]);
        memmove(&ctx->rows[0], &ctx->rows[1],
                sizeof(ctx->rows[0]) * (CHAT_MAX_MSGS - 1));
        ctx->row_count--;
    }
}

/* 按 UTF-8 边界截断过长消息，避免超长 label 反复换行拖慢界面。 */
static void chat_copy_text(char *dst, size_t size, const char *src)
{
    size_t len;

    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len < size) {
        memcpy(dst, src, len + 1);
        return;
    }

    len = size - 1;
    while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
        len--;
    }
    memcpy(dst, src, len);
    if (len + 3 < size) {
        memcpy(dst + len, "…", 3);
        len += 3;
    }
    dst[len] = '\0';
}

static lv_obj_t *chat_add_bubble_raw(chat_ctx_t *ctx, const char *text,
                                     bubble_role_t role)
{
    char shown[CHAT_BUBBLE_TEXT_MAX + 4];
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *label;
    lv_color_t bg;
    lv_color_t fg;
    bool right;

    if (!ctx || !ctx->msg_list || !text || !text[0]) {
        return NULL;
    }

    chat_copy_text(shown, sizeof(shown), text);
    if (!shown[0]) {
        return NULL;
    }

    chat_trim_rows(ctx);

    right = (role == BUBBLE_USER || role == BUBBLE_WECHAT_USER);
    row = lv_obj_create(ctx->msg_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          right ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, LV_PCT(78));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);

    if (role == BUBBLE_WECHAT_USER) {
        bg = lv_color_hex(0x07C160);
        fg = lv_color_white();
    } else if (role == BUBBLE_WECHAT_BOT) {
        bg = lv_color_hex(0x183A28);
        fg = lv_color_hex(0xD8FFE6);
    } else if (role == BUBBLE_USER) {
        bg = lv_color_hex(0x0A84FF);
        fg = lv_color_white();
    } else if (role == BUBBLE_SYSTEM) {
        bg = lv_color_hex(0x3A1F1F);
        fg = lv_color_hex(0xFF9F9F);
    } else {
        bg = lv_color_hex(0x2C2C2E);
        fg = lv_color_white();
    }

    lv_obj_set_style_bg_color(bubble, bg, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);

    label = lv_label_create(bubble);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, shown);
    lv_obj_set_style_text_font(label, chat_font(), 0);
    lv_obj_set_style_text_color(label, fg, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);

    ctx->rows[ctx->row_count++] = row;
    return row;
}

static void chat_scroll_latest(chat_ctx_t *ctx, lv_obj_t *row, bool animate)
{
    if (!ctx || !ctx->msg_list || !row) {
        return;
    }

    /* 批量同步时只做一次布局和滚动，避免每条消息都全量重排。 */
    lv_obj_update_layout(ctx->msg_list);
    lv_obj_scroll_to_view(row, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

static void chat_add_bubble(chat_ctx_t *ctx, const char *text,
                            bubble_role_t role)
{
    lv_obj_t *row = chat_add_bubble_raw(ctx, text, role);

    if (row) {
        chat_scroll_latest(ctx, row, true);
    }
}

/* 把微信最近消息批量追加到 Chat UI。 */
static void chat_sync_wechat(chat_ctx_t *ctx)
{
    wechat_ilink_chat_msg_t msg;
    lv_obj_t *last = NULL;
    int added = 0;
    int tries = 0;

    if (!ctx) {
        return;
    }

    while (tries++ < 12 && added < WECHAT_ILINK_CHAT_BOOTSTRAP &&
           wechat_ilink_read_chat(&ctx->wechat_cursor, &msg) > 0) {
        bubble_role_t role = (msg.role == WECHAT_ILINK_CHAT_USER)
                                 ? BUBBLE_WECHAT_USER
                                 : BUBBLE_WECHAT_BOT;
        lv_obj_t *row = chat_add_bubble_raw(ctx, msg.text, role);

        if (row) {
            last = row;
            added++;
        }
    }

    if (added) {
        chat_scroll_latest(ctx, last, false);
    }
}

static void chat_clear(chat_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    lv_obj_clean(ctx->msg_list);
    ctx->row_count = 0;
    wechat_ilink_clear_chat();
    ctx->wechat_cursor = wechat_ilink_chat_latest_seq();
    ai_memory_clear_channel(AI_CHANNEL_UI);

    chat_add_bubble(ctx,
                    "对话已清除。\n可以问我设备状态，或使用下面的快捷指令。",
                    BUBBLE_SYSTEM);
}

static void chat_send(chat_ctx_t *ctx, const char *text)
{
    int rc;

    if (!ctx || !text || !text[0]) {
        return;
    }

    chat_kb_hide(ctx);

    chat_add_bubble(ctx, text, BUBBLE_USER);

    rc = ui_bridge_send_text(text);
    if (rc == 0) {
        chat_set_status(ctx, "思考中…", lv_color_hex(0xFFD60A));
    } else if (rc == -2) {
        chat_set_status(ctx, "正在处理上一条", lv_color_hex(0xFF9F0A));
    } else if (rc == -1) {
        chat_set_status(ctx, "AI 正在初始化…", lv_color_hex(0x8E8E93));
        chat_add_bubble(ctx, "AI 尚未就绪，请稍候再试。", BUBBLE_SYSTEM);
    } else {
        chat_set_status(ctx, "发送失败", lv_color_hex(0xFF453A));
        chat_add_bubble(ctx, "发送失败，请检查网络或 API 配置。",
                        BUBBLE_SYSTEM);
    }

    if (ctx->textarea) {
        lv_textarea_set_text(ctx->textarea, "");
    }
}

static void chat_send_current(chat_ctx_t *ctx)
{
    const char *text;

    if (!ctx || !ctx->textarea) {
        return;
    }
    text = lv_textarea_get_text(ctx->textarea);
    if (!text || !text[0]) {
        chat_set_status(ctx, "请输入内容", lv_color_hex(0xFFD60A));
        return;
    }
    chat_send(ctx, text);
}

static void ta_event_cb(lv_event_t *e)
{
    chat_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (!ctx || !ctx->kb) {
        return;
    }

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        chat_kb_show(ctx, ta);
    } else if (code == LV_EVENT_CANCEL) {
        if (ctx->active_ta == ta) {
            chat_kb_hide(ctx);
        }
    }
}

static void send_btn_event_cb(lv_event_t *e)
{
    chat_ctx_t *ctx = lv_event_get_user_data(e);

    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        chat_send_current(ctx);
    }
}

static void quick_phrase_event_cb(lv_event_t *e)
{
    chat_ctx_t *ctx = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    const char *text = lv_obj_get_user_data(btn);

    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !text) {
        return;
    }

    if (strcmp(text, "清除对话") == 0) {
        chat_clear(ctx);
    } else if (strcmp(text, "微信连接") == 0) {
        gui_app_run("WeChat");
    } else {
        chat_send(ctx, text);
    }
}

static void settings_close(chat_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    chat_kb_hide(ctx);
    if (ctx->settings) {
        lv_obj_del(ctx->settings);
        ctx->settings = NULL;
    }
    ctx->settings_hint = NULL;
    ctx->set_url_ta = NULL;
    ctx->set_key_ta = NULL;
    ctx->set_model_ta = NULL;
}

static void settings_save_event_cb(lv_event_t *e)
{
    chat_ctx_t *ctx = lv_event_get_user_data(e);
    int rc;

    if (!ctx || !ctx->settings || !ctx->set_url_ta ||
        !ctx->set_key_ta || !ctx->set_model_ta ||
        lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    rc = claw_port_ai_config_set(lv_textarea_get_text(ctx->set_url_ta),
                                 lv_textarea_get_text(ctx->set_key_ta),
                                 lv_textarea_get_text(ctx->set_model_ta));
    if (rc == CLAW_OK) {
        chat_set_status(ctx, "设置已保存", lv_color_hex(0x30D158));
        settings_close(ctx);
    } else {
        chat_set_status(ctx, "设置保存失败", lv_color_hex(0xFF453A));
        if (ctx->settings_hint) {
            lv_label_set_text(ctx->settings_hint, "保存失败，请检查参数");
            lv_obj_set_style_text_color(ctx->settings_hint,
                                        lv_color_hex(0xFF9F9F), 0);
        }
    }
}

static void settings_close_event_cb(lv_event_t *e)
{
    chat_ctx_t *ctx = lv_event_get_user_data(e);

    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_close(ctx);
    }
}

static lv_obj_t *settings_add_field(chat_ctx_t *ctx, lv_obj_t *parent,
                                    const char *label_text,
                                    const char *value, bool password)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, chat_font_small(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xB0B0B0), 0);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, 40);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 255);
    lv_textarea_set_text(ta, value ? value : "");
    lv_obj_set_style_text_font(ta, chat_font_small(), 0);
    if (password) {
        lv_textarea_set_password_mode(ta, true);
    }
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, ctx);

    return ta;
}

static void settings_open(chat_ctx_t *ctx)
{
    lv_obj_t *overlay;
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *row;
    lv_obj_t *save_btn;
    lv_obj_t *close_btn;
    lv_obj_t *label;

    if (!ctx || ctx->settings) {
        return;
    }

    chat_kb_hide(ctx);

    overlay = lv_obj_create(ctx->scr);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_left(overlay, 76, 0);
    lv_obj_set_style_pad_right(overlay, 76, 0);
    lv_obj_set_style_pad_top(overlay, 64, 0);
    lv_obj_set_style_pad_bottom(overlay, 64, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(overlay, 8, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    ctx->settings = overlay;

    title = lv_label_create(overlay);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, chat_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    hint = lv_label_create(overlay);
    lv_label_set_text(hint, "编辑后点“保存”");
    lv_obj_set_style_text_font(hint, chat_font_small(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8E8E93), 0);
    ctx->settings_hint = hint;

    ctx->set_url_ta = settings_add_field(ctx, overlay, "API URL",
                                         ai_get_api_url(), false);
    ctx->set_key_ta = settings_add_field(ctx, overlay, "API Key",
                                         ai_get_api_key(), true);
    ctx->set_model_ta = settings_add_field(ctx, overlay, "Model",
                                           ai_get_model(), false);

    row = lv_obj_create(overlay);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    close_btn = lv_btn_create(row);
    lv_obj_set_size(close_btn, 100, 40);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x3A3A3C), 0);
    lv_obj_add_event_cb(close_btn, settings_close_event_cb,
                        LV_EVENT_CLICKED, ctx);
    label = lv_label_create(close_btn);
    lv_label_set_text(label, "关闭");
    lv_obj_set_style_text_font(label, chat_font_small(), 0);
    lv_obj_center(label);

    save_btn = lv_btn_create(row);
    lv_obj_set_size(save_btn, 100, 40);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_add_event_cb(save_btn, settings_save_event_cb,
                        LV_EVENT_CLICKED, ctx);
    label = lv_label_create(save_btn);
    lv_label_set_text(label, "保存");
    lv_obj_set_style_text_font(label, chat_font_small(), 0);
    lv_obj_center(label);
}

static void settings_btn_event_cb(lv_event_t *e)
{
    chat_ctx_t *ctx = lv_event_get_user_data(e);

    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_open(ctx);
    }
}

static void chat_timer_cb(lv_timer_t *timer)
{
    chat_ctx_t *ctx = timer ? timer->user_data : NULL;

    if (!ctx) {
        return;
    }

    chat_update_net_ui(ctx);
    chat_sync_wechat(ctx);

    if (!ui_bridge_is_ready()) {
        chat_set_status(ctx, "初始化中…", lv_color_hex(0x8E8E93));
    } else if (strcmp(lv_label_get_text(ctx->status_label),
                      "初始化中…") == 0) {
        chat_set_status(ctx, "就绪", lv_color_hex(0x30D158));
    }
}

static void chat_build_ui(chat_ctx_t *ctx)
{
    lv_obj_t *scr = ctx->scr;
    lv_obj_t *settings_btn;
    lv_obj_t *label;
    lv_obj_t *bar;
    static const char *quick[] = {
        "微信连接", "现在几点", "设备状态", "清除对话",
    };

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* status bar — kept below the round top edge */
    ctx->status_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(ctx->status_bar);
    lv_obj_set_size(ctx->status_bar, CHAT_STATUS_W, 34);
    lv_obj_align(ctx->status_bar, LV_ALIGN_TOP_MID, 0, CHAT_STATUS_Y);
    lv_obj_set_style_bg_opa(ctx->status_bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ctx->status_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* four signal bars: 0 = PAN offline, 2 = PAN up / no IP, 4 = online */
    ctx->net_icon = lv_obj_create(ctx->status_bar);
    lv_obj_remove_style_all(ctx->net_icon);
    lv_obj_set_size(ctx->net_icon, 22, 18);
    lv_obj_align(ctx->net_icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(ctx->net_icon, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        bar = lv_obj_create(ctx->net_icon);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 5 + i * 4);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 6, 0);
        ctx->net_bars[i] = bar;
    }

    ctx->net_label = lv_label_create(ctx->status_bar);
    lv_label_set_text(ctx->net_label, "未连接");
    lv_obj_set_width(ctx->net_label, 64);
    lv_label_set_long_mode(ctx->net_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(ctx->net_label, chat_font_small(), 0);
    lv_obj_set_style_text_color(ctx->net_label,
                                lv_color_hex(0xFF9F9F), 0);
    lv_obj_align(ctx->net_label, LV_ALIGN_LEFT_MID, 28, 0);

    ctx->status_label = lv_label_create(ctx->status_bar);
    lv_label_set_text(ctx->status_label, "初始化中…");
    lv_obj_set_width(ctx->status_label, 150);
    lv_label_set_long_mode(ctx->status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ctx->status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->status_label, chat_font_small(), 0);
    lv_obj_set_style_text_color(ctx->status_label,
                                lv_color_hex(0x8E8E93), 0);
    lv_obj_align(ctx->status_label, LV_ALIGN_LEFT_MID, 96, 0);

    settings_btn = lv_btn_create(ctx->status_bar);
    lv_obj_set_size(settings_btn, 52, 28);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(settings_btn, 12, 0);
    lv_obj_align(settings_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(settings_btn, settings_btn_event_cb,
                        LV_EVENT_CLICKED, ctx);
    label = lv_label_create(settings_btn);
    lv_label_set_text(label, "设置");
    lv_obj_set_style_text_font(label, chat_font_small(), 0);
    lv_obj_center(label);

    /* message list */
    ctx->msg_list = lv_obj_create(scr);
    lv_obj_set_size(ctx->msg_list, CHAT_CONTENT_W, CHAT_MSG_H);
    lv_obj_align(ctx->msg_list, LV_ALIGN_TOP_MID, 0, CHAT_MSG_Y);
    lv_obj_set_style_bg_opa(ctx->msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->msg_list, 0, 0);
    lv_obj_set_style_pad_all(ctx->msg_list, 4, 0);
    lv_obj_set_style_pad_row(ctx->msg_list, 8, 0);
    lv_obj_set_flex_flow(ctx->msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctx->msg_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(ctx->msg_list, LV_SCROLLBAR_MODE_OFF);

    /* quick phrases — one scrollable row, not a clipped two-row grid */
    ctx->quick_row = lv_obj_create(scr);
    lv_obj_remove_style_all(ctx->quick_row);
    lv_obj_set_size(ctx->quick_row, CHAT_CONTENT_W, 34);
    lv_obj_align(ctx->quick_row, LV_ALIGN_TOP_MID, 0, CHAT_QUICK_Y);
    lv_obj_set_flex_flow(ctx->quick_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ctx->quick_row, 6, 0);
    lv_obj_set_style_pad_ver(ctx->quick_row, 2, 0);
    lv_obj_set_scroll_dir(ctx->quick_row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(ctx->quick_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ctx->quick_row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(quick) / sizeof(quick[0]); i++) {
        lv_obj_t *btn = lv_btn_create(ctx->quick_row);
        lv_obj_set_height(btn, 30);
        lv_obj_set_style_pad_hor(btn, 12, 0);
        lv_obj_set_style_pad_ver(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_radius(btn, 15, 0);
        lv_obj_set_user_data(btn, (void *)quick[i]);
        lv_obj_add_event_cb(btn, quick_phrase_event_cb,
                            LV_EVENT_CLICKED, ctx);
        label = lv_label_create(btn);
        lv_label_set_text(label, quick[i]);
        lv_obj_set_style_text_font(label, chat_font_small(), 0);
        lv_obj_center(label);
    }

    /* text input row, kept above the round bottom edge */
    ctx->input_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(ctx->input_panel);
    lv_obj_set_size(ctx->input_panel, CHAT_INPUT_W, 44);
    lv_obj_align(ctx->input_panel, LV_ALIGN_BOTTOM_MID, 0,
                 CHAT_INPUT_BOTTOM);
    lv_obj_set_style_bg_opa(ctx->input_panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ctx->input_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctx->input_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->input_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctx->input_panel, 8, 0);

    ctx->textarea = lv_textarea_create(ctx->input_panel);
    lv_obj_set_height(ctx->textarea, 40);
    lv_obj_set_flex_grow(ctx->textarea, 1);
    lv_textarea_set_one_line(ctx->textarea, true);
    lv_textarea_set_placeholder_text(ctx->textarea, "说点什么…");
    lv_textarea_set_max_length(ctx->textarea, 480);
    lv_obj_set_style_text_font(ctx->textarea, chat_font_small(), 0);
    lv_obj_add_event_cb(ctx->textarea, ta_event_cb, LV_EVENT_ALL, ctx);

    ctx->send_btn = lv_btn_create(ctx->input_panel);
    lv_obj_set_size(ctx->send_btn, 76, 40);
    lv_obj_set_style_bg_color(ctx->send_btn, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_radius(ctx->send_btn, 14, 0);
    lv_obj_add_event_cb(ctx->send_btn, send_btn_event_cb,
                        LV_EVENT_CLICKED, ctx);
    label = lv_label_create(ctx->send_btn);
    lv_label_set_text(label, "发送");
    lv_obj_set_style_text_font(label, chat_font_small(), 0);
    lv_obj_center(label);

    /* compact 7-column keyboard with explicit 删除/完成 keys */
    ctx->kb = lv_keyboard_create(scr);
    lv_obj_set_size(ctx->kb, CHAT_KB_W, CHAT_KB_H);
    lv_obj_align(ctx->kb, LV_ALIGN_TOP_MID, 0, CHAT_KB_Y);
    lv_obj_set_style_bg_color(ctx->kb, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(ctx->kb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ctx->kb, 18, 0);
    lv_obj_set_style_border_width(ctx->kb, 0, 0);
    lv_obj_set_style_pad_all(ctx->kb, 6, 0);
    lv_obj_set_style_pad_row(ctx->kb, 4, 0);
    lv_obj_set_style_pad_column(ctx->kb, 4, 0);
    lv_obj_set_style_bg_color(ctx->kb, lv_color_hex(0x3A3A3C),
                              LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(ctx->kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(ctx->kb, lv_color_white(),
                                LV_PART_ITEMS);
    lv_obj_set_style_text_font(ctx->kb, chat_font_small(),
                               LV_PART_ITEMS);
    lv_obj_set_style_radius(ctx->kb, 9, LV_PART_ITEMS);
    lv_obj_set_style_border_width(ctx->kb, 0, LV_PART_ITEMS);

    lv_obj_remove_event_cb(ctx->kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(ctx->kb, kb_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_keyboard_set_map(ctx->kb, LV_KEYBOARD_MODE_TEXT_LOWER,
                        kb_map_letters, kb_ctrl_letters);
    lv_keyboard_set_map(ctx->kb, LV_KEYBOARD_MODE_TEXT_UPPER,
                        kb_map_letters_upper, kb_ctrl_letters);
    lv_keyboard_set_map(ctx->kb, LV_KEYBOARD_MODE_NUMBER,
                        kb_map_num, kb_ctrl_num);
    lv_keyboard_set_mode(ctx->kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
}

static void on_start(void)
{
    chat_ctx_t *ctx = rt_malloc(sizeof(*ctx));

    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->scr = lv_scr_act();
    ctx->net_state = -1;
    ctx->wechat_cursor = 0;
    s_ctx = ctx;

    chat_build_ui(ctx);
    ctx->timer = lv_timer_create(chat_timer_cb, 1000, ctx);
    chat_update_net_ui(ctx);
    chat_set_status(ctx,
                    ui_bridge_is_ready() ? "就绪" : "初始化中…",
                    ui_bridge_is_ready() ? lv_color_hex(0x30D158)
                                         : lv_color_hex(0x8E8E93));

    chat_sync_wechat(ctx);
    chat_add_bubble(ctx,
                    "你好，我是 SF-Claw。\n可以问我设备状态，或使用下面的快捷指令。",
                    BUBBLE_ASSISTANT);
}

static void on_stop(void)
{
    if (s_ctx) {
        if (s_ctx->timer) {
            lv_timer_del(s_ctx->timer);
            s_ctx->timer = NULL;
        }
        rt_free(s_ctx);
        s_ctx = NULL;
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

/* ---------------- ui_bridge sinks (GUI thread) ---------------- */

void chat_ui_on_status(int status, const char *detail)
{
    if (!s_ctx) {
        return;
    }

    if (status == AI_STATUS_THINKING) {
        chat_set_status(s_ctx, "思考中…", lv_color_hex(0xFFD60A));
    } else if (status == AI_STATUS_TOOL_CALL) {
        char buf[96];
        snprintf(buf, sizeof(buf), "执行: %s", detail ? detail : "tool");
        chat_set_status(s_ctx, buf, lv_color_hex(0x64D2FF));
    } else if (status == AI_STATUS_DONE) {
        chat_set_status(s_ctx, "就绪", lv_color_hex(0x30D158));
    }
}

void chat_ui_on_reply(const char *text)
{
    if (!s_ctx || !text) {
        return;
    }
    chat_add_bubble(s_ctx, text, BUBBLE_ASSISTANT);
    chat_set_status(s_ctx, "就绪", lv_color_hex(0x30D158));
}

void chat_ui_on_error(const char *text)
{
    const char *friendly = "请求失败，请稍后重试。";

    if (!s_ctx) {
        return;
    }

    if (text) {
        if (strstr(text, "no API key")) {
            friendly = "请先在设置中填写 API Key。";
        } else if (strstr(text, "API request failed")) {
            friendly = "请求失败，请检查手机网络或 API Key。";
        } else if (strstr(text, "is busy")) {
            friendly = "正在处理上一条，请稍候。";
        } else if (strstr(text, "not initialized")) {
            friendly = "AI 尚未初始化。";
        } else if (strstr(text, "API error")) {
            friendly = text;
        }
    }

    chat_add_bubble(s_ctx, friendly, BUBBLE_SYSTEM);
    chat_set_status(s_ctx, "出错", lv_color_hex(0xFF453A));
}

void chat_ui_on_net(int online)
{
    (void)online;
    if (s_ctx) {
        chat_update_net_ui(s_ctx);
    }
}

static int app_chat(intent_t i)
{
    (void)i;
    gui_app_regist_msg_handler(APP_ID, msg_handler);
    return 0;
}

BUILTIN_APP_EXPORT(LV_EXT_STR_ID(chat), LV_EXT_IMG_GET(img_messages),
                   APP_ID, app_chat, 1);
