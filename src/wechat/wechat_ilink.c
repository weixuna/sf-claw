/**
 * @file wechat_ilink.c
 * @brief WeChat ClawBot (iLink) client: QR login + HTTPS long polling.
 *
 * Protocol summary (see docs/protocol.md in Tencent/openclaw-weixin):
 *   GET  /ilink/bot/get_bot_qrcode?bot_type=3
 *   GET  /ilink/bot/get_qrcode_status?qrcode=...
 *   POST /ilink/bot/getupdates          (35 s long poll)
 *   POST /ilink/bot/sendmessage
 *
 * All requests are outbound HTTPS, so they work over the phone's BT PAN
 * link (b0) without a public IP or callback URL.  The QR payload is
 * qrcode_img_content; the GUI renders it with an LVGL RGB565 canvas.
 */

#include "wechat_ilink.h"

#include "wechat_http.h"

#include <rtthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "osal/claw_os.h"
#include "osal/claw_kv.h"
#include "osal/claw_net.h"
#include "claw_port.h"
#include "claw_port_boot.h"
#include "claw_port_net.h"
#include "claw/services/ai/ai_engine.h"
#include "claw/services/ai/ai_memory.h"
#include "ui_bridge.h"

#define TAG "wechat"

#define WECHAT_KV_NS             "wechat"
#define WECHAT_DEFAULT_BASE      "https://ilinkai.weixin.qq.com"
#define WECHAT_APP_ID            "bot"
#define WECHAT_CLIENT_VERSION    "131329"
#define WECHAT_CHANNEL_VERSION   "sf-claw-wechat"

#define WECHAT_HTTP_RESP_SIZE    32768
#define WECHAT_AI_REPLY_SIZE     4096
#define WECHAT_IN_TEXT_SIZE      4096
#define WECHAT_SEND_CHUNK        3000
#define WECHAT_DEDUP_SLOTS       16
#define WECHAT_QR_REFRESH_MAX    3
#define WECHAT_QR_TTL_MS         (4 * 60 * 1000)
#define WECHAT_POLL_RETRY_MS     2000
#define WECHAT_QR_POLL_RETRY_MS  2000
#define WECHAT_QR_HTTP_TIMEOUT_MS     15000
#define WECHAT_LONGPOLL_TIMEOUT_MS    50000
#define WECHAT_SEND_HTTP_TIMEOUT_MS   20000
#define WECHAT_THREAD_STACK      12288
#define WECHAT_THREAD_PRIO       24

#define WECHAT_TOKEN_MAX         512
#define WECHAT_BASE_MAX          256
#define WECHAT_CURSOR_MAX        512
#define WECHAT_CONTEXT_MAX       256
#define WECHAT_CHAT_MAX          96

typedef struct {
    char id[40];
} wechat_dedup_t;

typedef struct {
    struct claw_mutex *lock;
    struct claw_sem   *wake;
    struct claw_thread *thread;

    uint32_t gen;

    bool service_ready;
    bool login_requested;
    bool login_active;
    bool poll_enabled;

    wechat_ilink_state_t state;
    char status[128];

    char token[WECHAT_TOKEN_MAX];
    char base_url[WECHAT_BASE_MAX];
    char bot_id[64];
    char user_id[128];
    char cursor[WECHAT_CURSOR_MAX];

    char qr[WECHAT_ILINK_QR_MAX];
    char qr_token[192];
    char qr_api_base[192];

    char last_chat_id[WECHAT_CHAT_MAX];
    char last_context_token[WECHAT_CONTEXT_MAX];
    char last_rx[WECHAT_ILINK_MSG_MAX];
    char last_tx[WECHAT_ILINK_MSG_MAX];
    uint32_t rx_count;
    uint32_t tx_count;

    /* Chat UI 同步记录，seq 单调递增，清空记录时也不回退。 */
    wechat_ilink_chat_msg_t chat[WECHAT_ILINK_CHAT_SLOTS];
    int chat_idx;
    uint32_t chat_seq;

    wechat_dedup_t dedup[WECHAT_DEDUP_SLOTS];
    int dedup_idx;
} wechat_ctx_t;

static wechat_ctx_t s_wx;

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void wx_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

/* Copy and cut on a UTF-8 boundary so LVGL never sees half a character. */
static void wx_copy_utf8(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* 调用方必须持有 s_wx.lock。 */
static void wx_chat_add_locked(uint8_t role, const char *text)
{
    wechat_ilink_chat_msg_t *msg;

    if (!text || !text[0]) {
        return;
    }

    msg = &s_wx.chat[s_wx.chat_idx];
    memset(msg, 0, sizeof(*msg));
    msg->seq = ++s_wx.chat_seq;
    msg->role = role;
    wx_copy_utf8(msg->text, sizeof(msg->text), text);
    s_wx.chat_idx = (s_wx.chat_idx + 1) % WECHAT_ILINK_CHAT_SLOTS;
}

static void wx_chat_clear_locked(void)
{
    memset(s_wx.chat, 0, sizeof(s_wx.chat));
    s_wx.chat_idx = 0;
}

static void wx_lock(void)
{
    if (s_wx.lock) {
        claw_mutex_lock(s_wx.lock, CLAW_WAIT_FOREVER);
    }
}

static void wx_unlock(void)
{
    if (s_wx.lock) {
        claw_mutex_unlock(s_wx.lock);
    }
}

static bool wx_gen_changed(uint32_t gen)
{
    bool changed;

    wx_lock();
    changed = (s_wx.gen != gen);
    wx_unlock();
    return changed;
}

static void wx_set_state_locked(wechat_ilink_state_t state, const char *status)
{
    s_wx.state = state;
    wx_copy(s_wx.status, sizeof(s_wx.status), status);
}

static void wx_set_message_locked(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_wx.status, sizeof(s_wx.status), fmt, ap);
    va_end(ap);
}

static bool wx_time_valid(void)
{
    time_t now = time(NULL);
    struct tm tm_now;

    if (now <= 0) {
        return false;
    }
    if (!localtime_r(&now, &tm_now)) {
        return false;
    }
    return (tm_now.tm_year + 1900) >= 2024;
}

static int wx_base64_encode(const unsigned char *src, size_t len,
                            char *dst, size_t dst_size)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4 + 1;
    size_t i;
    size_t o = 0;

    if (!src || !dst || dst_size < need) {
        return -1;
    }

    for (i = 0; i < len; i += 3) {
        unsigned int v = (unsigned int)src[i] << 16;
        size_t remain = len - i;

        if (remain > 1) {
            v |= (unsigned int)src[i + 1] << 8;
        }
        if (remain > 2) {
            v |= (unsigned int)src[i + 2];
        }

        dst[o++] = tbl[(v >> 18) & 0x3f];
        dst[o++] = tbl[(v >> 12) & 0x3f];
        dst[o++] = (remain > 1) ? tbl[(v >> 6) & 0x3f] : '=';
        dst[o++] = (remain > 2) ? tbl[v & 0x3f] : '=';
    }
    dst[o] = '\0';
    return 0;
}

static void wx_make_uin(char *buf, size_t size)
{
    char decimal[16];
    uint32_t v;

    v = (uint32_t)time(NULL);
    v ^= (uint32_t)rt_tick_get() * 2654435761u;
    v ^= (uint32_t)(uintptr_t)buf;

    snprintf(decimal, sizeof(decimal), "%lu", (unsigned long)v);
    if (wx_base64_encode((const unsigned char *)decimal, strlen(decimal),
                         buf, size) != 0) {
        wx_copy(buf, size, "MA==");
    }
}

static int wx_url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (!src || !dst || dst_size == 0) {
        return -1;
    }

    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            if (o + 1 >= dst_size) {
                return -1;
            }
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= dst_size) {
                return -1;
            }
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0f];
        }
    }

    dst[o] = '\0';
    return 0;
}

static void wx_normalize_base(char *dst, size_t dst_size, const char *src)
{
    char tmp[WECHAT_BASE_MAX];
    size_t len;

    if (!src || !src[0]) {
        wx_copy(dst, dst_size, WECHAT_DEFAULT_BASE);
        return;
    }

    if (strncmp(src, "https://", 8) == 0 || strncmp(src, "http://", 7) == 0) {
        wx_copy(tmp, sizeof(tmp), src);
    } else {
        snprintf(tmp, sizeof(tmp), "https://%s", src);
    }

    len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    wx_copy(dst, dst_size, tmp);
}

static void wx_client_id(char *buf, size_t size)
{
    static uint32_t seq;
    uint32_t a = (uint32_t)rt_tick_get();
    uint32_t b = ++seq;

    snprintf(buf, size, "rtclaw-%08lx%08lx",
             (unsigned long)a, (unsigned long)b);
}

/* ------------------------------------------------------------------ */
/*  HTTP                                                               */
/* ------------------------------------------------------------------ */

static int wx_get(const char *url, bool with_auth,
                  char *resp, size_t resp_size, uint32_t timeout_ms)
{
    claw_net_header_t h[5];
    char uin[64];
    char auth[WECHAT_TOKEN_MAX + 8];
    int n = 0;
    int status;

    wx_make_uin(uin, sizeof(uin));
    h[n++] = (claw_net_header_t){ "iLink-App-Id", WECHAT_APP_ID };
    h[n++] = (claw_net_header_t){ "iLink-App-ClientVersion",
                                  WECHAT_CLIENT_VERSION };
    h[n++] = (claw_net_header_t){ "X-WECHAT-UIN", uin };

    if (with_auth) {
        wx_lock();
        snprintf(auth, sizeof(auth), "Bearer %s", s_wx.token);
        wx_unlock();
        h[n++] = (claw_net_header_t){ "AuthorizationType", "ilink_bot_token" };
        h[n++] = (claw_net_header_t){ "Authorization", auth };
    }

    status = wechat_http_request("GET", url, h, n, NULL, 0,
                                 resp, resp_size, NULL, timeout_ms);
    return status;
}

static int wx_post(const char *url, const char *body, bool with_auth,
                   char *resp, size_t resp_size, uint32_t timeout_ms)
{
    claw_net_header_t h[6];
    char uin[64];
    char auth[WECHAT_TOKEN_MAX + 8];
    int n = 0;
    int status;

    wx_make_uin(uin, sizeof(uin));
    h[n++] = (claw_net_header_t){ "Content-Type", "application/json" };
    h[n++] = (claw_net_header_t){ "iLink-App-Id", WECHAT_APP_ID };
    h[n++] = (claw_net_header_t){ "iLink-App-ClientVersion",
                                  WECHAT_CLIENT_VERSION };
    h[n++] = (claw_net_header_t){ "X-WECHAT-UIN", uin };
    h[n++] = (claw_net_header_t){ "AuthorizationType", "ilink_bot_token" };

    if (with_auth) {
        wx_lock();
        snprintf(auth, sizeof(auth), "Bearer %s", s_wx.token);
        wx_unlock();
        h[n++] = (claw_net_header_t){ "Authorization", auth };
    }

    status = wechat_http_request("POST", url, h, n, body, strlen(body),
                                 resp, resp_size, NULL, timeout_ms);
    return status;
}

static const char *wx_json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int wx_json_int(cJSON *root, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return atoi(item->valuestring);
    }
    return def;
}

/* ------------------------------------------------------------------ */
/*  KV persistence                                                     */
/* ------------------------------------------------------------------ */

static void wx_save_kv(const char *token, const char *base_url,
                       const char *bot_id, const char *user_id)
{
    if (claw_kv_set_str(WECHAT_KV_NS, "token", token) != CLAW_OK) {
        CLAW_LOGW(TAG, "save token failed");
    }
    if (base_url && base_url[0] &&
        claw_kv_set_str(WECHAT_KV_NS, "base_url", base_url) != CLAW_OK) {
        CLAW_LOGW(TAG, "save base_url failed");
    }
    if (bot_id && bot_id[0] &&
        claw_kv_set_str(WECHAT_KV_NS, "bot_id", bot_id) != CLAW_OK) {
        CLAW_LOGW(TAG, "save bot_id failed");
    }
    if (user_id && user_id[0] &&
        claw_kv_set_str(WECHAT_KV_NS, "user_id", user_id) != CLAW_OK) {
        CLAW_LOGW(TAG, "save user_id failed");
    }
}

static void wx_delete_kv(void)
{
    (void)claw_kv_delete(WECHAT_KV_NS, "token");
    (void)claw_kv_delete(WECHAT_KV_NS, "base_url");
    (void)claw_kv_delete(WECHAT_KV_NS, "bot_id");
    (void)claw_kv_delete(WECHAT_KV_NS, "user_id");
}

static void wx_load_credentials(void)
{
    char token[WECHAT_TOKEN_MAX] = {0};
    char base[WECHAT_BASE_MAX] = {0};
    char bot_id[64] = {0};
    char user_id[128] = {0};

    (void)claw_kv_get_str(WECHAT_KV_NS, "token", token, sizeof(token));
    (void)claw_kv_get_str(WECHAT_KV_NS, "base_url", base, sizeof(base));
    (void)claw_kv_get_str(WECHAT_KV_NS, "bot_id", bot_id, sizeof(bot_id));
    (void)claw_kv_get_str(WECHAT_KV_NS, "user_id", user_id, sizeof(user_id));

    wx_lock();
    wx_copy(s_wx.token, sizeof(s_wx.token), token);
    wx_copy(s_wx.base_url, sizeof(s_wx.base_url),
            base[0] ? base : WECHAT_DEFAULT_BASE);
    wx_copy(s_wx.bot_id, sizeof(s_wx.bot_id), bot_id);
    wx_copy(s_wx.user_id, sizeof(s_wx.user_id), user_id);
    s_wx.cursor[0] = '\0';
    s_wx.service_ready = true;

    if (s_wx.token[0] && s_wx.base_url[0]) {
        s_wx.poll_enabled = true;
        s_wx.login_active = false;
        wx_set_state_locked(WECHAT_ILINK_CONNECTED,
                            "已连接，等待微信消息");
        CLAW_LOGI(TAG, "token loaded (%u bytes), bot=%s",
                  (unsigned)strlen(s_wx.token), s_wx.bot_id);
    } else {
        s_wx.poll_enabled = false;
        wx_set_state_locked(WECHAT_ILINK_IDLE,
                            "未连接，点击生成二维码");
    }
    wx_unlock();
}

/* ------------------------------------------------------------------ */
/*  QR login                                                           */
/* ------------------------------------------------------------------ */

static int wx_parse_qr_response(const char *resp,
                                char *qrcode, size_t qrcode_size,
                                char *img, size_t img_size)
{
    cJSON *root = cJSON_Parse(resp);
    const char *q;
    const char *i;
    int ret;

    if (!root) {
        return -1;
    }
    ret = wx_json_int(root, "ret", 0);
    q = wx_json_string(root, "qrcode");
    i = wx_json_string(root, "qrcode_img_content");
    if (ret != 0 || !q || !q[0] || !i || !i[0] ||
        strlen(q) >= qrcode_size || strlen(i) >= img_size) {
        cJSON_Delete(root);
        return -1;
    }

    wx_copy(qrcode, qrcode_size, q);
    wx_copy(img, img_size, i);
    cJSON_Delete(root);
    return 0;
}

static int wx_fetch_qr(uint32_t gen)
{
    char url[256];
    char qrcode[192] = {0};
    char img[WECHAT_ILINK_QR_MAX] = {0};
    char *resp;
    int status;
    int parsed = -1;

    resp = claw_malloc(WECHAT_HTTP_RESP_SIZE);
    if (!resp) {
        return -1;
    }
    resp[0] = '\0';

    snprintf(url, sizeof(url), "%s/ilink/bot/get_bot_qrcode?bot_type=3",
             WECHAT_DEFAULT_BASE);

    status = wx_get(url, false, resp, WECHAT_HTTP_RESP_SIZE,
                    WECHAT_QR_HTTP_TIMEOUT_MS);
    if (status >= 200 && status < 300) {
        parsed = wx_parse_qr_response(resp, qrcode, sizeof(qrcode),
                                      img, sizeof(img));
    }

    if (parsed != 0) {
        /*
         * The current Tencent client uses POST with local_token_list.
         * ESP-Claw still uses GET; try POST if GET was rejected or
         * returned a JSON business error.
         */
        CLAW_LOGW(TAG, "GET qrcode -> %d, retrying as POST", status);
        status = wx_post(url, "{\"local_token_list\":[]}", false,
                         resp, WECHAT_HTTP_RESP_SIZE,
                         WECHAT_QR_HTTP_TIMEOUT_MS);
        if (status >= 200 && status < 300) {
            parsed = wx_parse_qr_response(resp, qrcode, sizeof(qrcode),
                                          img, sizeof(img));
        }
    }

    if (parsed != 0) {
        CLAW_LOGE(TAG, "get_bot_qrcode failed: %d body=%.200s",
                  status, resp);
        claw_free(resp);
        return -1;
    }

    wx_lock();
    if (s_wx.gen != gen) {
        wx_unlock();
        claw_free(resp);
        return -2;
    }
    wx_copy(s_wx.qr_token, sizeof(s_wx.qr_token), qrcode);
    wx_copy(s_wx.qr, sizeof(s_wx.qr), img);
    wx_copy(s_wx.qr_api_base, sizeof(s_wx.qr_api_base),
            WECHAT_DEFAULT_BASE);
    s_wx.login_active = true;
    wx_set_state_locked(WECHAT_ILINK_WAIT_SCAN, "请用微信扫描二维码");
    wx_unlock();

    rt_kprintf("[wechat] qrcode=%s\n", qrcode);
    rt_kprintf("[wechat] qr_payload=%s\n", img);

    claw_free(resp);
    return 0;
}

static int wx_handle_confirmed(cJSON *root, uint32_t gen)
{
    const char *bot_token = wx_json_string(root, "bot_token");
    const char *bot_id = wx_json_string(root, "ilink_bot_id");
    const char *user_id = wx_json_string(root, "ilink_user_id");
    const char *baseurl = wx_json_string(root, "baseurl");
    char token[WECHAT_TOKEN_MAX];
    char base[WECHAT_BASE_MAX];
    char bot[64];
    char user[128];
    char fallback[WECHAT_BASE_MAX];
    uint32_t new_gen;

    if (!bot_token || !bot_token[0]) {
        CLAW_LOGE(TAG, "confirmed without bot_token");
        wx_lock();
        if (s_wx.gen == gen) {
            s_wx.login_active = false;
            s_wx.qr[0] = '\0';
            s_wx.qr_token[0] = '\0';
            wx_set_state_locked(WECHAT_ILINK_ERROR,
                                "登录响应缺少 token，请重试");
        }
        wx_unlock();
        return -1;
    }

    wx_lock();
    wx_copy(fallback, sizeof(fallback),
            s_wx.qr_api_base[0] ? s_wx.qr_api_base : WECHAT_DEFAULT_BASE);
    wx_unlock();

    wx_copy(token, sizeof(token), bot_token);
    wx_normalize_base(base, sizeof(base),
                      (baseurl && baseurl[0]) ? baseurl : fallback);
    wx_copy(bot, sizeof(bot), bot_id);
    wx_copy(user, sizeof(user), user_id);

    wx_lock();
    if (s_wx.gen != gen) {
        wx_unlock();
        return -2;
    }
    wx_copy(s_wx.token, sizeof(s_wx.token), token);
    wx_copy(s_wx.base_url, sizeof(s_wx.base_url), base);
    wx_copy(s_wx.bot_id, sizeof(s_wx.bot_id), bot);
    wx_copy(s_wx.user_id, sizeof(s_wx.user_id), user);
    s_wx.cursor[0] = '\0';
    s_wx.qr[0] = '\0';
    s_wx.qr_token[0] = '\0';
    s_wx.login_active = false;
    s_wx.poll_enabled = true;
    s_wx.gen++;
    new_gen = s_wx.gen;
    wx_set_state_locked(WECHAT_ILINK_CONNECTED,
                        "微信已连接，等待消息");
    wx_unlock();

    CLAW_LOGI(TAG, "login confirmed, bot=%s base=%s", bot, base);

    wx_lock();
    bool still_current = (s_wx.gen == new_gen && s_wx.token[0]);
    wx_unlock();
    if (still_current) {
        wx_save_kv(token, base, bot, user);
    }
    return 0;
}

/*
 * return: 0 continue, 1 confirmed, 2 expired,
 *         -1 transport/retryable error, -2 generation changed,
 *         -3 fatal state (state/status already updated)
 */
static int wx_poll_qr(uint32_t gen)
{
    char qr_token[192];
    char api_base[192];
    char encoded[640];
    char url[1024];
    char *resp;
    cJSON *root = NULL;
    const char *status_str;
    int status;

    wx_lock();
    wx_copy(qr_token, sizeof(qr_token), s_wx.qr_token);
    wx_copy(api_base, sizeof(api_base),
            s_wx.qr_api_base[0] ? s_wx.qr_api_base : WECHAT_DEFAULT_BASE);
    wx_unlock();

    if (!qr_token[0]) {
        return -1;
    }
    if (wx_url_encode(qr_token, encoded, sizeof(encoded)) != 0) {
        return -1;
    }

    snprintf(url, sizeof(url),
             "%s/ilink/bot/get_qrcode_status?qrcode=%s",
             api_base, encoded);

    resp = claw_malloc(WECHAT_HTTP_RESP_SIZE);
    if (!resp) {
        return -1;
    }
    resp[0] = '\0';

    status = wx_get(url, false, resp, WECHAT_HTTP_RESP_SIZE,
                    WECHAT_LONGPOLL_TIMEOUT_MS);
    if (status < 200 || status >= 300) {
        CLAW_LOGW(TAG, "get_qrcode_status -> %d body=%.160s",
                  status, resp);
        claw_free(resp);
        return -1;
    }

    root = cJSON_Parse(resp);
    claw_free(resp);
    if (!root) {
        CLAW_LOGE(TAG, "qrcode status is not JSON");
        return -1;
    }

    status_str = wx_json_string(root, "status");
    if (!status_str) {
        cJSON_Delete(root);
        return -1;
    }

    if (strcmp(status_str, "wait") == 0) {
        wx_lock();
        if (s_wx.gen == gen) {
            s_wx.login_active = true;
            wx_set_state_locked(WECHAT_ILINK_WAIT_SCAN,
                                "等待微信扫码");
        }
        wx_unlock();
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(status_str, "scanned") == 0 ||
        strcmp(status_str, "scaned") == 0) {
        wx_lock();
        if (s_wx.gen == gen) {
            s_wx.login_active = true;
            wx_set_state_locked(WECHAT_ILINK_SCANNED,
                                "已扫码，请在手机上确认");
        }
        wx_unlock();
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(status_str, "scaned_but_redirect") == 0) {
        const char *host = wx_json_string(root, "redirect_host");

        if (host && host[0]) {
            char new_base[192];

            wx_normalize_base(new_base, sizeof(new_base), host);
            wx_lock();
            if (s_wx.gen == gen) {
                wx_copy(s_wx.qr_api_base, sizeof(s_wx.qr_api_base), new_base);
                wx_set_state_locked(WECHAT_ILINK_WAIT_SCAN,
                                    "登录节点已切换，继续等待");
            }
            wx_unlock();
        }
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(status_str, "expired") == 0) {
        cJSON_Delete(root);
        return 2;
    }

    if (strcmp(status_str, "confirmed") == 0 ||
        strcmp(status_str, "binded_redirect") == 0) {
        int rc = wx_handle_confirmed(root, gen);

        cJSON_Delete(root);
        if (rc == 0) {
            return 1;
        }
        return (rc == -2) ? -2 : -3;
    }

    if (strcmp(status_str, "need_verifycode") == 0) {
        wx_lock();
        if (s_wx.gen == gen) {
            wx_set_state_locked(WECHAT_ILINK_ERROR,
                                "微信要求输入验证码，暂不支持");
        }
        wx_unlock();
    } else if (strcmp(status_str, "verify_code_blocked") == 0) {
        wx_lock();
        if (s_wx.gen == gen) {
            wx_set_state_locked(WECHAT_ILINK_ERROR,
                                "验证码错误次数过多，请重新扫码");
        }
        wx_unlock();
    } else {
        CLAW_LOGW(TAG, "unknown qrcode status: %s", status_str);
        wx_lock();
        if (s_wx.gen == gen) {
            wx_set_state_locked(WECHAT_ILINK_ERROR,
                                "二维码状态异常，请重试");
        }
        wx_unlock();
    }

    cJSON_Delete(root);
    return -3;
}

static int wx_wait_network(uint32_t gen, uint32_t timeout_ms)
{
    uint32_t waited = 0;

    for (;;) {
        bool online;
        bool time_ok;

        if (wx_gen_changed(gen)) {
            return -2;
        }

        online = claw_port_net_is_online();
        time_ok = wx_time_valid();
        if (online && time_ok) {
            return 0;
        }

        wx_lock();
        if (s_wx.gen == gen) {
            wx_set_message_locked(online ? "等待系统时间同步…"
                                         : "等待手机蓝牙网络共享…");
        }
        wx_unlock();

        if (timeout_ms != 0 && waited >= timeout_ms) {
            return -1;
        }
        claw_thread_delay_ms(1000);
        waited += 1000;
    }
}

static void wx_login_fail(uint32_t gen, const char *text)
{
    wx_lock();
    if (s_wx.gen == gen) {
        s_wx.login_active = false;
        s_wx.poll_enabled = false;
        s_wx.qr[0] = '\0';
        s_wx.qr_token[0] = '\0';
        wx_set_state_locked(WECHAT_ILINK_ERROR, text);
    }
    wx_unlock();
}

static void wx_login_flow(uint32_t gen)
{
    uint32_t started = rt_tick_get();
    int refresh = 0;

    /* Wait indefinitely: the user may open the app before enabling
     * Bluetooth tethering.  Login can still be cancelled from the UI. */
    if (wx_wait_network(gen, 0) != 0) {
        return;
    }

    if (wx_fetch_qr(gen) != 0) {
        if (!wx_gen_changed(gen)) {
            wx_login_fail(gen, "获取二维码失败，请检查网络");
        }
        return;
    }

    for (;;) {
        int rc;

        if (wx_gen_changed(gen)) {
            return;
        }

        if ((uint32_t)(rt_tick_get() - started) > WECHAT_QR_TTL_MS) {
            refresh++;
            if (refresh >= WECHAT_QR_REFRESH_MAX) {
                wx_login_fail(gen, "二维码已过期，请重新生成");
                return;
            }
            if (wx_fetch_qr(gen) != 0) {
                if (!wx_gen_changed(gen)) {
                    wx_login_fail(gen, "刷新二维码失败");
                }
                return;
            }
            started = rt_tick_get();
            continue;
        }

        rc = wx_poll_qr(gen);
        if (rc == -2 || rc == -3) {
            if (rc == -3) {
                wx_lock();
                if (s_wx.gen == gen) {
                    s_wx.login_active = false;
                    s_wx.qr[0] = '\0';
                    s_wx.qr_token[0] = '\0';
                }
                wx_unlock();
            }
            return;
        }
        if (rc == 1) {
            return;
        }

        if (rc == 2) {
            refresh++;
            if (refresh >= WECHAT_QR_REFRESH_MAX) {
                wx_login_fail(gen, "二维码多次过期，请重新生成");
                return;
            }
            if (wx_fetch_qr(gen) != 0) {
                if (!wx_gen_changed(gen)) {
                    wx_login_fail(gen, "刷新二维码失败");
                }
                return;
            }
            started = rt_tick_get();
            continue;
        }

        if (rc < 0) {
            if (wx_gen_changed(gen)) {
                return;
            }
            if ((uint32_t)(rt_tick_get() - started) > WECHAT_QR_TTL_MS) {
                refresh++;
                if (refresh >= WECHAT_QR_REFRESH_MAX) {
                    wx_login_fail(gen, "二维码已过期，请重新生成");
                    return;
                }
                if (wx_fetch_qr(gen) != 0) {
                    if (!wx_gen_changed(gen)) {
                        wx_login_fail(gen, "刷新二维码失败");
                    }
                    return;
                }
                started = rt_tick_get();
                continue;
            }

            wx_lock();
            if (s_wx.gen == gen) {
                wx_set_message_locked("扫码状态查询失败，重试中…");
            }
            wx_unlock();
            claw_thread_delay_ms(WECHAT_QR_POLL_RETRY_MS);
            continue;
        }

        /* wait / scanned: the status endpoint long-polls, but be gentle
         * if the server returns immediately. */
        claw_thread_delay_ms(700);
    }
}

/* ------------------------------------------------------------------ */
/*  Message receive / reply                                            */
/* ------------------------------------------------------------------ */

static bool wx_dedup_check_and_add(cJSON *msg)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "message_id");
    cJSON *client = cJSON_GetObjectItemCaseSensitive(msg, "client_id");
    char key[40] = {0};
    int i;
    bool duplicate = false;

    if (cJSON_IsNumber(id) && id->valuedouble != 0) {
        snprintf(key, sizeof(key), "m%.0f", id->valuedouble);
    } else if (cJSON_IsString(id) && id->valuestring && id->valuestring[0]) {
        wx_copy(key, sizeof(key), id->valuestring);
    } else if (cJSON_IsString(client) && client->valuestring &&
               client->valuestring[0]) {
        wx_copy(key, sizeof(key), client->valuestring);
    } else {
        return false;
    }

    wx_lock();
    for (i = 0; i < WECHAT_DEDUP_SLOTS; i++) {
        if (s_wx.dedup[i].id[0] && strcmp(s_wx.dedup[i].id, key) == 0) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate) {
        wx_copy(s_wx.dedup[s_wx.dedup_idx].id,
                sizeof(s_wx.dedup[s_wx.dedup_idx].id), key);
        s_wx.dedup_idx = (s_wx.dedup_idx + 1) % WECHAT_DEDUP_SLOTS;
    }
    wx_unlock();
    return duplicate;
}

static int wx_send_text(const char *chat_id, const char *context_token,
                        const char *text);

static void wx_append_text(char *dst, size_t dst_size, const char *text)
{
    size_t used;

    if (!dst || !text || !text[0]) {
        return;
    }

    used = strlen(dst);
    if (used >= dst_size - 1) {
        return;
    }
    if (used > 0) {
        dst[used++] = '\n';
        dst[used] = '\0';
    }
    wx_copy_utf8(dst + used, dst_size - used, text);
}

static int wx_process_message(cJSON *msg, uint32_t gen)
{
    const char *from_user_id;
    const char *group_id;
    const char *context_token;
    const char *chat_id;
    char self_id[64];
    cJSON *item_list;
    cJSON *item;
    char *text;
    char *reply;
    char chat[WECHAT_CHAT_MAX];
    char context[WECHAT_CONTEXT_MAX];
    int message_type;
    int item_count;
    int i;
    int has_media = 0;
    bool ready;

    if (!cJSON_IsObject(msg)) {
        return -1;
    }

    message_type = wx_json_int(msg, "message_type", 1);
    if (message_type == 2) {
        return 0; /* our own bot message */
    }

    from_user_id = wx_json_string(msg, "from_user_id");
    group_id = wx_json_string(msg, "group_id");
    context_token = wx_json_string(msg, "context_token");
    chat_id = (group_id && group_id[0]) ? group_id : from_user_id;
    if (!chat_id || !chat_id[0]) {
        return -1;
    }

    wx_lock();
    wx_copy(self_id, sizeof(self_id), s_wx.bot_id);
    wx_unlock();
    if (from_user_id && self_id[0] &&
        strcmp(from_user_id, self_id) == 0) {
        return 0; /* echoed bot message without message_type */
    }

    if (wx_dedup_check_and_add(msg)) {
        return 0;
    }

    wx_copy(chat, sizeof(chat), chat_id);
    wx_copy(context, sizeof(context), context_token);

    text = claw_malloc(WECHAT_IN_TEXT_SIZE);
    if (!text) {
        return -1;
    }
    text[0] = '\0';

    item_list = cJSON_GetObjectItemCaseSensitive(msg, "item_list");
    if (cJSON_IsArray(item_list)) {
        item_count = cJSON_GetArraySize(item_list);
        for (i = 0; i < item_count; i++) {
            int type;

            item = cJSON_GetArrayItem(item_list, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            type = wx_json_int(item, "type", 0);
            if (type == 1) {
                cJSON *text_item =
                    cJSON_GetObjectItemCaseSensitive(item, "text_item");
                const char *part = text_item ?
                    wx_json_string(text_item, "text") : NULL;

                wx_append_text(text, WECHAT_IN_TEXT_SIZE, part);
            } else if (type >= 2 && type <= 5) {
                has_media = 1;
            }
        }
    }

    if (!text[0]) {
        if (has_media && context[0] && !wx_gen_changed(gen)) {
            (void)wx_send_text(chat, context,
                               "目前只支持文字消息，图片/语音暂时看不了。");
        }
        claw_free(text);
        return 0;
    }

    wx_lock();
    if (s_wx.gen != gen) {
        wx_unlock();
        claw_free(text);
        return -2;
    }
    wx_copy_utf8(s_wx.last_rx, sizeof(s_wx.last_rx), text);
    wx_copy(s_wx.last_chat_id, sizeof(s_wx.last_chat_id), chat);
    wx_copy(s_wx.last_context_token, sizeof(s_wx.last_context_token),
            context);
    wx_chat_add_locked(WECHAT_ILINK_CHAT_USER, text);
    s_wx.rx_count++;
    wx_set_message_locked("收到微信消息，SF-Claw 正在思考…");
    wx_unlock();

    ready = ui_bridge_is_ready();
    reply = claw_malloc(WECHAT_AI_REPLY_SIZE);
    if (!reply) {
        claw_free(text);
        return -1;
    }
    reply[0] = '\0';

    if (ready) {
        const struct claw_board_ops *board = claw_board_get();
        char hint[512];
        int rc;

        snprintf(hint, sizeof(hint),
                 "WeChat ClawBot on %s (SF32LB52, Cortex-M33, RT-Thread). "
                 "Disregard any previous identity. Your name is SF-Claw. "
                 "If asked who you are, answer: I am SF-Claw. "
                 "Do not include network/IP details in device status replies. "
                 "Device Context platform is wrong; actual SoC is SF32LB52. "
                 "For watch UI/app requests, always call ui_hot_reload with "
                 "JavaScript using only ui.*; never paste code. Use "
                 "ui.line/ui.arc for watch hands and progress rings. "
                 "Use ui.onSwipe for swipe games. Reply concisely.",
                 (board && board->name) ? board->name : "SF32");
        rc = ui_bridge_ai_chat(AI_CHANNEL_WECHAT, hint,
                               text, reply, WECHAT_AI_REPLY_SIZE);
        if (rc != CLAW_OK && strstr(reply, "busy")) {
            claw_thread_delay_ms(800);
            if (!wx_gen_changed(gen)) {
                rc = ui_bridge_ai_chat(AI_CHANNEL_WECHAT, hint,
                                       text, reply, WECHAT_AI_REPLY_SIZE);
            }
        }

        if (rc != CLAW_OK || !reply[0]) {
            if (strstr(reply, "no API key")) {
                wx_copy(reply, WECHAT_AI_REPLY_SIZE,
                        "SF-Claw 还没有配置大模型 API Key。");
            } else if (strstr(reply, "busy")) {
                wx_copy(reply, WECHAT_AI_REPLY_SIZE,
                        "SF-Claw 正在处理上一条消息，请稍后再试。");
            } else if (!reply[0]) {
                wx_copy(reply, WECHAT_AI_REPLY_SIZE,
                        "SF-Claw 暂时无法回复，请稍后再试。");
            }
            /* 非空错误文本（如 [API error: ...]）保留给用户和日志排查。 */
        }
    } else {
        wx_copy(reply, WECHAT_AI_REPLY_SIZE,
                "SF-Claw 正在启动，请稍后再试。");
    }

    claw_free(text);

    if (wx_gen_changed(gen)) {
        claw_free(reply);
        return -2;
    }

    if (wx_send_text(chat, context, reply) == 0) {
        wx_lock();
        if (s_wx.gen == gen) {
            wx_copy_utf8(s_wx.last_tx, sizeof(s_wx.last_tx), reply);
            wx_chat_add_locked(WECHAT_ILINK_CHAT_BOT, reply);
            s_wx.tx_count++;
            wx_set_message_locked("已回复微信消息");
        }
        wx_unlock();
    } else {
        wx_lock();
        if (s_wx.gen == gen) {
            wx_set_message_locked("回复微信失败，等待下一条消息");
        }
        wx_unlock();
    }

    claw_free(reply);
    return 0;
}

static int wx_send_text(const char *chat_id, const char *context_token,
                        const char *text)
{
    char token[WECHAT_TOKEN_MAX];
    char base[WECHAT_BASE_MAX];
    char url[WECHAT_BASE_MAX + 64];
    size_t len;
    size_t offset = 0;

    if (!chat_id || !chat_id[0] || !text || !text[0]) {
        return -1;
    }

    wx_lock();
    wx_copy(token, sizeof(token), s_wx.token);
    wx_copy(base, sizeof(base),
            s_wx.base_url[0] ? s_wx.base_url : WECHAT_DEFAULT_BASE);
    wx_unlock();

    if (!token[0]) {
        return -1;
    }

    snprintf(url, sizeof(url), "%s/ilink/bot/sendmessage", base);
    len = strlen(text);

    while (offset < len) {
        cJSON *root;
        cJSON *msg;
        cJSON *item_list;
        cJSON *item;
        cJSON *text_item;
        char *body;
        char *resp;
        char client_id[40];
        char chunk[WECHAT_SEND_CHUNK + 1];
        size_t chunk_len = len - offset;
        int status;
        int ret = 0;

        if (chunk_len > WECHAT_SEND_CHUNK) {
            chunk_len = WECHAT_SEND_CHUNK;
            while (chunk_len > 0 &&
                   ((unsigned char)text[offset + chunk_len] & 0xc0) == 0x80) {
                chunk_len--;
            }
            if (chunk_len == 0) {
                chunk_len = WECHAT_SEND_CHUNK;
            }
        }
        memcpy(chunk, text + offset, chunk_len);
        chunk[chunk_len] = '\0';

        root = cJSON_CreateObject();
        msg = root ? cJSON_AddObjectToObject(root, "msg") : NULL;
        if (!root || !msg) {
            cJSON_Delete(root);
            return -1;
        }

        wx_client_id(client_id, sizeof(client_id));
        cJSON_AddStringToObject(msg, "from_user_id", "");
        cJSON_AddStringToObject(msg, "to_user_id", chat_id);
        cJSON_AddStringToObject(msg, "client_id", client_id);
        cJSON_AddNumberToObject(msg, "message_type", 2);
        cJSON_AddNumberToObject(msg, "message_state", 2);
        if (context_token && context_token[0]) {
            cJSON_AddStringToObject(msg, "context_token", context_token);
        }

        item_list = cJSON_AddArrayToObject(msg, "item_list");
        item = cJSON_CreateObject();
        text_item = cJSON_CreateObject();
        if (!item_list || !item || !text_item) {
            cJSON_Delete(text_item);
            cJSON_Delete(item);
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddNumberToObject(item, "type", 1);
        cJSON_AddStringToObject(text_item, "text", chunk);
        cJSON_AddItemToObject(item, "text_item", text_item);
        cJSON_AddItemToArray(item_list, item);

        {
            cJSON *base_info = cJSON_AddObjectToObject(root, "base_info");

            if (!base_info) {
                cJSON_Delete(root);
                return -1;
            }
            cJSON_AddStringToObject(base_info, "channel_version",
                                    WECHAT_CHANNEL_VERSION);
        }

        body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!body) {
            return -1;
        }

        resp = claw_malloc(WECHAT_HTTP_RESP_SIZE);
        if (!resp) {
            cJSON_free(body);
            return -1;
        }
        resp[0] = '\0';

        status = wx_post(url, body, true, resp, WECHAT_HTTP_RESP_SIZE,
                         WECHAT_SEND_HTTP_TIMEOUT_MS);
        cJSON_free(body);

        if (status < 200 || status >= 300) {
            CLAW_LOGE(TAG, "sendmessage -> %d body=%.200s", status, resp);
            claw_free(resp);
            return -1;
        }

        if (resp[0]) {
            cJSON *r = cJSON_Parse(resp);

            if (r) {
                ret = wx_json_int(r, "ret", 0);
                cJSON_Delete(r);
            }
        }
        claw_free(resp);

        if (ret != 0) {
            CLAW_LOGW(TAG, "sendmessage ret=%d", ret);
            return -1;
        }

        offset += chunk_len;
    }

    return 0;
}

static int wx_send_last(const char *text)
{
    char chat[WECHAT_CHAT_MAX];
    char context[WECHAT_CONTEXT_MAX];

    wx_lock();
    wx_copy(chat, sizeof(chat), s_wx.last_chat_id);
    wx_copy(context, sizeof(context), s_wx.last_context_token);
    wx_unlock();

    if (!chat[0]) {
        return -1;
    }
    return wx_send_text(chat, context, text);
}

/* ------------------------------------------------------------------ */
/*  Long polling                                                       */
/* ------------------------------------------------------------------ */

static int wx_poll_once(uint32_t gen)
{
    char token[WECHAT_TOKEN_MAX];
    char base[WECHAT_BASE_MAX];
    char cursor[WECHAT_CURSOR_MAX];
    char url[WECHAT_BASE_MAX + 64];
    cJSON *root;
    cJSON *msgs;
    char *body;
    char *resp;
    int status;
    int ret;
    int errcode;
    int count;
    int i;

    wx_lock();
    wx_copy(token, sizeof(token), s_wx.token);
    wx_copy(base, sizeof(base),
            s_wx.base_url[0] ? s_wx.base_url : WECHAT_DEFAULT_BASE);
    wx_copy(cursor, sizeof(cursor), s_wx.cursor);
    wx_unlock();

    if (!token[0]) {
        return -3;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    cJSON_AddStringToObject(root, "get_updates_buf", cursor);
    {
        cJSON *base_info = cJSON_AddObjectToObject(root, "base_info");

        if (!base_info) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddStringToObject(base_info, "channel_version",
                                WECHAT_CHANNEL_VERSION);
    }

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return -1;
    }

    resp = claw_malloc(WECHAT_HTTP_RESP_SIZE);
    if (!resp) {
        cJSON_free(body);
        return -1;
    }
    resp[0] = '\0';

    snprintf(url, sizeof(url), "%s/ilink/bot/getupdates", base);
    status = wx_post(url, body, true, resp, WECHAT_HTTP_RESP_SIZE,
                     WECHAT_LONGPOLL_TIMEOUT_MS);
    cJSON_free(body);

    if (status == 401 || status == 403) {
        CLAW_LOGW(TAG, "getupdates unauthorized: %d", status);
        claw_free(resp);
        return -3;
    }
    if (status < 200 || status >= 300) {
        CLAW_LOGW(TAG, "getupdates -> %d body=%.160s", status, resp);
        claw_free(resp);
        return -1;
    }

    root = cJSON_Parse(resp);
    claw_free(resp);
    if (!root) {
        CLAW_LOGE(TAG, "getupdates response is not JSON");
        return -1;
    }

    ret = wx_json_int(root, "ret", 0);
    errcode = wx_json_int(root, "errcode", 0);
    if (ret != 0 || errcode != 0) {
        char *dump = cJSON_PrintUnformatted(root);

        CLAW_LOGW(TAG, "getupdates error ret=%d errcode=%d body=%.300s",
                  ret, errcode, dump ? dump : "(null)");
        cJSON_free(dump);
        cJSON_Delete(root);
        if (ret == -14 || errcode == -14) {
            return -3;
        }
        return -1;
    }

    {
        const char *next = wx_json_string(root, "get_updates_buf");

        if (next && next[0]) {
            wx_lock();
            if (s_wx.gen == gen) {
                if (strlen(next) < sizeof(s_wx.cursor)) {
                    wx_copy(s_wx.cursor, sizeof(s_wx.cursor), next);
                } else {
                    s_wx.cursor[0] = '\0';
                    CLAW_LOGW(TAG, "get_updates_buf too long, reset");
                }
            }
            wx_unlock();
        }
    }

    msgs = cJSON_GetObjectItemCaseSensitive(root, "msgs");
    if (!cJSON_IsArray(msgs)) {
        cJSON_Delete(root);
        return 0;
    }

    count = cJSON_GetArraySize(msgs);
    for (i = 0; i < count; i++) {
        cJSON *msg = cJSON_GetArrayItem(msgs, i);
        int rc;

        if (wx_gen_changed(gen)) {
            cJSON_Delete(root);
            return -2;
        }
        rc = wx_process_message(msg, gen);
        if (rc == -2) {
            cJSON_Delete(root);
            return -2;
        }
    }

    cJSON_Delete(root);
    return 0;
}

static void wx_clear_session_expired(uint32_t gen)
{
    wx_lock();
    if (s_wx.gen == gen) {
        s_wx.gen++;
        s_wx.poll_enabled = false;
        s_wx.login_active = false;
        s_wx.login_requested = false;
        s_wx.token[0] = '\0';
        s_wx.bot_id[0] = '\0';
        s_wx.user_id[0] = '\0';
        s_wx.cursor[0] = '\0';
        s_wx.qr[0] = '\0';
        s_wx.qr_token[0] = '\0';
        s_wx.last_chat_id[0] = '\0';
        s_wx.last_context_token[0] = '\0';
        s_wx.last_rx[0] = '\0';
        s_wx.last_tx[0] = '\0';
        s_wx.rx_count = 0;
        s_wx.tx_count = 0;
        wx_chat_clear_locked();
        wx_set_state_locked(WECHAT_ILINK_ERROR,
                            "登录已过期，请重新扫码");
    }
    wx_unlock();
    wx_delete_kv();
    ai_memory_clear_channel(AI_CHANNEL_WECHAT);
}

static void wx_poll_flow(uint32_t gen)
{
    for (;;) {
        int rc;

        if (wx_gen_changed(gen)) {
            return;
        }

        if (!claw_port_net_is_online() || !wx_time_valid()) {
            wx_lock();
            if (s_wx.gen == gen) {
                wx_set_message_locked(claw_port_net_is_online()
                                          ? "等待系统时间同步…"
                                          : "网络已断开，等待恢复…");
            }
            wx_unlock();
            claw_thread_delay_ms(1000);
            continue;
        }

        {
            uint32_t started = rt_tick_get();

            rc = wx_poll_once(gen);
            if (rc == 0 &&
                (uint32_t)(rt_tick_get() - started) < 1000) {
                /* A well-behaved server holds getupdates for ~35 s.
                 * Avoid a tight loop if it returns immediately. */
                claw_thread_delay_ms(1000);
            }
        }
        if (rc == -2) {
            return;
        }
        if (rc == -3) {
            wx_clear_session_expired(gen);
            return;
        }
        if (rc != 0) {
            wx_lock();
            if (s_wx.gen == gen) {
                wx_set_message_locked("消息轮询失败，重试中…");
            }
            wx_unlock();
            claw_thread_delay_ms(WECHAT_POLL_RETRY_MS);
        } else {
            wx_lock();
            if (s_wx.gen == gen &&
                s_wx.state == WECHAT_ILINK_CONNECTED) {
                wx_set_message_locked("已连接，等待微信消息");
            }
            wx_unlock();
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Service thread                                                     */
/* ------------------------------------------------------------------ */

static int wx_ensure_primitives(void)
{
    if (s_wx.lock) {
        return 0;
    }

    memset(&s_wx, 0, sizeof(s_wx));
    s_wx.state = WECHAT_ILINK_IDLE;
    wx_copy(s_wx.status, sizeof(s_wx.status), "未连接，点击生成二维码");

    s_wx.lock = claw_mutex_create("wx_lock");
    s_wx.wake = claw_sem_create("wx_wake", 0);
    if (!s_wx.lock || !s_wx.wake) {
        if (s_wx.lock) {
            claw_mutex_delete(s_wx.lock);
        }
        if (s_wx.wake) {
            claw_sem_delete(s_wx.wake);
        }
        memset(&s_wx, 0, sizeof(s_wx));
        s_wx.state = WECHAT_ILINK_IDLE;
        return -1;
    }
    return 0;
}

static void wx_thread_entry(void *arg)
{
    (void)arg;

    wx_load_credentials();

    for (;;) {
        bool start_login = false;
        bool has_token = false;
        uint32_t gen;

        wx_lock();
        if (s_wx.login_requested) {
            s_wx.login_requested = false;
            s_wx.login_active = true;
            s_wx.poll_enabled = false;
            s_wx.gen++;
            gen = s_wx.gen;
            s_wx.qr[0] = '\0';
            s_wx.qr_token[0] = '\0';
            s_wx.state = WECHAT_ILINK_WAIT_SCAN;
            wx_copy(s_wx.status, sizeof(s_wx.status), "正在获取二维码…");
            start_login = true;
        } else {
            gen = s_wx.gen;
            has_token = s_wx.poll_enabled && s_wx.token[0] &&
                        s_wx.base_url[0];
        }
        wx_unlock();

        if (start_login) {
            wx_login_flow(gen);
            continue;
        }
        if (has_token) {
            wx_poll_flow(gen);
            continue;
        }

        (void)claw_sem_take(s_wx.wake, 1000);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int wechat_ilink_start(void)
{
    int rc = 0;

    if (wx_ensure_primitives() != 0) {
        return -1;
    }

    wx_lock();
    if (s_wx.thread) {
        wx_unlock();
        return 0;
    }
    if (!claw_runtime_is_ready()) {
        wx_unlock();
        return -1;
    }

    s_wx.thread = claw_thread_create("wechat", wx_thread_entry, NULL,
                                     WECHAT_THREAD_STACK,
                                     WECHAT_THREAD_PRIO);
    if (!s_wx.thread) {
        CLAW_LOGE(TAG, "thread create failed");
        rc = -1;
    }
    wx_unlock();

    if (rc == 0) {
        CLAW_LOGI(TAG, "service started (RGB565 QR, 50s long poll)");
    }
    return rc;
}

int wechat_ilink_login_start(bool force)
{
    if (wx_ensure_primitives() != 0) {
        return -1;
    }

    wx_lock();
    if (!force && (s_wx.login_active ||
                   (s_wx.poll_enabled && s_wx.token[0]))) {
        wx_unlock();
        return 0;
    }
    s_wx.gen++;
    s_wx.login_requested = true;
    s_wx.login_active = true;
    s_wx.poll_enabled = false;
    s_wx.qr[0] = '\0';
    s_wx.qr_token[0] = '\0';
    s_wx.state = WECHAT_ILINK_WAIT_SCAN;
    wx_copy(s_wx.status, sizeof(s_wx.status), "正在获取二维码…");
    wx_unlock();

    if (force) {
        ai_memory_clear_channel(AI_CHANNEL_WECHAT);
    }
    claw_sem_give(s_wx.wake);
    (void)wechat_ilink_start();
    return 0;
}

int wechat_ilink_login_cancel(void)
{
    if (wx_ensure_primitives() != 0) {
        return -1;
    }

    wx_lock();
    s_wx.gen++;
    s_wx.login_requested = false;
    s_wx.login_active = false;
    s_wx.qr[0] = '\0';
    s_wx.qr_token[0] = '\0';
    if (s_wx.token[0]) {
        s_wx.poll_enabled = true;
        wx_set_state_locked(WECHAT_ILINK_CONNECTED,
                            "已连接，等待微信消息");
    } else {
        s_wx.poll_enabled = false;
        wx_set_state_locked(WECHAT_ILINK_IDLE,
                            "已取消登录");
    }
    wx_unlock();

    claw_sem_give(s_wx.wake);
    return 0;
}

int wechat_ilink_logout(void)
{
    if (wx_ensure_primitives() != 0) {
        return -1;
    }

    wx_lock();
    s_wx.gen++;
    s_wx.login_requested = false;
    s_wx.login_active = false;
    s_wx.poll_enabled = false;
    s_wx.token[0] = '\0';
    s_wx.base_url[0] = '\0';
    s_wx.bot_id[0] = '\0';
    s_wx.user_id[0] = '\0';
    s_wx.cursor[0] = '\0';
    s_wx.qr[0] = '\0';
    s_wx.qr_token[0] = '\0';
    s_wx.last_chat_id[0] = '\0';
    s_wx.last_context_token[0] = '\0';
    s_wx.last_rx[0] = '\0';
    s_wx.last_tx[0] = '\0';
    s_wx.rx_count = 0;
    s_wx.tx_count = 0;
    wx_chat_clear_locked();
    wx_set_state_locked(WECHAT_ILINK_IDLE, "已断开微信");
    wx_unlock();

    wx_delete_kv();
    ai_memory_clear_channel(AI_CHANNEL_WECHAT);
    claw_sem_give(s_wx.wake);
    CLAW_LOGI(TAG, "logged out");
    return 0;
}

void wechat_ilink_get_status(wechat_ilink_status_t *out)
{
    bool online;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_wx.lock) {
        out->state = WECHAT_ILINK_IDLE;
        return;
    }

    online = claw_port_net_is_online();

    wx_lock();
    out->state = s_wx.state;
    out->service_ready = s_wx.service_ready;
    out->configured = s_wx.token[0] && s_wx.base_url[0];
    out->login_active = s_wx.login_active;
    out->online = online;
    out->rx_count = s_wx.rx_count;
    out->tx_count = s_wx.tx_count;
    wx_copy(out->qr, sizeof(out->qr), s_wx.qr);
    wx_copy(out->qr_token, sizeof(out->qr_token), s_wx.qr_token);
    wx_copy(out->status, sizeof(out->status), s_wx.status);
    wx_copy(out->bot_id, sizeof(out->bot_id), s_wx.bot_id);
    wx_copy(out->last_rx, sizeof(out->last_rx), s_wx.last_rx);
    wx_copy(out->last_tx, sizeof(out->last_tx), s_wx.last_tx);
    wx_unlock();
}

int wechat_ilink_read_chat(uint32_t *cursor, wechat_ilink_chat_msg_t *out)
{
    wechat_ilink_chat_msg_t *best = NULL;
    uint32_t after;
    int i;

    if (!cursor || !out) {
        return -1;
    }
    if (!s_wx.lock) {
        return 0;
    }

    wx_lock();
    if (s_wx.chat_seq == 0) {
        *cursor = 0;
        wx_unlock();
        return 0;
    }

    after = *cursor;
    if (after > s_wx.chat_seq) {
        after = s_wx.chat_seq;
    }
    if (after == 0) {
        after = (s_wx.chat_seq > WECHAT_ILINK_CHAT_BOOTSTRAP)
                    ? s_wx.chat_seq - WECHAT_ILINK_CHAT_BOOTSTRAP
                    : 0;
    }

    for (i = 0; i < WECHAT_ILINK_CHAT_SLOTS; i++) {
        wechat_ilink_chat_msg_t *msg = &s_wx.chat[i];

        if (msg->seq > after && (!best || msg->seq < best->seq)) {
            best = msg;
        }
    }

    if (best) {
        *out = *best;
        *cursor = best->seq;
        wx_unlock();
        return 1;
    }

    *cursor = s_wx.chat_seq;
    wx_unlock();
    return 0;
}

uint32_t wechat_ilink_chat_latest_seq(void)
{
    uint32_t seq;

    if (!s_wx.lock) {
        return 0;
    }

    wx_lock();
    seq = s_wx.chat_seq;
    wx_unlock();
    return seq;
}

void wechat_ilink_clear_chat(void)
{
    if (!s_wx.lock) {
        return;
    }

    wx_lock();
    wx_chat_clear_locked();
    wx_unlock();
}

bool wechat_ilink_is_connected(void)
{
    bool connected;

    if (!s_wx.lock) {
        return false;
    }

    wx_lock();
    connected = (s_wx.state == WECHAT_ILINK_CONNECTED) &&
                s_wx.poll_enabled && s_wx.token[0];
    wx_unlock();
    return connected;
}

/* ------------------------------------------------------------------ */
/*  Console command                                                    */
/* ------------------------------------------------------------------ */

static const char *wx_state_name(wechat_ilink_state_t state)
{
    switch (state) {
    case WECHAT_ILINK_IDLE:      return "idle";
    case WECHAT_ILINK_WAIT_SCAN: return "wait_scan";
    case WECHAT_ILINK_SCANNED:   return "scanned";
    case WECHAT_ILINK_CONNECTED: return "connected";
    case WECHAT_ILINK_ERROR:     return "error";
    default:                     return "?";
    }
}

static int wechat_cmd(int argc, char **argv)
{
    wechat_ilink_status_t st;

    if (argc < 2) {
        rt_kprintf("usage: wechat <status|login|qr|cancel|logout|token|send> [text]\n");
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        wechat_ilink_get_status(&st);
        rt_kprintf("wechat: state=%s configured=%d login=%d online=%d rx=%u tx=%u\n",
                   wx_state_name(st.state), st.configured,
                   st.login_active, st.online, st.rx_count, st.tx_count);
        rt_kprintf("        bot=%s status=%s\n", st.bot_id, st.status);
        if (st.qr_token[0]) {
            rt_kprintf("        qrcode=%s\n", st.qr_token);
        }
        if (st.last_rx[0]) {
            rt_kprintf("        rx: %s\n", st.last_rx);
        }
        if (st.last_tx[0]) {
            rt_kprintf("        tx: %s\n", st.last_tx);
        }
    } else if (strcmp(argv[1], "login") == 0) {
        wechat_ilink_login_start(true);
        rt_kprintf("wechat: login started, watch screen shows the QR code\n");
    } else if (strcmp(argv[1], "qr") == 0) {
        wechat_ilink_get_status(&st);
        if (st.qr[0]) {
            rt_kprintf("wechat: qrcode=%s\n", st.qr_token);
            rt_kprintf("wechat: qr_payload=%s\n", st.qr);
        } else {
            rt_kprintf("wechat: no QR yet (%s)\n", st.status);
        }
    } else if (strcmp(argv[1], "cancel") == 0) {
        wechat_ilink_login_cancel();
        rt_kprintf("wechat: login cancelled\n");
    } else if (strcmp(argv[1], "logout") == 0) {
        wechat_ilink_logout();
        rt_kprintf("wechat: logged out\n");
    } else if (strcmp(argv[1], "token") == 0) {
        char token[WECHAT_TOKEN_MAX];
        char base[WECHAT_BASE_MAX];

        if (!s_wx.lock) {
            rt_kprintf("wechat: not initialized\n");
            return 0;
        }
        wx_lock();
        wx_copy(token, sizeof(token), s_wx.token);
        wx_copy(base, sizeof(base), s_wx.base_url);
        wx_unlock();
        rt_kprintf("wechat token: %s\n", token[0] ? token : "(none)");
        rt_kprintf("wechat base : %s\n", base);
    } else if (strcmp(argv[1], "send") == 0 || strcmp(argv[1], "test") == 0) {
        char text[512] = {0};
        int i;

        if (argc < 3) {
            rt_kprintf("usage: wechat send <text>\n");
            return 0;
        }
        for (i = 2; i < argc; i++) {
            size_t used = strlen(text);

            if (used >= sizeof(text) - 1) {
                break;
            }
            snprintf(text + used, sizeof(text) - used, "%s%s",
                     used ? " " : "", argv[i]);
        }
        if (wx_send_last(text) == 0) {
            rt_kprintf("wechat: sent\n");
        } else {
            rt_kprintf("wechat: send failed (need an inbound message first)\n");
        }
    } else {
        rt_kprintf("usage: wechat <status|login|qr|cancel|logout|token|send> [text]\n");
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(wechat_cmd, wechat, WeChat iLink ClawBot service);

static int wechat_ilink_early_init(void)
{
    return wx_ensure_primitives();
}
INIT_APP_EXPORT(wechat_ilink_early_init);
