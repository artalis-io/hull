/*
 * reqctx.h - Tagged per-request context for middleware->handler data passing
 *
 * Replaces the raw void* + size_t-prefix layout on KlHttpRequest.ctx.
 * Supports three kinds: JSON string (test dispatch), Lua registry ref,
 * and JS value ref - avoiding JSON round-trips between middleware hops.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_REQCTX_H
#define HL_REQCTX_H

#include <stddef.h>
#include <stdint.h>

#define HL_REQCTX_JSON     1   /* JSON string (test dispatch) */
#define HL_REQCTX_LUA_REF  2   /* Lua registry reference */
#define HL_REQCTX_JS_VAL   3   /* JS value (stored as raw bytes) */

typedef struct HlReqCtx {
    int     kind;
    union {
        struct {
            char   *data;      /* owned, NUL-terminated */
            size_t  len;
        } json;                /* kind == HL_REQCTX_JSON */
        int lua_ref;           /* kind == HL_REQCTX_LUA_REF, LUA_REGISTRYINDEX ref */
        char js_val_bytes[16]; /* kind == HL_REQCTX_JS_VAL, JSValue stored as bytes
                                * (16 bytes covers both NaN-boxing uint64_t and
                                *  struct { JSValueUnion; int64_t } layouts) */
    };
    /* Tracking (hl_reqctx_track): the request (KlHttpRequest *) whose ctx
     * this is, and the runtime's list of live ones. NULL owner: untracked. */
    const void      *owner;
    struct HlReqCtx *prev, *next;
} HlReqCtx;

/* ── Tracking a middleware ctx until its request ends ────────────────────
 *
 * A middleware's req.ctx is stored on the request (KlHttpRequest.ctx) for
 * the next middleware and the handler. Keel runs middleware before it
 * checks the route, the upgrade and the body, so many requests that pass
 * middleware never reach a handler - a 404 / 405, a WebSocket upgrade, a
 * 413 / 415, a body that fails to parse, a client gone mid-body - and Keel
 * then zeroes the request without telling the runtime: the ctx (a JS value,
 * a Lua reference) was pinned in the script heap for good, one per such
 * request (audit 7 H2). The runtime tracks every ctx it stores by its
 * request and frees it when the response has been sent
 * (HlRuntimeVtable.request_done, Keel's access-log hook). As a backstop for
 * a request that ends with no response sent (a WebSocket upgrade, a client
 * gone mid-body): Keel zeroes a request before reusing it, so the next time
 * a middleware or handler sees that request with no ctx, any ctx still
 * tracked for it is left over from an earlier request and is freed. At most
 * one ctx per connection slot is ever left over that way. */

typedef struct HlReqCtxList {
    HlReqCtx *head;
} HlReqCtxList;

static inline void hl_reqctx_track(HlReqCtxList *list, HlReqCtx *c,
                                   const void *owner)
{
    c->owner = owner;
    c->prev = NULL;
    c->next = list->head;
    if (list->head) list->head->prev = c;
    list->head = c;
}

/* Idempotent: an untracked ctx (owner NULL) is left alone. */
static inline void hl_reqctx_untrack(HlReqCtxList *list, HlReqCtx *c)
{
    if (!c->owner) return;
    if (c->prev) c->prev->next = c->next;
    else list->head = c->next;
    if (c->next) c->next->prev = c->prev;
    c->owner = NULL;
    c->prev = c->next = NULL;
}

/* A ctx tracked for @p owner, or NULL. A caller that sees @p owner's
 * request with no ctx frees (and untracks) every one this returns. */
static inline HlReqCtx *hl_reqctx_find(const HlReqCtxList *list,
                                       const void *owner)
{
    for (HlReqCtx *c = list->head; c; c = c->next)
        if (c->owner == owner) return c;
    return NULL;
}

#endif /* HL_REQCTX_H */
