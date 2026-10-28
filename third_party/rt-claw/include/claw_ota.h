/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * OTA (Over-The-Air) update — platform abstraction interface.
 *
 * Each platform provides its own implementation of these functions.
 * The OTA service (claw/services/ota/) calls them for the actual
 * flash operations.  This follows the same pattern as claw_board.h.
 */

#ifndef CLAW_OTA_H
#define CLAW_OTA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OTA state machine */
typedef enum {
    CLAW_OTA_IDLE = 0,
    CLAW_OTA_DOWNLOADING,
    CLAW_OTA_VERIFYING,
    CLAW_OTA_DONE,
    CLAW_OTA_ERROR,
} Claw_OtaState;

/* Firmware info returned by version check */
typedef struct {
    char     version[32];
    char     url[256];
    uint32_t size;
    char     sha256[65];   /* hex string, NUL-terminated */
} claw_ota_info_t;

/*
 * Progress callback — called during firmware download.
 * @param received  Bytes received so far
 * @param total     Total firmware size (0 if unknown)
 */
typedef void (*claw_ota_progress_cb)(uint32_t received, uint32_t total);

/*
 * Initialize platform OTA subsystem.
 * Called once at boot by the OTA service.
 */
int claw_ota_platform_init(void);

/*
 * Download firmware from url and flash it to the inactive partition.
 * This is a blocking call — it returns after the entire update completes
 * or fails.  The progress callback (if non-NULL) is called periodically.
 *
 * On success the new partition is set as boot target but the device
 * is NOT rebooted — the caller decides when to reboot.
 *
 * @return CLAW_OK on success, CLAW_ERROR on failure.
 */
int claw_ota_do_update(const char *url, claw_ota_progress_cb progress);

/*
 * Roll back to the previous firmware.
 * Only valid after an OTA update before the new firmware is marked valid.
 */
int claw_ota_rollback(void);

/*
 * Mark the currently running firmware as valid.
 * Call this after the new firmware boots successfully.
 * Prevents automatic rollback on next reboot.
 */
int claw_ota_mark_valid(void);

/* Return the version string of the running firmware. */
const char *claw_ota_running_version(void);

/* Return 1 if platform supports OTA, 0 otherwise. */
int claw_ota_supported(void);

#ifdef __cplusplus
}
#endif

#endif /* CLAW_OTA_H */
