/*
 * cap/ws.h — WebSocket connection registry
 *
 * Tracks WebSocket connections per-endpoint for broadcast and
 * connection counting. Runtime-agnostic — used by both Lua and JS.
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
    KlWsServerConn *kl_ws;      /* Keel connection (NULL after close) */
    uint64_t        id;         /* monotonic connection ID */
    const char     *path;       /* borrowed from endpoint */
    int             closed;     /* 1 after close frame sent/received */
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

/* Find an HlWsConn by its underlying Keel connection pointer. */
HlWsConn *hl_ws_registry_find(HlWsRegistry *reg, const char *path,
                                KlWsServerConn *kl_ws);

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
