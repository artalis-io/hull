/*
 * cap/http_async.h — Non-blocking HTTP client via KlWatcher
 *
 * Drives outbound HTTP requests through a state machine on the event
 * loop. Uses KlWatcher for socket readiness and KlAsyncOp to suspend
 * the inbound connection while the outbound request is in flight.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_ASYNC_H
#define HL_CAP_HTTP_ASYNC_H

#include "hull/cap/http.h"
#include "hull/cap/http_parser.h"
#include "hull/async.h"
#include "hull/limits.h"

#include <keel/tls.h>

/* Forward declarations */
typedef struct HlAllocator HlAllocator;

/* ── State machine phases ────────────────────────────────────────── */

typedef enum {
    HL_HCLIENT_CONNECTING,
    HL_HCLIENT_TLS_HANDSHAKE,
    HL_HCLIENT_SENDING,
    HL_HCLIENT_RECEIVING,
} HlHttpClientState;

/* ── HlHttpClient — per-request async HTTP state ─────────────────── */

typedef struct HlHttpClient {
    /* Socket + state */
    int                fd;
    HlHttpClientState  state;
    KlServer          *server;
    HlAsyncCtx        *async_ctx;    /* back-pointer for completion */

    /* Config (borrowed, not owned) */
    HlHttpConfig      *http_cfg;

    /* Request (heap-copied, owned) */
    char              *request_buf;   /* formatted HTTP/1.1 request */
    size_t             request_len;
    size_t             request_sent;

    /* Response */
    HlHttpResponse     resp;
    HlHttpParser      *parser;

    /* TLS (NULL if plain HTTP) */
    KlTls             *tls;
    KlTlsConfig       *tls_cfg;      /* borrowed */
    char               host_buf[HL_HTTP_HOSTNAME_MAX]; /* NUL-terminated for SNI */

    HlAllocator       *alloc;
    KlAllocator        kl_alloc;     /* stored (not stack), parser points here */
} HlHttpClient;

/*
 * Start an async HTTP request. Creates a non-blocking socket, begins
 * connect, registers a KlWatcher, creates an HlAsyncCtx, and suspends
 * the inbound connection.
 *
 * On success, returns the HlHttpClient (set as ctx->driver).
 * On failure, returns NULL (error already logged, conn NOT suspended).
 *
 * The caller must set ctx->cont before this returns (via the binding
 * layer, which calls hl_async_http_start then wires the continuation).
 *
 * Actually: this function creates the HlAsyncCtx internally and sets
 * ctx->driver. The caller must then wire ctx->cont and call
 * kl_async_suspend after this returns.
 */
HlHttpClient *hl_async_http_start(KlServer *server, KlConn *conn,
                                   HlAllocator *alloc,
                                   HlHttpConfig *http_cfg,
                                   const char *method, const char *url,
                                   const HlHttpHeader *headers, int num_headers,
                                   const char *body, size_t body_len);

/*
 * Free an HlHttpClient. Closes the socket, destroys TLS, frees the
 * request buffer and parser. Does NOT free the response (caller owns
 * it after successful completion).
 */
void hl_async_http_free_client(HlHttpClient *client, int free_resp);

#endif /* HL_CAP_HTTP_ASYNC_H */
