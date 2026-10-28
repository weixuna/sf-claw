/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * OSAL KV storage — Linux file-based backend.
 * Data stored at ~/.config/rt-claw/kv/<namespace>/<key>.
 */

#include "osal/claw_os.h"
#include "osal/claw_kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define TAG "kv"

static char s_base_path[256];

static void ensure_base_path(void)
{
    if (s_base_path[0] != '\0') {
        return;
    }
    const char *home = getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    snprintf(s_base_path, sizeof(s_base_path),
             "%s/.config/rt-claw/kv", home);
}

static int mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int build_path(char *out, size_t out_size,
                      const char *ns, const char *key)
{
    ensure_base_path();
    int n = snprintf(out, out_size, "%s/%s/%s", s_base_path, ns, key);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

static int ensure_ns_dir(const char *ns)
{
    char dir[512];
    ensure_base_path();
    snprintf(dir, sizeof(dir), "%s/%s", s_base_path, ns);
    return mkdirs(dir);
}

int claw_kv_init(void)
{
    ensure_base_path();
    mkdirs(s_base_path);
    return CLAW_OK;
}

int claw_kv_set_str(const char *ns, const char *key, const char *value)
{
    char path[512];
    if (build_path(path, sizeof(path), ns, key) < 0) {
        return CLAW_ERROR;
    }
    ensure_ns_dir(ns);

    /* Atomic write: temp file + rename */
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        return CLAW_ERROR;
    }
    fputs(value, f);
    fclose(f);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return CLAW_ERROR;
    }
    return CLAW_OK;
}

int claw_kv_get_str(const char *ns, const char *key,
                    char *buf, size_t size)
{
    char path[512];
    if (build_path(path, sizeof(path), ns, key) < 0) {
        return CLAW_ERROR;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return CLAW_ERROR;
    }

    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return CLAW_OK;
}

int claw_kv_set_blob(const char *ns, const char *key,
                     const void *data, size_t len)
{
    char path[512];
    if (build_path(path, sizeof(path), ns, key) < 0) {
        return CLAW_ERROR;
    }
    ensure_ns_dir(ns);

    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return CLAW_ERROR;
    }
    fwrite(data, 1, len, f);
    fclose(f);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return CLAW_ERROR;
    }
    return CLAW_OK;
}

int claw_kv_get_blob(const char *ns, const char *key,
                     void *data, size_t *len)
{
    char path[512];
    if (build_path(path, sizeof(path), ns, key) < 0) {
        return CLAW_ERROR;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return CLAW_ERROR;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t to_read = (size_t)fsize;
    if (*len < to_read) {
        to_read = *len;
    }

    *len = fread(data, 1, to_read, f);
    fclose(f);
    return CLAW_OK;
}

int claw_kv_set_u8(const char *ns, const char *key, uint8_t val)
{
    return claw_kv_set_blob(ns, key, &val, sizeof(val));
}

int claw_kv_get_u8(const char *ns, const char *key, uint8_t *val)
{
    size_t len = sizeof(*val);
    return claw_kv_get_blob(ns, key, val, &len);
}

int claw_kv_delete(const char *ns, const char *key)
{
    char path[512];
    if (build_path(path, sizeof(path), ns, key) < 0) {
        return CLAW_ERROR;
    }
    return (unlink(path) == 0) ? CLAW_OK : CLAW_ERROR;
}

static int rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmdir_recursive(full);
        } else {
            unlink(full);
        }
    }
    closedir(d);
    return rmdir(path);
}

int claw_kv_erase_ns(const char *ns)
{
    char dir[512];
    ensure_base_path();
    snprintf(dir, sizeof(dir), "%s/%s", s_base_path, ns);
    return (rmdir_recursive(dir) == 0) ? CLAW_OK : CLAW_ERROR;
}
