/**
 * @file claw_net_webclient.c
 * @brief rt-claw network backend built on RT-Thread webclient + mbedTLS.
 *
 * The stock osal/rtthread/claw_net_rtthread.c only supports plain HTTP.
 * Real LLM endpoints use HTTPS, so this backend replaces it.  The body
 * is read through webclient_read() rather than webclient_request() so
 * replies larger than WEBCLIENT_RESPONSE_BUFSZ are not silently cut.
 */

#include <rtthread.h>
#include <webclient.h>

#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <stdbool.h>

#include "osal/claw_os.h"
#include "osal/claw_net.h"
#include "claw_port_mem.h"

#define TAG "claw_net"
#define CLAW_HTTP_TIMEOUT_MS 60000
#define CLAW_HTTP_HEADER_SZ  4096
#define CLAW_HTTP_CHUNK_SZ   512

/*
 * Heap watermark logging.  A TLS request allocates tens of KB; keeping
 * before/peak/after numbers in the log makes SRAM leaks and PSRAM
 * fallbacks visible on a device without a debugger.
 */
static void log_heap(const char *stage)
{
    rt_size_t total = 0, used = 0;
    size_t pt = 0, pu = 0, pf = 0, pl = 0;

    rt_memory_info(&total, &used, RT_NULL);
    claw_port_mem_info(&pt, &pu, &pf, &pl);
    CLAW_LOGI(TAG, "heap %-8s sram used=%u free=%u | psram used=%u free=%u",
              stage,
              (unsigned)used, (unsigned)(total - used),
              (unsigned)pu, (unsigned)pf);
}

static int add_request_headers(struct webclient_session *session,
                               const claw_net_header_t *headers,
                               int header_count,
                               int content_length)
{
    int has_length = 0;
    int has_connection = 0;

    for (int i = 0; i < header_count; i++) {
        if (!headers[i].key || !headers[i].value) {
            continue;
        }
        if (strcasecmp(headers[i].key, "Content-Length") == 0) {
            has_length = 1;
        }
        if (strcasecmp(headers[i].key, "Connection") == 0) {
            has_connection = 1;
        }
        if (webclient_header_fields_add(session, "%s: %s\r\n",
                                        headers[i].key,
                                        headers[i].value) < 0) {
            return -1;
        }
    }

    if (content_length >= 0 && !has_length) {
        if (webclient_header_fields_add(session, "Content-Length: %d\r\n",
                                        content_length) < 0) {
            return -1;
        }
    }

    if (!has_connection) {
        if (webclient_header_fields_add(session, "Connection: close\r\n") < 0) {
            return -1;
        }
    }

    return 0;
}

/* 部分服务端返回的 chunked 响应没有被 webclient 解码，这里做兜底。 */
static int body_looks_chunked(const char *buf, size_t len)
{
    size_t i = 0;

    if (!buf || len < 3) {
        return 0;
    }
    while (i < len && i < 16) {
        char c = buf[i];

        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
            i++;
        } else {
            break;
        }
    }
    if (i == 0 || i + 1 >= len) {
        return 0;
    }
    return buf[i] == '\r' && buf[i + 1] == '\n';
}

static int dechunk_body(char *buf, size_t *len)
{
    size_t in = 0;
    size_t out = 0;
    size_t total = *len;

    while (in < total) {
        size_t chunk = 0;
        int digits = 0;

        while (in < total) {
            char c = buf[in];
            int value;

            if (c >= '0' && c <= '9') {
                value = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                value = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                value = c - 'A' + 10;
            } else {
                break;
            }
            chunk = chunk * 16 + (size_t)value;
            digits++;
            in++;
        }
        if (!digits) {
            return -1;
        }

        /* 跳过 chunk 扩展，直到 CRLF。 */
        while (in < total && buf[in] != '\r') {
            in++;
        }
        if (in + 1 >= total || buf[in] != '\r' || buf[in + 1] != '\n') {
            return -1;
        }
        in += 2;

        if (chunk == 0) {
            *len = out;
            buf[out] = '\0';
            return 0;
        }
        if (in + chunk > total) {
            return -1;
        }

        memmove(buf + out, buf + in, chunk);
        out += chunk;
        in += chunk;

        if (in + 1 < total && buf[in] == '\r' && buf[in + 1] == '\n') {
            in += 2;
        } else if (in < total) {
            return -1;
        }
    }

    *len = out;
    buf[out] = '\0';
    return 0;
}

static int read_body(struct webclient_session *session,
                     char *resp, size_t resp_size, size_t *resp_len)
{
    char chunk[CLAW_HTTP_CHUNK_SZ];
    size_t stored = 0;
    size_t total = 0;
    int truncated = 0;

    if (!resp || resp_size == 0) {
        return -1;
    }
    resp[0] = '\0';

    for (;;) {
        int n = webclient_read(session, chunk, sizeof(chunk));

        if (n == 0) {
            break;
        }
        if (n < 0) {
            CLAW_LOGW(TAG, "read failed: %d", n);
            break;
        }

        total += (size_t)n;

        if (stored + (size_t)n < resp_size) {
            memcpy(resp + stored, chunk, (size_t)n);
            stored += (size_t)n;
        } else {
            size_t room = resp_size - 1 - stored;
            if (room > 0) {
                memcpy(resp + stored, chunk, room);
                stored += room;
            }
            truncated = 1;
            /* Continue draining until the server closes / body ends. */
        }
    }

    resp[stored] = '\0';
    if (resp_len) {
        *resp_len = total;
    }
    if (truncated) {
        CLAW_LOGW(TAG, "response truncated: %u bytes, buffer %u",
                  (unsigned)total, (unsigned)resp_size);
    }

    return 0;
}

static int do_request(const char *url, bool post,
                      const claw_net_header_t *headers, int header_count,
                      const char *body, size_t body_len,
                      char *resp, size_t resp_size, size_t *resp_len)
{
    struct webclient_session *session;
    int status;
    int rc;

    if (!url || !resp || resp_size == 0) {
        return CLAW_ERROR;
    }
    resp[0] = '\0';
    if (resp_len) {
        *resp_len = 0;
    }

    log_heap("http-in");

    session = webclient_session_create(CLAW_HTTP_HEADER_SZ);
    if (!session) {
        CLAW_LOGE(TAG, "session create failed");
        return CLAW_ERROR;
    }

    rc = add_request_headers(session, headers, header_count,
                             post ? (int)body_len : -1);
    if (rc < 0) {
        CLAW_LOGE(TAG, "header build failed");
        webclient_close(session);
        log_heap("hdr-fail");
        return CLAW_ERROR;
    }

    /*
     * webclient_get()/webclient_post() create the TLS (or TCP) socket
     * themselves.  Do not call webclient_connect() first: doing so used
     * to open one connection, overwrite it inside webclient_post(), and
     * leak the first mbedTLS context/socket on every request.  Set the
     * longer body timeout after the header has been received; the
     * header read itself tolerates short receive timeouts because the
     * webclient loops on MBEDTLS_ERR_SSL_WANT_READ.
     */
    if (post) {
        status = webclient_post(session, url, body, body_len);
    } else {
        status = webclient_get(session, url);
    }

    CLAW_LOGI(TAG, "%s %s -> %d",
              post ? "POST" : "GET", url, status);

    if (status > 0) {
        int expected = webclient_content_length_get(session);

        webclient_set_timeout(session, CLAW_HTTP_TIMEOUT_MS);
        read_body(session, resp, resp_size, resp_len);
        if (resp_len && *resp_len > 0 &&
            body_looks_chunked(resp, *resp_len)) {
            size_t decoded = *resp_len;

            if (dechunk_body(resp, &decoded) == 0) {
                *resp_len = decoded;
                CLAW_LOGI(TAG, "dechunked body: %u bytes",
                          (unsigned)decoded);
            } else {
                CLAW_LOGW(TAG, "chunked body decode failed");
            }
        }
        CLAW_LOGI(TAG, "body %u/%d bytes",
                  (unsigned)(resp_len ? *resp_len : 0), expected);
    }

    webclient_close(session);
    log_heap("closed");

    if (status <= 0) {
        return CLAW_ERROR;
    }
    return status;
}

int claw_net_post(const char *url,
                  const claw_net_header_t *headers, int header_count,
                  const char *body, size_t body_len,
                  char *resp, size_t resp_size, size_t *resp_len)
{
    return do_request(url, true, headers, header_count,
                      body, body_len, resp, resp_size, resp_len);
}

int claw_net_get(const char *url,
                 const claw_net_header_t *headers, int header_count,
                 char *resp, size_t resp_size, size_t *resp_len)
{
    return do_request(url, false, headers, header_count,
                      NULL, 0, resp, resp_size, resp_len);
}
