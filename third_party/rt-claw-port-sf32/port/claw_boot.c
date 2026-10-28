/**
 * @file claw_boot.c
 * @brief Start rt-claw from a background thread after the project UI is up.
 *
 * This file is board-independent.  Board-specific work is reached through
 * claw_board_ops (net_prepare, kv_partition, psram_size, early_init), and
 * the project is notified through the weak claw_port_on_ready() hook.
 */

#include <rtthread.h>
#include <rtconfig.h>

#include "osal/claw_os.h"
#include "osal/claw_kv.h"
#include "claw/claw_init.h"
#include "claw/tools/claw_tools.h"
#include "cJSON.h"

#if defined(PKG_USING_MBEDTLS) && defined(MBEDTLS_PLATFORM_MEMORY)
#include "mbedtls/platform.h"
#endif

#include "claw_port.h"
#include "claw_port_boot.h"
#include "claw_port_cfg.h"
#include "claw_port_mem.h"
#include "claw_port_time.h"

#define TAG "claw_boot"

#ifndef CLAW_PORT_BOOT_STACK
#define CLAW_PORT_BOOT_STACK 6144
#endif
#ifndef CLAW_PORT_BOOT_PRIO
#define CLAW_PORT_BOOT_PRIO 24
#endif

static volatile int s_runtime_ready;

/*
 * cJSON is the biggest source of small heap nodes during an AI request.
 * Route every cJSON allocation to PSRAM; only fall back to the SRAM heap
 * if the PSRAM arena is unavailable or full.
 */
static void *claw_cjson_malloc(size_t size)
{
#ifdef RTCLAW_PORT_PSRAM
    void *p = claw_port_mem_alloc_any(size);
    if (p) {
        return p;
    }
#endif
    return rt_malloc(size);
}

static void claw_cjson_free(void *ptr)
{
    claw_free(ptr);
}

static void claw_runtime_thread(void *arg)
{
    const struct claw_board_ops *board = claw_board_get();
    int kv_ok;
    (void)arg;

    /* Registration constructors already ran; console is up by now. */
    claw_log_set_enabled(1);

    if (board && board->early_init) {
        board->early_init();
    }

#ifdef RTCLAW_PORT_PSRAM
    claw_port_mem_init();
#endif

#if defined(PKG_USING_MBEDTLS) && defined(MBEDTLS_PLATFORM_MEMORY) && \
    defined(RTCLAW_PORT_PSRAM)
    /*
     * TLS needs two ~16KB I/O buffers plus contexts.  Keep them out of the
     * fragmented SRAM heap; claw_calloc routes >=256B allocations to the
     * PSRAM memheap and claw_free handles PSRAM/SRAM pointers.
     */
    mbedtls_platform_set_calloc_free(claw_calloc, claw_free);
#endif

    kv_ok = (claw_kv_init() == CLAW_OK);
    if (!kv_ok) {
        CLAW_LOGW(TAG, "KV init failed; settings will not persist");
    }

    claw_port_ai_config_load();

    /*
     * cJSON documents can be large; point it at the PSRAM-aware
     * allocator before claw_init() starts the AI service.
     */
    cJSON_InitHooks(&(cJSON_Hooks){
        .malloc_fn = claw_cjson_malloc,
        .free_fn   = claw_cjson_free,
    });

#ifdef RTCLAW_PORT_SNTP
    claw_port_time_start();
#endif

    /*
     * Collect the linker-section tools first, then replace the upstream
     * RT-Thread system_info/memory_info implementations with SF32-aware
     * ones.  claw_init() will see s_collected and skip a second scan.
     */
    claw_tool_core_collect_from_section();
    claw_port_tools_override();

    if (claw_init() != CLAW_OK) {
        CLAW_LOGE(TAG, "claw_init failed");
        claw_port_on_ready(0);
        return;
    }

    {
        rt_size_t total = 0, used = 0;
        rt_memory_info(&total, &used, RT_NULL);
#ifdef RTCLAW_PORT_PSRAM
        {
            size_t pt = 0, pu = 0, pf = 0, pl = 0;
            claw_port_mem_info(&pt, &pu, &pf, &pl);
            CLAW_LOGI(TAG,
                      "heap after init: sram used=%u free=%u | psram used=%u free=%u",
                      (unsigned)used, (unsigned)(total - used),
                      (unsigned)pu, (unsigned)pf);
        }
#else
        CLAW_LOGI(TAG, "heap after init: sram used=%u free=%u",
                  (unsigned)used, (unsigned)(total - used));
#endif
    }

    s_runtime_ready = 1;
    CLAW_LOGI(TAG, "SF-Claw ready on %s, tools=%d",
              (board && board->name) ? board->name : "sf32",
              claw_tools_count());

    claw_port_on_ready(1);
}

void claw_runtime_start(void)
{
    static int started;
    rt_thread_t tid;

    if (started) {
        return;
    }
    started = 1;

    tid = rt_thread_create("claw_boot", claw_runtime_thread, RT_NULL,
                           CLAW_PORT_BOOT_STACK, CLAW_PORT_BOOT_PRIO, 10);
    if (!tid) {
        CLAW_LOGE(TAG, "boot thread create failed");
        claw_port_on_ready(0);
        return;
    }
    rt_thread_startup(tid);
}

int claw_runtime_is_ready(void)
{
    return s_runtime_ready;
}
