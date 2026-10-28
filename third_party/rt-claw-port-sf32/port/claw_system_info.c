/**
 * @file claw_system_info.c
 * @brief SF32-specific override for the upstream system_info/memory_info tools.
 *
 * The upstream RT-Thread branch of tool_system.c is written for the
 * vexpress-a9 QEMU reference and returns a hard-coded platform string.  The
 * upstream tree stays pristine; this file finds the already-registered
 * tools and swaps only their execute ops.
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "claw_config.h"
#include "claw/core/claw_tool.h"
#include "claw/tools/claw_tools.h"
#include "claw_port.h"
#include "claw_port_mem.h"
#include "cJSON.h"

#if defined(SF32LB52X)
#include "bf0_hal.h"
#endif

#ifndef SIFLI_VERSION
#define SIFLI_VERSION 0
#endif

static const char *board_name(void)
{
    const struct claw_board_ops *board = claw_board_get();

    return (board && board->name) ? board->name : "sf32";
}

static const char *chip_name(void)
{
#if defined(SF32LB52X)
    return "SF32LB52";
#elif defined(SF32LB55X)
    return "SF32LB55";
#else
    return "SF32";
#endif
}

static const char *revision_name(uint8_t rev)
{
#if defined(HAL_CHIP_REV_ID_A3)
    if (rev == HAL_CHIP_REV_ID_A3) {
        return "A3";
    }
#endif
#if defined(HAL_CHIP_REV_ID_A4)
    if (rev == HAL_CHIP_REV_ID_A4) {
        return "A4";
    }
#endif
#if defined(HAL_CHIP_REV_ID_B4)
    if (rev == HAL_CHIP_REV_ID_B4) {
        return "B4";
    }
#endif
    return "unknown";
}

static void sdk_version(char *buf, size_t size)
{
    unsigned v = (unsigned)SIFLI_VERSION;

    if (v == 0) {
        snprintf(buf, size, "unknown");
        return;
    }
    snprintf(buf, size, "%u.%u.%u",
             (v >> 24) & 0xffU,
             (v >> 16) & 0xffU,
             (v >> 8) & 0xffU);
}

static claw_err_t sf32_system_info_exec(struct claw_tool *tool,
                                        const cJSON *params,
                                        cJSON *result)
{
    char sdk[24];
    const struct claw_board_ops *board = claw_board_get();
    rt_size_t total = 0;
    rt_size_t used = 0;

    (void)tool;
    (void)params;

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "project", "SF-Claw");
    cJSON_AddStringToObject(result, "product", "SF-Claw");
    cJSON_AddStringToObject(result, "name", "SF-Claw");
    cJSON_AddStringToObject(result, "assistant_name", "SF-Claw");
    cJSON_AddStringToObject(result, "version", RT_CLAW_VERSION);
    cJSON_AddStringToObject(result, "firmware", "SF-Claw " RT_CLAW_VERSION);
    cJSON_AddStringToObject(result, "platform", board_name());
    cJSON_AddStringToObject(result, "board", board_name());
    cJSON_AddStringToObject(result, "chip", chip_name());
    cJSON_AddStringToObject(result, "cpu", "Cortex-M33");
    cJSON_AddStringToObject(result, "execution_core", "HCPU");
    cJSON_AddNumberToObject(result, "cores", 2);

#if defined(SF32LB52X)
    {
        uint8_t rev = (uint8_t)__HAL_SYSCFG_GET_REVID();

        cJSON_AddNumberToObject(result, "revision", rev);
        cJSON_AddStringToObject(result, "revision_name",
                                revision_name(rev));
    }
#endif

    cJSON_AddStringToObject(result, "rtos", "RT-Thread");
    cJSON_AddNumberToObject(result, "rtos_version",
                            RT_VERSION_MAJOR * 10000 +
                            RT_VERSION_MINOR * 100 +
                            RT_VERSION_PATCH);
    cJSON_AddNumberToObject(result, "uptime_seconds",
                            (double)rt_tick_get() / RT_TICK_PER_SECOND);

    {
        time_t now = time(RT_NULL);

        if (now > 1700000000) {
            struct tm tm_now;

            if (localtime_r(&now, &tm_now) != RT_NULL) {
                char tbuf[32];

                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S",
                         &tm_now);
                cJSON_AddStringToObject(result, "local_time", tbuf);
                cJSON_AddBoolToObject(result, "time_synced", 1);
            }
        } else {
            cJSON_AddBoolToObject(result, "time_synced", 0);
        }
    }

    sdk_version(sdk, sizeof(sdk));
    cJSON_AddStringToObject(result, "sdk", sdk);
    cJSON_AddStringToObject(result, "build", __DATE__ " " __TIME__);

    rt_memory_info(&total, &used, RT_NULL);
    cJSON_AddNumberToObject(result, "free_heap_bytes",
                            (double)(total - used));

#ifdef RTCLAW_PORT_PSRAM
    {
        size_t pt = 0, pu = 0, pf = 0, pl = 0;

        claw_port_mem_info(&pt, &pu, &pf, &pl);
        cJSON_AddNumberToObject(result, "psram_free_bytes", (double)pf);
    }
#endif


    return CLAW_OK;
}

static claw_err_t sf32_memory_info_exec(struct claw_tool *tool,
                                        const cJSON *params,
                                        cJSON *result)
{
    rt_size_t total = 0;
    rt_size_t used = 0;
    rt_size_t max_used = 0;

    (void)tool;
    (void)params;

    rt_memory_info(&total, &used, &max_used);

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "heap_total_bytes", (double)total);
    cJSON_AddNumberToObject(result, "heap_used_bytes", (double)used);
    cJSON_AddNumberToObject(result, "heap_free_bytes",
                            (double)(total - used));
    cJSON_AddNumberToObject(result, "heap_max_used_bytes",
                            (double)max_used);
    cJSON_AddNumberToObject(result, "heap_min_free_bytes",
                            (double)(total - max_used));
    if (total != 0) {
        cJSON_AddNumberToObject(result, "heap_used_percent",
                                (double)used * 100.0 / (double)total);
    }

#ifdef RTCLAW_PORT_PSRAM
    {
        size_t pt = 0, pu = 0, pf = 0, pl = 0;

        claw_port_mem_info(&pt, &pu, &pf, &pl);
        cJSON_AddNumberToObject(result, "psram_total_bytes", (double)pt);
        cJSON_AddNumberToObject(result, "psram_used_bytes", (double)pu);
        cJSON_AddNumberToObject(result, "psram_free_bytes", (double)pf);
    }
#endif

    return CLAW_OK;
}

static const struct claw_tool_ops s_system_info_ops = {
    .execute = sf32_system_info_exec,
};

static const struct claw_tool_ops s_memory_info_ops = {
    .execute = sf32_memory_info_exec,
};

void claw_port_tools_override(void)
{
    struct claw_tool *tool;

    tool = (struct claw_tool *)claw_tool_find("system_info");
    if (tool) {
        tool->ops = &s_system_info_ops;
        tool->description =
            "Get system information: board, chip model, CPU/core count, "
            "revision, firmware/SDK version, local time, and uptime in "
            "seconds.";
    }

    tool = (struct claw_tool *)claw_tool_find("memory_info");
    if (tool) {
        tool->ops = &s_memory_info_ops;
        tool->description =
            "Get heap memory status: SRAM total/used/free/peak, and PSRAM "
            "total/used/free on this board.";
    }
}
