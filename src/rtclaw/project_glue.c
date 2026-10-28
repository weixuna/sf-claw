/**
 * @file project_glue.c
 * @brief Project-level hooks from the reusable SF32 port to the watch UI.
 */

#include "claw_port.h"
#include "ui_bridge.h"
#include "wechat_ilink.h"

void claw_port_on_ready(int ok)
{
    ui_bridge_set_ai_ready(ok);

    if (ok) {
        /*
         * KV and SNTP are ready by the time the port calls this hook.
         * Starting here lets a persisted WeChat token resume polling
         * without a new QR scan.  If the user already opened the
         * WeChat app while boot was running, its login request is
         * queued and the service thread will pick it up immediately.
         */
        (void)wechat_ilink_start();
    }
}
