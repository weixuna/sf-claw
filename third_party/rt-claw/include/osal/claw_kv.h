/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * OSAL Key-Value storage abstraction.
 * ESP-IDF: backed by NVS Flash.
 * RT-Thread: RAM-only fallback (persistent backends TBD).
 */

#ifndef CLAW_KV_H
#define CLAW_KV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  claw_kv_init(void);

int  claw_kv_set_str(const char *ns, const char *key, const char *value);
int  claw_kv_get_str(const char *ns, const char *key,
                     char *buf, size_t size);

int  claw_kv_set_blob(const char *ns, const char *key,
                      const void *data, size_t len);
int  claw_kv_get_blob(const char *ns, const char *key,
                      void *data, size_t *len);

int  claw_kv_set_u8(const char *ns, const char *key, uint8_t val);
int  claw_kv_get_u8(const char *ns, const char *key, uint8_t *val);

int  claw_kv_delete(const char *ns, const char *key);
int  claw_kv_erase_ns(const char *ns);

#ifdef __cplusplus
}
#endif

#endif /* CLAW_KV_H */
