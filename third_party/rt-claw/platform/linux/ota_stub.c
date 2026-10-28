/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * OTA platform stub — weak defaults for platforms without OTA.
 * Unit tests and real implementations may override these.
 */

#include "claw_ota.h"
#include "claw_config.h"
#include "osal/claw_os.h"

#define TAG "ota_stub"

__attribute__((weak))
int claw_ota_platform_init(void)
{
    CLAW_LOGI(TAG, "OTA not supported on this platform");
    return CLAW_OK;
}

__attribute__((weak))
int claw_ota_do_update(const char *url,
                       claw_ota_progress_cb progress)
{
    (void)url;
    (void)progress;
    return CLAW_ERROR;
}

__attribute__((weak))
int claw_ota_rollback(void)
{
    return CLAW_ERROR;
}

__attribute__((weak))
int claw_ota_mark_valid(void)
{
    return CLAW_OK;
}

__attribute__((weak))
const char *claw_ota_running_version(void)
{
    return RT_CLAW_VERSION;
}

__attribute__((weak))
int claw_ota_supported(void)
{
    return 0;
}
