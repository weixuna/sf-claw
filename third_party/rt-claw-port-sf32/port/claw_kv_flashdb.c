/**
 * @file claw_kv_flashdb.c
 * @brief rt-claw KV persistence using FlashDB on a board-provided partition.
 *
 * We use FlashDB directly instead of the SDK share_prefs wrapper because
 * share_prefs hardcodes the "prefdb" partition name.  The board adapter
 * supplies claw_board_ops.kv_partition, so other SF32 boards can use a
 * different FAL partition without changing this package.
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "flashdb.h"

#include "osal/claw_os.h"
#include "osal/claw_kv.h"
#include "claw_port.h"

#define TAG "claw_kv"
#define CLAW_KV_DB_NAME "claw_kv"
#define CLAW_KV_KEY_MAX 80

static struct fdb_kvdb s_kvdb;
static bool s_ready;

static int make_key(char *buf, size_t size, const char *ns, const char *key)
{
    int n;

    if (!ns || !key) {
        return -1;
    }

    n = snprintf(buf, size, "%s.%s", ns, key);
    if (n < 0 || (size_t)n >= size) {
        return -1;
    }
    return 0;
}

int claw_kv_init(void)
{
    const struct claw_board_ops *board;
    const char *partition;
    fdb_err_t err;

    if (s_ready) {
        return CLAW_OK;
    }

    board = claw_board_get();
    partition = (board && board->kv_partition) ? board->kv_partition : NULL;
    if (!partition || partition[0] == '\0') {
        CLAW_LOGW(TAG, "no KV partition provided by board");
        return CLAW_ERROR;
    }

    err = fdb_kvdb_init(&s_kvdb, CLAW_KV_DB_NAME, partition,
                        NULL, NULL);
    if (err != FDB_NO_ERR) {
        CLAW_LOGE(TAG, "fdb_kvdb_init(%s) failed: %d", partition, err);
        return CLAW_ERROR;
    }

    s_ready = true;
    CLAW_LOGI(TAG, "FlashDB partition '%s' ready", partition);
    return CLAW_OK;
}

int claw_kv_set_str(const char *ns, const char *key, const char *value)
{
    char k[CLAW_KV_KEY_MAX];
    struct fdb_blob blob;
    size_t len;

    if (!s_ready || !value) {
        return CLAW_ERROR;
    }
    if (make_key(k, sizeof(k), ns, key) < 0) {
        return CLAW_ERROR;
    }

    len = strlen(value) + 1;
    return (fdb_kv_set_blob(&s_kvdb, k,
                            fdb_blob_make(&blob, value, len)) == FDB_NO_ERR)
               ? CLAW_OK : CLAW_ERROR;
}

int claw_kv_get_str(const char *ns, const char *key,
                    char *buf, size_t size)
{
    char k[CLAW_KV_KEY_MAX];
    struct fdb_blob blob;
    size_t got;

    if (!s_ready || !buf || size == 0) {
        return CLAW_ERROR;
    }
    if (make_key(k, sizeof(k), ns, key) < 0) {
        return CLAW_ERROR;
    }

    got = fdb_kv_get_blob(&s_kvdb, k, fdb_blob_make(&blob, buf, size));
    if (got == 0) {
        return CLAW_ERROR;
    }

    buf[size - 1] = '\0';
    return CLAW_OK;
}

int claw_kv_set_blob(const char *ns, const char *key,
                     const void *data, size_t len)
{
    char k[CLAW_KV_KEY_MAX];
    struct fdb_blob blob;

    if (!s_ready || !data || len == 0) {
        return CLAW_ERROR;
    }
    if (make_key(k, sizeof(k), ns, key) < 0) {
        return CLAW_ERROR;
    }

    return (fdb_kv_set_blob(&s_kvdb, k,
                            fdb_blob_make(&blob, data, len)) == FDB_NO_ERR)
               ? CLAW_OK : CLAW_ERROR;
}

int claw_kv_get_blob(const char *ns, const char *key,
                     void *data, size_t *len)
{
    char k[CLAW_KV_KEY_MAX];
    struct fdb_blob blob;
    size_t got;

    if (!s_ready || !data || !len || *len == 0) {
        return CLAW_ERROR;
    }
    if (make_key(k, sizeof(k), ns, key) < 0) {
        return CLAW_ERROR;
    }

    got = fdb_kv_get_blob(&s_kvdb, k, fdb_blob_make(&blob, data, *len));
    if (got == 0) {
        return CLAW_ERROR;
    }
    if (blob.saved.len > *len) {
        return CLAW_ERROR;   /* caller buffer too small */
    }

    *len = blob.saved.len;
    return CLAW_OK;
}

int claw_kv_set_u8(const char *ns, const char *key, uint8_t val)
{
    return claw_kv_set_blob(ns, key, &val, sizeof(val));
}

int claw_kv_get_u8(const char *ns, const char *key, uint8_t *val)
{
    size_t len = sizeof(*val);

    if (!val) {
        return CLAW_ERROR;
    }
    return claw_kv_get_blob(ns, key, val, &len);
}

int claw_kv_delete(const char *ns, const char *key)
{
    char k[CLAW_KV_KEY_MAX];

    if (!s_ready) {
        return CLAW_ERROR;
    }
    if (make_key(k, sizeof(k), ns, key) < 0) {
        return CLAW_ERROR;
    }

    return (fdb_kv_del(&s_kvdb, k) == FDB_NO_ERR)
               ? CLAW_OK : CLAW_ERROR;
}

int claw_kv_erase_ns(const char *ns)
{
    /*
     * Deleting while iterating is not safe with FlashDB's sector scanner.
     * v1 leaves the namespace in place; callers delete individual keys.
     */
    (void)ns;
    return s_ready ? CLAW_OK : CLAW_ERROR;
}
