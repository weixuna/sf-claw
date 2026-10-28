/**
 * @file ui_bridge.h
 * @brief Thread-safe bridge between rt-claw AI and the LVGL chat UI.
 */

#ifndef UI_BRIDGE_H
#define UI_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    UI_EVT_AGENT_STATUS = 1,
    UI_EVT_AGENT_REPLY,
    UI_EVT_AGENT_ERROR,
    UI_EVT_NET_STATUS,
} ui_evt_type_t;

typedef struct {
    ui_evt_type_t type;
    int           value;
    char         *text;   /* heap text, receiver frees */
} ui_event_t;

int  ui_bridge_init(void);
void ui_bridge_set_ai_ready(int ready);
int  ui_bridge_send_text(const char *text);
void ui_bridge_poll(void);
int  ui_bridge_is_ready(void);
int  ui_bridge_is_busy(void);

/*
 * Serialize ai_set_channel()/ai_set_channel_hint()/ai_chat() across the
 * UI worker and the WeChat service.  The AI engine captures the channel
 * when ai_chat() is called, so the two calls must not interleave.
 */
int  ui_bridge_ai_chat(int channel, const char *hint,
                       const char *text, char *reply, size_t reply_size);

/*
 * Weak UI sinks, overridden by the Chat app.  All of them are called
 * from the GUI thread by ui_bridge_poll().
 */
void chat_ui_on_status(int status, const char *detail);
void chat_ui_on_reply(const char *text);
void chat_ui_on_error(const char *text);
void chat_ui_on_net(int online);

#endif /* UI_BRIDGE_H */
