/*
 * cap/ws.c — WebSocket connection registry
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/ws.h"
#include "hull/utils/alloc.h"
#include "hull/http_feature.h"  /* hl_http_ws_registry_free (seam strong override) */

#include <keel/websocket_server.h>

#include <string.h>

/* ── Endpoint lookup ───────────────────────────────────────────────── */

static HlWsEndpoint *find_endpoint(HlWsRegistry *reg, const char *path)
{
    for (HlWsEndpoint *ep = reg->endpoints; ep; ep = ep->next) {
        if (strcmp(ep->path, path) == 0)
            return ep;
    }
    return NULL;
}

static HlWsEndpoint *find_or_create_endpoint(HlWsRegistry *reg,
                                               const char *path)
{
    HlWsEndpoint *ep = find_endpoint(reg, path);
    if (ep)
        return ep;

    ep = hl_alloc_malloc(reg->alloc, sizeof(HlWsEndpoint));
    if (!ep)
        return NULL;

    memset(ep, 0, sizeof(*ep));
    size_t len = strlen(path);
    if (len >= sizeof(ep->path))
        len = sizeof(ep->path) - 1;
    memcpy(ep->path, path, len);
    ep->path[len] = '\0';

    ep->next = reg->endpoints;
    reg->endpoints = ep;
    return ep;
}

/* ── Public API ────────────────────────────────────────────────────── */

void hl_ws_registry_init(HlWsRegistry *reg, HlAllocator *alloc)
{
    memset(reg, 0, sizeof(*reg));
    reg->alloc = alloc;
    reg->next_id = 1;
}

void hl_ws_registry_free(HlWsRegistry *reg)
{
    if (!reg)
        return;

    HlWsEndpoint *ep = reg->endpoints;
    while (ep) {
        HlWsEndpoint *next_ep = ep->next;

        /* Free all connections */
        HlWsConn *conn = ep->conns;
        while (conn) {
            HlWsConn *next_conn = conn->next;
            hl_alloc_free(reg->alloc, conn, sizeof(HlWsConn));
            conn = next_conn;
        }

        hl_alloc_free(reg->alloc, ep, sizeof(HlWsEndpoint));
        ep = next_ep;
    }
    reg->endpoints = NULL;
    reg->next_id = 1;
}

HlWsConn *hl_ws_registry_add(HlWsRegistry *reg, const char *path,
                               KlWsServerConn *kl_ws)
{
    if (!reg || !path)
        return NULL;

    HlWsEndpoint *ep = find_or_create_endpoint(reg, path);
    if (!ep)
        return NULL;

    HlWsConn *conn = hl_alloc_malloc(reg->alloc, sizeof(HlWsConn));
    if (!conn)
        return NULL;

    memset(conn, 0, sizeof(*conn));
    conn->kl_ws = kl_ws;
    conn->id = reg->next_id++;
    conn->path = ep->path; /* borrowed from endpoint */

    /* Prepend to doubly-linked list */
    conn->next = ep->conns;
    conn->prev = NULL;
    if (ep->conns)
        ep->conns->prev = conn;
    ep->conns = conn;
    ep->conn_count++;

    return conn;
}

void hl_ws_registry_remove(HlWsRegistry *reg, HlWsConn *conn)
{
    if (!reg || !conn)
        return;

    HlWsEndpoint *ep = find_endpoint(reg, conn->path);
    if (!ep)
        return;

    /* Unlink from doubly-linked list */
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        ep->conns = conn->next;
    if (conn->next)
        conn->next->prev = conn->prev;

    if (ep->conn_count > 0)
        ep->conn_count--;

    hl_alloc_free(reg->alloc, conn, sizeof(HlWsConn));
}

HlWsConn *hl_ws_registry_find(HlWsRegistry *reg, const char *path,
                                KlWsServerConn *kl_ws)
{
    if (!reg || !path)
        return NULL;

    HlWsEndpoint *ep = find_endpoint(reg, path);
    if (!ep)
        return NULL;

    for (HlWsConn *c = ep->conns; c; c = c->next) {
        if (c->kl_ws == kl_ws)
            return c;
    }
    return NULL;
}

int hl_ws_broadcast_text(HlWsRegistry *reg, const char *path,
                          const char *data, size_t len)
{
    if (!reg || !path)
        return 0;

    HlWsEndpoint *ep = find_endpoint(reg, path);
    if (!ep)
        return 0;

    int sent = 0;
    /* Load-bearing invariant: kl_ws_server_send_* only enqueues a frame and
     * never re-enters Hull or closes the connection synchronously. on_close
     * (which calls hl_ws_registry_remove and frees the node) is delivered as
     * a separate Keel event on this same event-loop thread, so the cursor
     * `c` cannot be freed mid-iteration. If send ever becomes reentrant,
     * this loop must snapshot the connection list first. */
    for (HlWsConn *c = ep->conns; c; c = c->next) {
        if (!c->closed && c->kl_ws) {
            if (kl_ws_server_send_text(c->kl_ws, data, len) == 0)
                sent++;
        }
    }
    return sent;
}

int hl_ws_broadcast_binary(HlWsRegistry *reg, const char *path,
                            const void *data, size_t len)
{
    if (!reg || !path)
        return 0;

    HlWsEndpoint *ep = find_endpoint(reg, path);
    if (!ep)
        return 0;

    int sent = 0;
    /* See hl_ws_broadcast_text: safe only because send is non-reentrant and
     * node frees are deferred to a separate on_close event on this thread. */
    for (HlWsConn *c = ep->conns; c; c = c->next) {
        if (!c->closed && c->kl_ws) {
            if (kl_ws_server_send_binary(c->kl_ws, (const char *)data, len) == 0)
                sent++;
        }
    }
    return sent;
}

size_t hl_ws_connections(HlWsRegistry *reg, const char *path)
{
    if (!reg || !path)
        return 0;

    HlWsEndpoint *ep = find_endpoint(reg, path);
    return ep ? ep->conn_count : 0;
}

/* ── HTTP-feature seam: strong override ─────────────────────────────── */
/* Compiled only in an HTTP-server base (cap/ws.c is dropped when
 * HL_ENABLE_HTTP_SERVER=0), so a no-server base keeps the weak no-op. Lets the
 * runtime teardown free the ws registry without a direct hl_ws_* ref. */
void hl_http_ws_registry_free(void *ws_registry)
{
    hl_ws_registry_free((HlWsRegistry *)ws_registry);
}
