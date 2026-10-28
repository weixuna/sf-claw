/**
 * @file claw_port_cfg.c
 * @brief Load/save rt-claw AI settings in the prefdb KV store.
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>

#include "osal/claw_os.h"
#include "osal/claw_kv.h"
#include "claw/services/ai/ai_engine.h"

#include "claw_port_cfg.h"

#define TAG "claw_cfg"
#define CLAW_CFG_NS  "claw"
#define CLAW_CFG_BUF 256

void claw_port_ai_config_load(void)
{
    char buf[CLAW_CFG_BUF];

    if (claw_kv_get_str(CLAW_CFG_NS, "api_url", buf, sizeof(buf))
        == CLAW_OK) {
        ai_set_api_url(buf);
        CLAW_LOGI(TAG, "api_url loaded");
    }
    if (claw_kv_get_str(CLAW_CFG_NS, "api_key", buf, sizeof(buf))
        == CLAW_OK) {
        ai_set_api_key(buf);
        CLAW_LOGI(TAG, "api_key loaded (%u bytes)",
                  (unsigned)strlen(buf));
    }
    if (claw_kv_get_str(CLAW_CFG_NS, "model", buf, sizeof(buf))
        == CLAW_OK) {
        ai_set_model(buf);
        CLAW_LOGI(TAG, "model loaded: %s", buf);
    }
}

int claw_port_ai_config_save(void)
{
    int rc = CLAW_OK;

    if (claw_kv_set_str(CLAW_CFG_NS, "api_url",
                        ai_get_api_url()) != CLAW_OK) {
        rc = CLAW_ERROR;
    }
    if (claw_kv_set_str(CLAW_CFG_NS, "api_key",
                        ai_get_api_key()) != CLAW_OK) {
        rc = CLAW_ERROR;
    }
    if (claw_kv_set_str(CLAW_CFG_NS, "model",
                        ai_get_model()) != CLAW_OK) {
        rc = CLAW_ERROR;
    }

    if (rc == CLAW_OK) {
        CLAW_LOGI(TAG, "settings saved");
    } else {
        CLAW_LOGE(TAG, "settings save failed");
    }
    return rc;
}

int claw_port_ai_config_set(const char *url, const char *key,
                            const char *model)
{
    if (url) {
        ai_set_api_url(url);
    }
    if (key) {
        ai_set_api_key(key);
    }
    if (model) {
        ai_set_model(model);
    }
    return claw_port_ai_config_save();
}

void claw_port_ai_key_masked(char *buf, size_t size)
{
    const char *key = ai_get_api_key();
    size_t len;

    if (!buf || size == 0) {
        return;
    }

    len = strlen(key);
    if (len == 0) {
        snprintf(buf, size, "(not set)");
    } else if (len <= 8) {
        snprintf(buf, size, "********");
    } else {
        snprintf(buf, size, "%.4s...%.4s (%u bytes)",
                 key, key + len - 4, (unsigned)len);
    }
}
