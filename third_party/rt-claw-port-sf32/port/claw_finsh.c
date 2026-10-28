/**
 * @file claw_finsh.c
 * @brief Board-independent UART bring-up commands for the SF32 port.
 */

#include <rtthread.h>
#include <rtconfig.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "osal/claw_os.h"
#include "osal/claw_net.h"
#include "claw/services/ai/ai_engine.h"
#include "claw/services/ai/ai_memory.h"
#include "claw/tools/claw_tools.h"

#include "claw_port.h"
#include "claw_port_cfg.h"
#include "claw_port_mem.h"
#include "claw_port_net.h"

#define CLAW_TEST_TEXT_MAX 512
#define CLAW_TEST_REPLY_MAX 4096
#define CLAW_NET_TEST_BUF 4096

static const char *claw_time_str(char *buf, size_t size)
{
    time_t now = time(RT_NULL);
    struct tm tm_now;

    if (!localtime_r(&now, &tm_now)) {
        snprintf(buf, size, "invalid");
        return buf;
    }
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    return buf;
}

static int net_test_cmd(int argc, char **argv)
{
    const char *url = (argc > 1) ? argv[1]
                                 : "https://api.github.com/zen";
    char *resp = claw_malloc(CLAW_NET_TEST_BUF);
    size_t len = 0;
    uint32_t t0 = claw_tick_ms();
    int rc;

    if (!resp) {
        rt_kprintf("net_test: no memory\n");
        return 0;
    }

    rc = claw_net_get(url, RT_NULL, 0, resp, CLAW_NET_TEST_BUF, &len);
    rt_kprintf("net_test rc=%d len=%u time=%ums\n",
               rc, (unsigned)len,
               (unsigned)(claw_tick_ms() - t0));
    if (rc > 0) {
        rt_kprintf("%.*s\n", 200, resp);
    }

    claw_free(resp);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(net_test_cmd, net_test, test HTTPS GET request);

static int claw_test_cmd(int argc, char **argv)
{
    char text[CLAW_TEST_TEXT_MAX];
    char *reply;
    size_t off = 0;
    int rc;

    if (argc < 2) {
        rt_kprintf("usage: claw_test <text>\n");
        return 0;
    }

    text[0] = '\0';
    for (int i = 1; i < argc; i++) {
        int n = snprintf(text + off, sizeof(text) - off, "%s%s",
                         i > 1 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(text) - off) {
            break;
        }
        off += (size_t)n;
    }

    reply = claw_malloc(CLAW_TEST_REPLY_MAX);
    if (!reply) {
        rt_kprintf("claw_test: no memory\n");
        return 0;
    }

    ai_set_channel(AI_CHANNEL_SHELL);
    {
        const struct claw_board_ops *board = claw_board_get();
        char hint[512];
        snprintf(hint, sizeof(hint),
                 "\nserial console on %s (SF32LB52, Cortex-M33, RT-Thread). "
                 "Device Context platform is wrong; actual SoC is SF32LB52. "
                 "Disregard any previous identity. Your name is SF-Claw. "
                     "If asked who you are, answer: I am SF-Claw. "
                     "Do not include network/IP details in device status replies. "
                 "For local time, call system_info.",
                 (board && board->name) ? board->name : "SF32");
        ai_set_channel_hint(hint);
    }

    rc = ai_chat(text, reply, CLAW_TEST_REPLY_MAX);
    rt_kprintf("claw_test rc=%d\n%s\n", rc, reply);

    claw_free(reply);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(claw_test_cmd, claw_test, test sf-claw chat);

static int claw_ping_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("claw_ping: %s\n", ai_ping() == CLAW_OK ? "ok" : "failed");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(claw_ping_cmd, claw_ping, test LLM API reachability);

static int claw_cfg_cmd(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf("usage: claw_cfg <show|url|key|model|save|load> [value]\n");
        return 0;
    }

    if (strcmp(argv[1], "show") == 0) {
        char masked[64];
        claw_port_ai_key_masked(masked, sizeof(masked));
        rt_kprintf("url:   %s\n", ai_get_api_url());
        rt_kprintf("key:   %s\n", masked);
        rt_kprintf("model: %s\n", ai_get_model());
    } else if (strcmp(argv[1], "url") == 0) {
        if (argc < 3) {
            rt_kprintf("url: %s\n", ai_get_api_url());
        } else {
            rt_kprintf("save: %s\n",
                       claw_port_ai_config_set(argv[2], RT_NULL, RT_NULL)
                           == CLAW_OK ? "ok" : "failed");
        }
    } else if (strcmp(argv[1], "key") == 0) {
        if (argc < 3) {
            rt_kprintf("usage: claw_cfg key <api-key>\n");
        } else {
            rt_kprintf("save: %s\n",
                       claw_port_ai_config_set(RT_NULL, argv[2], RT_NULL)
                           == CLAW_OK ? "ok" : "failed");
        }
    } else if (strcmp(argv[1], "model") == 0) {
        if (argc < 3) {
            rt_kprintf("model: %s\n", ai_get_model());
        } else {
            rt_kprintf("save: %s\n",
                       claw_port_ai_config_set(RT_NULL, RT_NULL, argv[2])
                           == CLAW_OK ? "ok" : "failed");
        }
    } else if (strcmp(argv[1], "save") == 0) {
        rt_kprintf("save: %s\n",
                   claw_port_ai_config_save() == CLAW_OK ? "ok" : "failed");
    } else if (strcmp(argv[1], "load") == 0) {
        claw_port_ai_config_load();
        rt_kprintf("loaded\n");
    } else {
        rt_kprintf("unknown option: %s\n", argv[1]);
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(claw_cfg_cmd, claw_cfg, configure sf-claw LLM settings);

static int claw_status_cmd(int argc, char **argv)
{
    const struct claw_board_ops *board = claw_board_get();
    char masked[64];
    char tbuf[32];
    rt_size_t total = 0, used = 0, max_used = 0;

    (void)argc;
    (void)argv;

    claw_port_ai_key_masked(masked, sizeof(masked));
    rt_memory_info(&total, &used, &max_used);

    rt_kprintf("board:  %s\n",
               (board && board->name) ? board->name : "sf32");
    rt_kprintf("net:    %s (%s)\n",
               claw_port_net_is_online() ? "online" : "offline",
               (board && board->netdev_name) ? board->netdev_name : "e0");
    rt_kprintf("time:   %s\n", claw_time_str(tbuf, sizeof(tbuf)));
    rt_kprintf("tools:  %d\n", claw_tools_count());
    rt_kprintf("msgs:   %d\n", ai_memory_count());
    rt_kprintf("url:    %s\n", ai_get_api_url());
    rt_kprintf("key:    %s\n", masked);
    rt_kprintf("model:  %s\n", ai_get_model());
    rt_kprintf("sram:   total=%u used=%u max=%u free=%u\n",
               (unsigned)total, (unsigned)used, (unsigned)max_used,
               (unsigned)(total - used));
#ifdef RTCLAW_PORT_PSRAM
    {
        size_t pt = 0, pu = 0, pf = 0, pl = 0;
        claw_port_mem_info(&pt, &pu, &pf, &pl);
        rt_kprintf("psram:  total=%u used=%u free=%u\n",
                   (unsigned)pt, (unsigned)pu, (unsigned)pf);
    }
#endif

    return 0;
}
MSH_CMD_EXPORT_ALIAS(claw_status_cmd, claw_status, show sf-claw status);

static int claw_mem_cmd(int argc, char **argv)
{
    rt_size_t total = 0, used = 0, max_used = 0;

    (void)argc;
    (void)argv;

    rt_memory_info(&total, &used, &max_used);
    rt_kprintf("sram:  total=%u used=%u max=%u free=%u\n",
               (unsigned)total, (unsigned)used, (unsigned)max_used,
               (unsigned)(total - used));
#ifdef RTCLAW_PORT_PSRAM
    {
        size_t pt = 0, pu = 0, pf = 0, pl = 0;
        claw_port_mem_info(&pt, &pu, &pf, &pl);
        rt_kprintf("psram: total=%u used=%u free=%u largest~%u\n",
                   (unsigned)pt, (unsigned)pu, (unsigned)pf, (unsigned)pl);
    }
#endif
    return 0;
}
MSH_CMD_EXPORT_ALIAS(claw_mem_cmd, claw_mem, show sf-claw memory usage);

#ifdef PKG_NETUTILS_NTP
static int claw_ntp_cmd(int argc, char **argv)
{
    extern time_t ntp_sync_to_rtc(const char *host_name);
    const char *host = (argc > 1) ? argv[1] : RT_NULL;

    rt_kprintf("claw_ntp: %s\n",
               ntp_sync_to_rtc(host) > 0 ? "ok" : "failed");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(claw_ntp_cmd, claw_ntp, sync RTC via SNTP);
#endif
