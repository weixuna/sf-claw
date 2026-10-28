/**
 * @file wechat_ilink.h
 * @brief WeChat ClawBot (iLink) client for the sf32 watch.
 *
 * The service owns a background thread which performs QR-code login,
 * HTTPS long-polling (getupdates), and text replies (sendmessage).  The
 * GUI only reads a snapshot through wechat_ilink_get_status(), so LVGL
 * is never touched from the service thread.
 */

#ifndef WECHAT_ILINK_H
#define WECHAT_ILINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WECHAT_ILINK_QR_MAX     512
#define WECHAT_ILINK_MSG_MAX    192

/* Chat UI 同步用的最近消息环形缓冲。 */
#define WECHAT_ILINK_CHAT_TEXT_MAX 512
#define WECHAT_ILINK_CHAT_SLOTS    16
#define WECHAT_ILINK_CHAT_BOOTSTRAP 10

typedef enum {
    WECHAT_ILINK_CHAT_USER = 1,
    WECHAT_ILINK_CHAT_BOT,
} wechat_ilink_chat_role_t;

typedef struct {
    uint32_t seq;
    uint8_t role;       /* wechat_ilink_chat_role_t */
    char text[WECHAT_ILINK_CHAT_TEXT_MAX];
} wechat_ilink_chat_msg_t;

typedef enum {
    WECHAT_ILINK_IDLE = 0,
    WECHAT_ILINK_WAIT_SCAN,
    WECHAT_ILINK_SCANNED,
    WECHAT_ILINK_CONNECTED,
    WECHAT_ILINK_ERROR,
} wechat_ilink_state_t;

typedef struct {
    wechat_ilink_state_t state;
    bool     service_ready;  /* credentials have been loaded from KV */
    bool     configured;     /* a bot_token is loaded */
    bool     login_active;   /* QR login thread is running */
    bool     online;         /* b0 has an IP */
    uint32_t rx_count;
    uint32_t tx_count;

    /* qrcode_img_content, i.e. the payload that must be rendered as a
     * QR code.  The separate qrcode field is only used for polling. */
    char qr[WECHAT_ILINK_QR_MAX];
    char qr_token[192];
    char status[128];
    char bot_id[64];
    char last_rx[WECHAT_ILINK_MSG_MAX];
    char last_tx[WECHAT_ILINK_MSG_MAX];
} wechat_ilink_status_t;

/*
 * Start the background service.  It is normally called from
 * claw_port_on_ready(); calling it earlier is safe and only records the
 * request.  Returns 0 when the thread is running.
 */
int wechat_ilink_start(void);

/*
 * Start (or refresh) QR login.  force=true discards an existing session
 * and requests a new QR code.  The QR payload is published through
 * wechat_ilink_get_status() for the GUI to render.
 */
int wechat_ilink_login_start(bool force);
int wechat_ilink_login_cancel(void);

/* Forget the bot token and stop polling. */
int wechat_ilink_logout(void);

/* Thread-safe snapshot for the GUI thread. */
void wechat_ilink_get_status(wechat_ilink_status_t *out);
bool wechat_ilink_is_connected(void);

/*
 * Read the next WeChat chat message newer than *cursor.
 * The first call with *cursor == 0 returns the last few messages.
 * Returns 1 when out is filled, 0 when there is no new message.
 */
int wechat_ilink_read_chat(uint32_t *cursor, wechat_ilink_chat_msg_t *out);

/* Latest recorded chat sequence, used by Chat UI's clear action. */
uint32_t wechat_ilink_chat_latest_seq(void);

/* Drop the Chat UI sync history without affecting WeChat's login state. */
void wechat_ilink_clear_chat(void);

#ifdef __cplusplus
}
#endif

#endif /* WECHAT_ILINK_H */
