/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * OSAL KV storage — RT-Thread RAM-only backend.
 * Data is lost on reboot. Persistent backend (filesystem / EasyFlash)
 * can be added later without changing callers.
 */

#include "osal/claw_os.h"
#include "osal/claw_kv.h"

#include <string.h>
#include <stdio.h>

#define TAG "kv"

#define KV_MAX_ENTRIES  48
#define KV_NS_MAX       16
#define KV_KEY_MAX      16
#define KV_VAL_MAX      256

typedef struct {
    char    ns[KV_NS_MAX];
    char    key[KV_KEY_MAX];
    uint8_t data[KV_VAL_MAX];
    size_t  len;
} kv_entry_t;

static kv_entry_t s_kv[KV_MAX_ENTRIES];
static int        s_kv_count;

static kv_entry_t *kv_find(const char *ns, const char *key)
{
    for (int i = 0; i < s_kv_count; i++) {
        if (strcmp(s_kv[i].ns, ns) == 0 &&
            strcmp(s_kv[i].key, key) == 0) {
            return &s_kv[i];
        }
    }
    return NULL;
}

static kv_entry_t *kv_alloc(const char *ns, const char *key)
{
    kv_entry_t *e = kv_find(ns, key);
    if (e) {
        return e;
    }
    if (s_kv_count >= KV_MAX_ENTRIES) {
        return NULL;
    }
    e = &s_kv[s_kv_count++];
    snprintf(e->ns, KV_NS_MAX, "%s", ns);
    snprintf(e->key, KV_KEY_MAX, "%s", key);
    return e;
}

int claw_kv_init(void)
{
    memset(s_kv, 0, sizeof(s_kv));
    s_kv_count = 0;
    CLAW_LOGI(TAG, "RAM-only KV initialized, max=%d", KV_MAX_ENTRIES);
    return CLAW_OK;
}

int claw_kv_set_str(const char *ns, const char *key, const char *value)
{
    kv_entry_t *e = kv_alloc(ns, key);
    if (!e) {
        return CLAW_ERROR;
    }
    size_t len = strlen(value) + 1;
    if (len > KV_VAL_MAX) {
        len = KV_VAL_MAX;
    }
    memcpy(e->data, value, len);
    e->data[KV_VAL_MAX - 1] = '\0';
    e->len = len;
    return CLAW_OK;
}

int claw_kv_get_str(const char *ns, const char *key,
                    char *buf, size_t size)
{
    kv_entry_t *e = kv_find(ns, key);
    if (!e) {
        return CLAW_ERROR;
    }
    snprintf(buf, size, "%s", (const char *)e->data);
    return CLAW_OK;
}

int claw_kv_set_blob(const char *ns, const char *key,
                     const void *data, size_t len)
{
    kv_entry_t *e = kv_alloc(ns, key);
    if (!e || len > KV_VAL_MAX) {
        return CLAW_ERROR;
    }
    memcpy(e->data, data, len);
    e->len = len;
    return CLAW_OK;
}

int claw_kv_get_blob(const char *ns, const char *key,
                     void *data, size_t *len)
{
    kv_entry_t *e = kv_find(ns, key);
    if (!e) {
        return CLAW_ERROR;
    }
    if (*len < e->len) {
        return CLAW_ERROR;
    }
    memcpy(data, e->data, e->len);
    *len = e->len;
    return CLAW_OK;
}

int claw_kv_set_u8(const char *ns, const char *key, uint8_t val)
{
    kv_entry_t *e = kv_alloc(ns, key);
    if (!e) {
        return CLAW_ERROR;
    }
    e->data[0] = val;
    e->len = 1;
    return CLAW_OK;
}

int claw_kv_get_u8(const char *ns, const char *key, uint8_t *val)
{
    kv_entry_t *e = kv_find(ns, key);
    if (!e || e->len < 1) {
        return CLAW_ERROR;
    }
    *val = e->data[0];
    return CLAW_OK;
}

int claw_kv_delete(const char *ns, const char *key)
{
    for (int i = 0; i < s_kv_count; i++) {
        if (strcmp(s_kv[i].ns, ns) == 0 &&
            strcmp(s_kv[i].key, key) == 0) {
            if (i < s_kv_count - 1) {
                memmove(&s_kv[i], &s_kv[i + 1],
                        (s_kv_count - i - 1) * sizeof(kv_entry_t));
            }
            s_kv_count--;
            return CLAW_OK;
        }
    }
    return CLAW_ERROR;
}

int claw_kv_erase_ns(const char *ns)
{
    int dst = 0;
    for (int i = 0; i < s_kv_count; i++) {
        if (strcmp(s_kv[i].ns, ns) != 0) {
            if (dst != i) {
                s_kv[dst] = s_kv[i];
            }
            dst++;
        }
    }
    s_kv_count = dst;
    return CLAW_OK;
}
