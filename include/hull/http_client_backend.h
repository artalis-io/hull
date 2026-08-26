/**
 * @file http_client_backend.h
 * @brief Pluggable HTTP client backend vtable.
 *
 * Outbound HTTP. Separate from HlNetBackend (the server side) because
 * some backends might pair them differently - Keel does both, libuv
 * is async-only with no built-in HTTP, an httprb-style impl might
 * just do client. Splitting keeps the contracts narrow.
 *
 * One backend ships:
 *
 *   keel - wraps KlClient + KlClientPool + KlRedirectClient. Default
 *          for HL_ENABLE_HTTP=1.
 *
 * HL_ENABLE_HTTP=0 builds drop the HTTP client entirely (apps that
 * import hull/http fail at module-resolve time per the
 * HL_MOD_CAP_HTTP requirement, so nothing reaches the client layer).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_HTTP_CLIENT_BACKEND_H
#define HL_HTTP_CLIENT_BACKEND_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlAllocator             HlAllocator;
typedef struct HlAsyncBackend          HlAsyncBackend;
typedef struct HlAsyncBackendCtx       HlAsyncBackendCtx;
typedef struct HlHttpClientBackendCtx  HlHttpClientBackendCtx;
typedef struct HlHttpClientPoolCtx     HlHttpClientPoolCtx;

/* ── Request / response shapes ─────────────────────────────────────── */

typedef struct HlHttpHeader {
    const char *name;
    const char *value;
} HlHttpHeader;

typedef struct HlHttpRequest {
    const char         *method;     /* "GET", "POST", ... */
    const char         *url;        /* full URL */
    const HlHttpHeader *headers;
    int                 header_count;
    const char         *body;
    size_t              body_len;

    int                 timeout_ms;        /* 0 = backend default */
    int                 follow_redirects;  /* max redirects, 0 = none */

    /* Optional: TLS context (KlTlsCtx today). NULL = use default. */
    void               *tls_ctx;
} HlHttpRequest;

typedef struct HlHttpResponse {
    int           status;
    HlHttpHeader *headers;
    int           header_count;
    char         *body;
    size_t        body_len;

    /* Filled if the request errored before getting a response. */
    int           error;
    const char   *error_msg;

    /* Internal allocator handle so the backend can free this. */
    void         *_alloc_state;
} HlHttpResponse;

typedef void (*HlHttpDoneFn)(HlHttpResponse *res, void *user);

/* ── Pool config ───────────────────────────────────────────────────── */

typedef struct HlHttpClientPoolConfig {
    int max_connections;       /* total cap */
    int max_per_host;
    int idle_timeout_ms;
} HlHttpClientPoolConfig;

/* ── The vtable ────────────────────────────────────────────────────── */

typedef struct HlHttpClientBackend {
    const char *name;       /* "keel", ... */

    /* Backend init / teardown. ctx wraps the connection pool +
     * backend-private state. */
    int    (*init)(HlHttpClientBackendCtx **out,
                   HlAsyncBackend *async, HlAsyncBackendCtx *async_ctx,
                   HlAllocator *alloc);
    void   (*free)(HlHttpClientBackendCtx *ctx);

    /* Synchronous request - blocks until response or error.
     * `out` is filled; caller must call response_free(). */
    int    (*request_sync)(HlHttpClientBackendCtx *ctx,
                           const HlHttpRequest *req,
                           HlHttpResponse *out);

    /* Async request - non-blocking. `done(res, user)` fires on the
     * event-loop thread when the request completes or errors. */
    int    (*request_async)(HlHttpClientBackendCtx *ctx,
                            const HlHttpRequest *req,
                            HlHttpDoneFn done, void *user);

    /* Free response body / headers allocated by request_sync/async. */
    void   (*response_free)(HlHttpResponse *res);

    /* ── Optional: connection pool ───────────────────────────────── */

    /* Some backends bake pooling into the main ctx; others expose a
     * separate pool object. If pool_create is NULL the backend
     * doesn't pool - every request opens a fresh connection. */
    int    (*pool_create)(HlHttpClientPoolCtx **out,
                          HlHttpClientBackendCtx *ctx,
                          const HlHttpClientPoolConfig *cfg);
    void   (*pool_free)  (HlHttpClientPoolCtx *pool);

    /* Use the pool for the next request. Returns the same shape as
     * request_sync/async but routes through the pool's connection
     * cache. NULL pool falls back to a one-shot connection. */
    int    (*request_sync_pooled)(HlHttpClientPoolCtx *pool,
                                  const HlHttpRequest *req,
                                  HlHttpResponse *out);
    int    (*request_async_pooled)(HlHttpClientPoolCtx *pool,
                                   const HlHttpRequest *req,
                                   HlHttpDoneFn done, void *user);
} HlHttpClientBackend;

/* ── Backend getter ────────────────────────────────────────────────── */

/*
 * Returns the compiled-in HTTP client backend, or NULL if
 * HL_ENABLE_HTTP=0. Callers must null-check (or guard with #ifdef).
 */
const HlHttpClientBackend *hl_http_client_backend(void);

#endif /* HL_HTTP_CLIENT_BACKEND_H */
