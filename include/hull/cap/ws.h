/**
 * @file cap/ws.h
 * @brief WebSocket connection registry.
 *
 * Tracks per-endpoint WebSocket connections to support broadcast and
 * `ws.connections(path)` counting. Runtime-agnostic - used by both Lua
 * (`hull.ws`) and JS (`hull:ws`).
 *
 * Server endpoints are registered via `app.ws(path, callbacks)`; client
 * connections (`ws.connect(url, callbacks)`) are tracked separately and
 * gated by the manifest's `hosts` allowlist.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_WS_H
#define HL_CAP_WS_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct KlWsServerConn KlWsServerConn;
typedef struct HlAllocator HlAllocator;

/* ── Per-connection handle ─────────────────────────────────────────── */

typedef struct HlWsConn {
    KlWsServerConn *kl_ws;      /* Keel connection (NULL once on_close ran) */
    uint64_t        id;         /* monotonic connection ID */
    const char     *path;       /* borrowed from endpoint */
    int             closed;     /* 1 once no frame may be sent: the app's
                                 * close() asked Keel to close (Keel still
                                 * holds the connection and delivers its
                                 * on_close later), or on_close ran */
    void           *user_data;  /* runtime-specific (Lua ref, JS value) */
    struct HlWsConn *next;
    struct HlWsConn *prev;
} HlWsConn;

/* ── Per-endpoint state ────────────────────────────────────────────── */

typedef struct HlWsEndpoint {
    char             path[256];
    HlWsConn        *conns;       /* doubly-linked list head */
    size_t           conn_count;
    struct HlWsEndpoint *next;    /* linked list of endpoints */
} HlWsEndpoint;

/* ── Registry ──────────────────────────────────────────────────────── */

typedef struct HlWsRegistry {
    HlWsEndpoint   *endpoints;   /* linked list of endpoints */
    HlAllocator    *alloc;
    uint64_t        next_id;     /* monotonic connection ID counter */
} HlWsRegistry;

/* Initialize a registry (zero + set alloc). */
void hl_ws_registry_init(HlWsRegistry *reg, HlAllocator *alloc);

/* Free all endpoints and connections. */
void hl_ws_registry_free(HlWsRegistry *reg);

/* Add a connection to the registry. Returns the new HlWsConn, or NULL on error. */
HlWsConn *hl_ws_registry_add(HlWsRegistry *reg, const char *path,
                               KlWsServerConn *kl_ws);

/* Remove a connection from the registry and free it. */
void hl_ws_registry_remove(HlWsRegistry *reg, HlWsConn *conn);

/* Find the HlWsConn Keel still holds for a Keel connection pointer.
 *
 * Invariant: a node whose on_close ran is never returned. Keel frees the
 * KlWsServerConn when on_close returns, while a node whose async on_close
 * handler is parked stays in the list until its completion hook removes it;
 * a new connection may then get the same address. So on_close marks the
 * node with hl_ws_registry_mark_closed (closed = 1, kl_ws = NULL) before any
 * app code runs, and find skips nodes whose kl_ws is NULL: a later callback
 * for the reused address resolves to the new node (or to none), never to the
 * stale one, whatever order the list holds them in.
 *
 * A node the APP closed (conn:close(): closed = 1, kl_ws kept) IS returned:
 * Keel still holds that connection and delivers its on_close later, which
 * must find the node to run the app's on_close and remove it (audit 44: a
 * find that skipped `closed` leaked the node and never ran on_close). Every
 * send path checks `closed` / `kl_ws`. */
HlWsConn *hl_ws_registry_find(HlWsRegistry *reg, const char *path,
                                KlWsServerConn *kl_ws);

/* Mark @p conn closed as its on_close starts: closed = 1 and kl_ws = NULL
 * (Keel frees it when on_close returns). The node stays allocated, and in
 * the list, until hl_ws_registry_remove; find no longer returns it. */
void hl_ws_registry_mark_closed(HlWsConn *conn);

/* Broadcast a text message to all connections on a path.
 * Returns the number of connections sent to. */
int hl_ws_broadcast_text(HlWsRegistry *reg, const char *path,
                          const char *data, size_t len);

/* Broadcast a binary message to all connections on a path.
 * Returns the number of connections sent to. */
int hl_ws_broadcast_binary(HlWsRegistry *reg, const char *path,
                            const void *data, size_t len);

/* Return the number of active connections on a path. */
size_t hl_ws_connections(HlWsRegistry *reg, const char *path);

#endif /* HL_CAP_WS_H */
