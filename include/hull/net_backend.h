/**
 * @file net_backend.h
 * @brief Pluggable HTTP/WebSocket server backend vtable.
 *
 * The higher-level half of Hull's net interface. Owns the HTTP server
 * lifecycle (bind/listen/accept), the router, request/response model,
 * middleware chain, WebSocket and SSE endpoints, body reading. Built
 * on top of HlAsyncBackend (see hull/async_backend.h) — the net
 * backend doesn't own its own event loop, it borrows the async
 * backend's.
 *
 * One backend ships:
 *
 *   keel — wraps Keel's KlServer + KlRouter + KlRequest/KlResponse +
 *          KlBodyReader + KlWs + KlSse. Default when HL_ENABLE_HTTP=1.
 *
 * HL_ENABLE_HTTP=0 builds have no net backend at all — `hl_net_backend()`
 * returns NULL. Hull's CLI driver (serve_cli.c) doesn't consume the
 * net interface.
 *
 * The vtable defines opaque HlReqHandle / HlResHandle types and
 * accessor methods so consumers (runtime/{lua,js}/dispatch.c +
 * bindings.c) never see Keel's KlRequest/KlResponse directly. This
 * doubles the migration footprint vs. wrapping the existing types
 * but cleans up a long-standing leakage and pays back for any future
 * backend.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_NET_BACKEND_H
#define HL_NET_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations — backend-opaque types. */
typedef struct HlAllocator       HlAllocator;
typedef struct HlAsyncBackend    HlAsyncBackend;
typedef struct HlAsyncBackendCtx HlAsyncBackendCtx;
typedef struct HlNetBackendCtx   HlNetBackendCtx;   /* server instance */
typedef struct HlReqHandle       HlReqHandle;       /* one in-flight request */
typedef struct HlResHandle       HlResHandle;       /* its response builder */

/* ── Handler callback ──────────────────────────────────────────────── */

/*
 * Request handler. Called by the net backend after route matching.
 * Returns 0 on success, non-zero to indicate the response is
 * pending (async — the handler will complete it later via the
 * async backend's op_complete).
 */
typedef int (*HlRouteHandler)(HlReqHandle *req, HlResHandle *res,
                              void *user);

/*
 * Middleware. Returns 0 to continue, non-zero to short-circuit
 * (response already written / dispatch should not reach the handler).
 */
typedef int (*HlMiddleware)(HlReqHandle *req, HlResHandle *res,
                            void *user);

/* ── HlNetServerConfig ─────────────────────────────────────────────── */

typedef struct HlNetServerConfig {
    HlAsyncBackend    *async;       /* required — event loop */
    HlAsyncBackendCtx *async_ctx;
    HlAllocator       *alloc;        /* may be NULL → backend default */

    const char        *bind_addr;    /* "127.0.0.1" or "0.0.0.0" */
    int                port;
    int                max_connections;
    long               body_max_size;
    int                read_timeout_ms;

    /* Optional: TLS handle (opaque — exact type depends on the active
     * TLS backend; today it's KlTlsCtx). May be NULL for plaintext. */
    void              *tls_ctx;

    /* Optional: compression handle (opaque — KlCompressCtx today). */
    void              *compress_ctx;
} HlNetServerConfig;

/* ── HlNetServerStats ──────────────────────────────────────────────── */

typedef struct HlNetServerStats {
    int active_connections;
    int max_connections;
    int async_suspended;
    int listen_paused;
} HlNetServerStats;

/* ── WebSocket endpoint config ─────────────────────────────────────── */

typedef struct HlWsHandle HlWsHandle;       /* one ws connection */

typedef struct HlWsConfig {
    int   max_frame_size;
    int   ping_interval_ms;

    /* Per-connection callbacks. user_data is the value passed to
     * ws_endpoint() at registration. */
    void (*on_open)   (HlWsHandle *conn, void *user_data);
    void (*on_message)(HlWsHandle *conn, const char *data, size_t len,
                       int is_binary, void *user_data);
    void (*on_close)  (HlWsHandle *conn, int code, const char *reason,
                       void *user_data);
    void *user_data;
} HlWsConfig;

/* ── The vtable ────────────────────────────────────────────────────── */

typedef struct HlNetBackend {
    const char *name;       /* "keel", ... */

    /* ── Server lifecycle ─────────────────────────────────────────── */

    int    (*server_init)(HlNetBackendCtx **out, const HlNetServerConfig *cfg);
    void   (*server_free)(HlNetBackendCtx *ctx);

    /* Block on the event loop until server_stop() is called. */
    int    (*server_run)(HlNetBackendCtx *ctx);
    void   (*server_stop)(HlNetBackendCtx *ctx);

    void   (*server_stats)(HlNetBackendCtx *ctx, HlNetServerStats *out);

    /* ── Routing ──────────────────────────────────────────────────── */

    /* Register a route. method may be "*" for any. pattern uses
     * ":param" for path captures. */
    int    (*route_add)(HlNetBackendCtx *ctx,
                        const char *method, const char *pattern,
                        HlRouteHandler handler, void *user);

    /* Register middleware. _pre fires before body is read; _post
     * after body is fully consumed. method "*" matches any. pattern
     * /\* matches everything. */
    int    (*middleware_pre)(HlNetBackendCtx *ctx,
                             const char *method, const char *pattern,
                             HlMiddleware mw, void *user);
    int    (*middleware_post)(HlNetBackendCtx *ctx,
                              const char *method, const char *pattern,
                              HlMiddleware mw, void *user);

    /* Register a WebSocket endpoint. cfg must outlive the server. */
    int    (*ws_endpoint)(HlNetBackendCtx *ctx,
                          const char *pattern, HlWsConfig *cfg);

    /* ── Request accessors (called from handlers) ─────────────────── */

    const char *(*req_method)(HlReqHandle *req);
    const char *(*req_path)(HlReqHandle *req);
    const char *(*req_query)(HlReqHandle *req);

    /* Header lookup is case-insensitive. Returns NULL if absent. */
    const char *(*req_header)(HlReqHandle *req, const char *name);

    /* Body bytes. After the body reader has finished, returns
     * (data_out, length). Before that, length is 0. */
    size_t      (*req_body)(HlReqHandle *req, const char **data_out);

    /* Path parameter (registered as :name in route pattern). */
    const char *(*req_param)(HlReqHandle *req, const char *name);

    /* ── Response builders ────────────────────────────────────────── */

    void   (*res_status)(HlResHandle *res, int status);
    void   (*res_header)(HlResHandle *res, const char *name, const char *value);

    /* Body modes — call exactly one of body_copy, body_borrow,
     * file, or stream_*. body_borrow does not copy; data must outlive
     * the response. */
    void   (*res_body_copy)  (HlResHandle *res, const char *data, size_t len);
    void   (*res_body_borrow)(HlResHandle *res, const char *data, size_t len);
    int    (*res_file)       (HlResHandle *res, const char *path);

    /* Chunked streaming. begin → repeated write → end. */
    int    (*res_stream_begin)(HlResHandle *res);
    int    (*res_stream_write)(HlResHandle *res, const char *data, size_t len);
    void   (*res_stream_end)  (HlResHandle *res);

    /* Compression hook (optional; backend may have already compressed
     * the body based on Accept-Encoding). Returns 0 on success. */
    int    (*res_compress)(HlResHandle *res, int level);

    /* ── SSE helpers ──────────────────────────────────────────────── */

    int    (*sse_begin)(HlResHandle *res);
    int    (*sse_event)(HlResHandle *res, const char *name,
                        const char *data, const char *id);
    int    (*sse_comment)(HlResHandle *res, const char *text);
    void   (*sse_end)  (HlResHandle *res);

    /* ── WebSocket send (from handlers, not from on_message) ──────── */

    int    (*ws_send_text)  (HlWsHandle *conn, const char *data, size_t len);
    int    (*ws_send_binary)(HlWsHandle *conn, const char *data, size_t len);
    int    (*ws_send_ping)  (HlWsHandle *conn, const char *data, size_t len);
    int    (*ws_close)      (HlWsHandle *conn, int code, const char *reason);
} HlNetBackend;

/* ── Backend getter ────────────────────────────────────────────────── */

/*
 * Returns the compiled-in net backend, or NULL if HL_ENABLE_HTTP=0.
 * Callers must always null-check (or guard with #ifdef HL_ENABLE_HTTP).
 */
const HlNetBackend *hl_net_backend(void);

#endif /* HL_NET_BACKEND_H */
