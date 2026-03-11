/*
 * cap/http_async.h — Non-blocking HTTP client via Keel's async client
 *
 * Thin wrapper around kl_client_start() that adds host allowlist
 * checking, audit logging, and HlAsyncCtx integration.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_ASYNC_H
#define HL_CAP_HTTP_ASYNC_H

#include "hull/cap/http.h"
#include "hull/async.h"

/* Forward declarations */
typedef struct HlAllocator HlAllocator;

/*
 * Start an async HTTP request. Checks allowlist, creates KlClient via
 * kl_client_start(), creates an HlAsyncCtx, and suspends the inbound
 * connection.
 *
 * On success, returns the HlAsyncCtx (with driver set to KlClient*).
 * The caller must set ctx->cont before returning to the event loop.
 *
 * On failure, returns NULL (conn NOT suspended).
 */
HlAsyncCtx *hl_async_http_start(KlServer *server, KlConn *conn,
                                  HlAllocator *alloc,
                                  HlHttpConfig *http_cfg,
                                  const char *method, const char *url,
                                  const HlHttpHeader *headers, int num_headers,
                                  const char *body, size_t body_len);

#endif /* HL_CAP_HTTP_ASYNC_H */
