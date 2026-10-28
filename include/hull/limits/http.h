/*
 * hull/limits/http.h - outbound HTTP client timeout default + ceilings
 *
 * The timeout is ONE deadline for the whole request: connect, TLS, send,
 * receive and every redirect hop.
 *
 *   - async (http.fetch / http.async.*): Hull arms it as the op's deadline,
 *     on the attached path (a request handler) and the detached one (timers,
 *     WebSocket callbacks, app.main) alike. It covers the whole redirect
 *     chain: Keel restarts its own per-hop timer on every hop, Hull's does
 *     not.
 *   - sync (http.get / post / put / patch / delete / request): Keel's sync
 *     client bounds one hop by timeout_ms (Keel >= 3.3.0); Hull computes the
 *     absolute deadline up front and, before each redirect hop, hands Keel
 *     only what is left of it, refusing the hop once nothing is left.
 *
 * DNS is NOT interruptible by the deadline. Both paths resolve with a
 * blocking getaddrinfo (the sync client always; the async client because
 * Hull sets system_dns = 1, which runs it on the event-loop thread). The
 * time a lookup takes counts against the deadline, but a stalled resolver
 * blocks until the system resolver's own timeout gives up.
 *
 * Redirects: at most HL_HTTP_MAX_REDIRECTS hops (unless HlHttpConfig sets
 * its own max_redirects), each checked against manifest.hosts.
 *
 * Resolution: a per-call option overrides the manifest's `http.timeout_ms`
 * (JS `http.timeoutMs`), which overrides HL_HTTP_DEFAULT_TIMEOUT_MS; the
 * result is clamped to the path's ceiling. The sync ceiling is lower
 * because a sync call blocks the event loop for its whole duration.
 *
 * Header-only: used by the manifest parsers (base), the http cap and the
 * runtime bindings (the composed http feature), and the unit tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_HTTP_H
#define HL_LIMITS_HTTP_H

#include <stddef.h>

#include "hull/shared/offheap.h"

#define HL_HTTP_DEFAULT_TIMEOUT_MS    30000    /* 30 s */
#define HL_HTTP_FETCH_MAX_TIMEOUT_MS  600000   /* 10 min: async http.fetch */
#define HL_HTTP_SYNC_MAX_TIMEOUT_MS   60000    /* 60 s: sync calls block the loop */
#define HL_HTTP_MAX_REDIRECTS         5        /* hops followed (Keel's default is 10) */
/* ws-client close handshake: after conn:close() the peer's Close must come
 * back within this, or the connection is dropped and on_close runs with 1006
 * (audit 26: Keel sets CLOSING with no timer of its own). */
#define HL_WS_CLIENT_CLOSE_TIMEOUT_MS 5000

/* ── Live ws.connect clients (audit 32) ──────────────────────────────
 *
 * Each Keel client connection keeps an outbound drain of up to 4 MiB and a
 * message reassembly buffer of up to max_frame_size on Keel's allocator,
 * outside the VM heap, for as long as the KlWsClientConn exists (Keel frees
 * them only in kl_ws_client_free, not on close). Nothing counted the
 * clients a VM kept, so a loop of ws.connect to a peer that stopped reading
 * held gigabytes beside a 64 MB heap. A VM now holds at most
 * HL_WS_CLIENT_MAX_LIVE Keel clients (counted from ws.connect until the
 * client is freed), and each client's frame limit is
 * hl_ws_client_max_frame(): HL_WS_CLIENT_MAX_FRAME, lowered to the VM heap
 * limit (a message bigger than the heap could never reach the app anyway).
 * So a VM's client buffers are bounded by 64 x (4 MiB + 4 MiB) = 512 MiB,
 * the same order as the http.fetch cap (64 x max_response_size). */
#define HL_WS_CLIENT_MAX_LIVE   64
#define HL_WS_CLIENT_MAX_FRAME  ((size_t)4 * 1024 * 1024)   /* Keel's default: 16 MiB */

static inline size_t hl_ws_client_max_frame(size_t heap_limit)
{
    return heap_limit > 0 && heap_limit < HL_WS_CLIENT_MAX_FRAME
           ? heap_limit : HL_WS_CLIENT_MAX_FRAME;
}

/* Give a client's slot back: @p *slot is the VM's count while the client
 * holds one, NULL once given back, so every end path may call it. */
static inline void hl_ws_client_slot_release(int **slot)
{
    if (*slot) {
        if (**slot > 0) (**slot)--;
        *slot = NULL;
    }
}

/* The effective whole-request timeout. @p per_call and @p app_default are
 * 0 when not given (anything <= 0 counts as not given); @p ceiling is
 * HL_HTTP_FETCH_MAX_TIMEOUT_MS or HL_HTTP_SYNC_MAX_TIMEOUT_MS. */
static inline int hl_http_timeout_resolve(long long per_call,
                                          long long app_default,
                                          int ceiling)
{
    long long t = per_call > 0    ? per_call
                : app_default > 0 ? app_default
                :                   HL_HTTP_DEFAULT_TIMEOUT_MS;
    if (t > ceiling) t = ceiling;
    return (int)t;
}

/* The redirect hop limit handed to Keel: @p configured when positive, else
 * HL_HTTP_MAX_REDIRECTS (never Keel's own default of 10). */
static inline int hl_http_max_redirects(int configured)
{
    return configured > 0 ? configured : HL_HTTP_MAX_REDIRECTS;
}

/* ── In-flight http.fetch admission (audit 31) ───────────────────────
 *
 * Keel buffers every async response (decompressed body included, bounded by
 * max_response_size, 4 MiB by default) on its own allocator, outside the VM
 * heap, until the op is done, and nothing counted the fetches a VM kept in
 * flight: `Promise.all(2000 x http.fetch(big))` could hold ~8 GiB beside a
 * 64 MB heap limit. A VM now keeps at most HL_HTTP_MAX_INFLIGHT fetches in
 * flight, so the response buffers it can hold are bounded by
 * HL_HTTP_MAX_INFLIGHT x max_response_size. The request body Keel copies is
 * still held on the VM's off-heap total under the heap limit (audit 22).
 * The count and the hold go back when the op's ctx goes (shared/async.c). */
#define HL_HTTP_MAX_INFLIGHT          64

enum {
    HL_HTTP_ADMIT_OK       = 0,
    HL_HTTP_ADMIT_INFLIGHT = -1,   /* HL_HTTP_MAX_INFLIGHT already in flight */
    HL_HTTP_ADMIT_HEAP     = -2,   /* the body copy past the heap limit */
};

/* Admit one more async fetch: @p inflight is the VM's count, @p total its
 * off-heap counter, @p limit its heap limit (0 = none). On success @p hold
 * holds @p body_len; on refusal nothing is held. */
static inline int hl_http_fetch_admit(int inflight, HlOffheapHold *hold,
                                      size_t *total, size_t limit,
                                      size_t body_len)
{
    if (inflight >= HL_HTTP_MAX_INFLIGHT)
        return HL_HTTP_ADMIT_INFLIGHT;
    if (hl_offheap_take(hold, total, limit, body_len) != 0)
        return HL_HTTP_ADMIT_HEAP;
    return HL_HTTP_ADMIT_OK;
}

#endif /* HL_LIMITS_HTTP_H */
