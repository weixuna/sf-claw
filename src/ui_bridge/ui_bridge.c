/**
 * @file ui_bridge.c
 * @brief Queue-based bridge: LVGL only ever runs on the GUI thread.
 */

#include <rtthread.h>
#include <finsh.h>
#include <string.h>
#include <stdio.h>

#include "gui_app_fwk.h"
#include "osal/claw_os.h"
#include "claw/services/ai/ai_engine.h"

#include "claw_port.h"
#include "claw_port_net.h"
#include "ui_bridge.h"

#define TAG "ui_bridge"

#define CLAW_PORT_UI_REQ_MAX    512
#define CLAW_PORT_UI_REPLY_MAX  4096
#define CLAW_PORT_UI_AI_STACK   4096
#define CLAW_PORT_UI_AI_PRIO    22

static struct claw_mq *s_req_q;
static struct claw_mq *s_evt_q;
static struct claw_thread *s_worker;
static struct claw_mutex *s_ai_lock;
static volatile int s_ready;
static volatile int s_ai_ready;
static volatile int s_busy;

/* ---------------- weak UI sinks ---------------- */

__attribute__((weak)) void chat_ui_on_status(int status, const char *detail)
{
    (void)status;
    (void)detail;
}

__attribute__((weak)) void chat_ui_on_reply(const char *text)
{
    (void)text;
}

__attribute__((weak)) void chat_ui_on_error(const char *text)
{
    (void)text;
}

__attribute__((weak)) void chat_ui_on_net(int online)
{
    (void)online;
}

/* ---------------- event helpers ---------------- */

static void ui_post(ui_evt_type_t type, int value, char *text)
{
    ui_event_t ev = {
        .type = type,
        .value = value,
        .text = text,
    };

    if (!s_evt_q ||
        claw_mq_send(s_evt_q, &ev, sizeof(ev), CLAW_NO_WAIT) != CLAW_OK) {
        if (text) {
            claw_free(text);
        }
    }
}

static void ui_status_cb(int status, const char *detail)
{
    char *copy = NULL;

    if (detail) {
        size_t n = strlen(detail) + 1;
        copy = claw_malloc(n);
        if (copy) {
            memcpy(copy, detail, n);
        }
    }

    s_busy = (status != AI_STATUS_DONE);
    ui_post(UI_EVT_AGENT_STATUS, status, copy);
}

/* ---------------- AI worker ---------------- */

static void ui_ai_worker(void *arg)
{
    char req[CLAW_PORT_UI_REQ_MAX];
    (void)arg;

    for (;;) {
        if (claw_mq_recv(s_req_q, req, sizeof(req),
                         CLAW_WAIT_FOREVER) != CLAW_OK) {
            continue;
        }

        char *reply = claw_malloc(CLAW_PORT_UI_REPLY_MAX);
        if (!reply) {
            s_busy = 0;
            ui_post(UI_EVT_AGENT_ERROR, CLAW_ERR_NOMEM, NULL);
            continue;
        }

        {
            const struct claw_board_ops *board = claw_board_get();
            char hint[512];
            snprintf(hint, sizeof(hint),
                     "\nwatch chat UI on %s (SF32LB52, Cortex-M33, RT-Thread). "
                     "Device Context platform is wrong; actual SoC is SF32LB52. "
                     "Disregard any previous identity. Your name is SF-Claw. "
                     "If asked who you are, answer: I am SF-Claw. "
                     "Do not include network/IP details in device status replies. "
                     "For watch UI/app requests always call ui_hot_reload "
                     "(ui.* only; never paste code). Use ui.line/ui.arc for "
                     "watch hands and progress rings. Use ui.onSwipe for "
                     "swipe games. For local time, call system_info.",
                     (board && board->name) ? board->name : "SF32");
            int rc = ui_bridge_ai_chat(AI_CHANNEL_UI, hint, req, reply,
                                       CLAW_PORT_UI_REPLY_MAX);
            s_busy = 0;

            if (rc == CLAW_OK) {
                ui_post(UI_EVT_AGENT_REPLY, 0, reply);
            } else {
                char *copy = claw_malloc(strlen(reply) + 1);
                if (copy) {
                    strcpy(copy, reply);
                }
                claw_free(reply);
                ui_post(UI_EVT_AGENT_ERROR, rc, copy);
            }
        }
    }
}

/* ---------------- public API ---------------- */

int ui_bridge_init(void)
{
    if (s_ready) {
        return 0;
    }

    s_req_q = claw_mq_create("ui_req", CLAW_PORT_UI_REQ_MAX, 4);
    s_evt_q = claw_mq_create("ui_evt", sizeof(ui_event_t), 8);
    s_ai_lock = claw_mutex_create("ui_ai_lock");
    if (!s_req_q || !s_evt_q || !s_ai_lock) {
        CLAW_LOGE(TAG, "queue/mutex create failed");
        return -1;
    }

    ai_set_status_cb(ui_status_cb);

    s_worker = claw_thread_create("ui_ai", ui_ai_worker, NULL,
                                  CLAW_PORT_UI_AI_STACK,
                                  CLAW_PORT_UI_AI_PRIO);
    if (!s_worker) {
        CLAW_LOGE(TAG, "worker create failed");
        return -1;
    }

    s_ready = 1;
    CLAW_LOGI(TAG, "ready");
    return 0;
}

void ui_bridge_set_ai_ready(int ready)
{
    s_ai_ready = ready ? 1 : 0;
}

int ui_bridge_ai_chat(int channel, const char *hint,
                      const char *text, char *reply, size_t reply_size)
{
    int rc;

    if (!s_ai_lock || !text || !reply || reply_size == 0) {
        return -1;
    }

    claw_mutex_lock(s_ai_lock, CLAW_WAIT_FOREVER);
    ai_set_channel(channel);
    ai_set_channel_hint(hint);
    rc = ai_chat(text, reply, reply_size);
    claw_mutex_unlock(s_ai_lock);

    return rc;
}

int ui_bridge_send_text(const char *text)
{
    char req[CLAW_PORT_UI_REQ_MAX];

    if (!s_ready || !s_ai_ready || !text || !text[0]) {
        return -1;
    }
    if (s_busy) {
        return -2;
    }

    snprintf(req, sizeof(req), "%s", text);
    s_busy = 1;

    if (claw_mq_send(s_req_q, req, sizeof(req), CLAW_NO_WAIT) != CLAW_OK) {
        s_busy = 0;
        return -3;
    }

    return 0;
}

void ui_bridge_poll(void)
{
    static uint32_t last_net_check;
    static int last_online = -1;
    ui_event_t ev;

    if (!s_ready) {
        return;
    }

    uint32_t now = rt_tick_get();
    if (last_net_check == 0 || (now - last_net_check) >=
        rt_tick_from_millisecond(1000)) {
        int online = claw_port_net_is_online() ? 1 : 0;
        last_net_check = now;
        if (online != last_online) {
            last_online = online;
            ui_post(UI_EVT_NET_STATUS, online, NULL);
        }
    }

    while (claw_mq_recv(s_evt_q, &ev, sizeof(ev), CLAW_NO_WAIT) == CLAW_OK) {
        switch (ev.type) {
        case UI_EVT_AGENT_STATUS:
            chat_ui_on_status(ev.value, ev.text);
            break;
        case UI_EVT_AGENT_REPLY:
            chat_ui_on_reply(ev.text);
            break;
        case UI_EVT_AGENT_ERROR:
            chat_ui_on_error(ev.text);
            break;
        case UI_EVT_NET_STATUS:
            chat_ui_on_net(ev.value);
            break;
        default:
            break;
        }

        if (ev.text) {
            claw_free(ev.text);
        }
    }
}

int ui_bridge_is_ready(void)
{
    return s_ai_ready;
}

int ui_bridge_is_busy(void)
{
    return s_busy;
}

static int claw_ui_cmd(int argc, char **argv)
{
    const char *app = (argc > 1) ? argv[1] : "Chat";
    int rc = gui_app_run(app);

    rt_kprintf("claw_ui: %s -> %d\n", app, rc);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(claw_ui_cmd, claw_ui, open the Chat UI);
