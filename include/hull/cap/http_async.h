/*
 * cap/http_async.h - Non-blocking HTTP client via Keel's async client
 *
 * Thin wrapper around kl_http_redirect_start() / kl_http_client_start() that adds
 * host allowlist checking, audit logging, and HlAsyncCtx integration.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_ASYNC_H
#define HL_CAP_HTTP_ASYNC_H

#include "hull/cap/http.h"
#include "hull/shared/async.h"

#include <keel/http_client.h>

/* Forward declarations */
typedef struct HlAllocator      HlAllocator;
typedef struct HlNetBackendCtx  HlNetBackendCtx;
typedef struct KlHttpClientResponse KlHttpClientResponse;

/*
 * Start an async HTTP request. Checks allowlist, creates async client
 * (redirect-following or plain), creates an HlAsyncCtx, and suspends
 * the inbound connection.
 *
 * On success, returns the HlAsyncCtx (with driver set).
 * The caller must set ctx->cont before returning to the event loop.
 *
 * On failure, returns NULL (conn NOT suspended).
 */
HlAsyncCtx *hl_async_http_start(KlHttpServer *server, KlHttpConn *conn,
                                  HlNetBackendCtx *net_ctx,
                                  HlAllocator *alloc,
                                  HlHttpConfig *http_cfg,
                                  const char *method, const char *url,
                                  const HlHttpHeader *headers, int num_headers,
                                  const char *body, size_t body_len);

/*
 * Extract response from an async HTTP driver handle.
 * Handles both redirect (tagged LSB=1) and plain client drivers.
 * Returns NULL if driver is NULL.
 */
const KlHttpClientResponse *hl_http_async_response(void *driver);

#endif /* HL_CAP_HTTP_ASYNC_H */
