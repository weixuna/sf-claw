/**
 * @file wechat_http.h
 * @brief Small HTTPS client with an explicit per-request timeout.
 *
 * RT-Thread's stock webclient sets a fixed 6 s receive timeout when it
 * creates the socket.  iLink uses a 35 s long poll, so we need to set the
 * socket timeout before reading the response header.  This wrapper uses
 * webclient only for DNS/TLS/session setup, then writes the request and
 * parses the response itself.
 */

#ifndef WECHAT_HTTP_H
#define WECHAT_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "osal/claw_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Perform an HTTPS request.
 *
 * @param method       "GET" or "POST"
 * @param url          https:// URL
 * @param headers      optional request headers
 * @param header_count number of headers
 * @param body         POST body (may be NULL)
 * @param body_len     POST body length
 * @param resp         response body buffer, NUL terminated
 * @param resp_size    response buffer size
 * @param resp_len     actual body length (may be NULL)
 * @param timeout_ms   socket receive timeout for this request
 *
 * @return HTTP status (>0) on success, negative on transport/parse error.
 */
int wechat_http_request(const char *method, const char *url,
                        const claw_net_header_t *headers, int header_count,
                        const char *body, size_t body_len,
                        char *resp, size_t resp_size, size_t *resp_len,
                        uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WECHAT_HTTP_H */
