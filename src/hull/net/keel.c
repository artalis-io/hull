/*
 * net/keel.c - HlNetBackend implementation backed by Keel.
 *
 * Implements the connection-bound async-op pair
 * (op_suspend / op_complete), which is enough to migrate every
 * existing kl_async_suspend / kl_async_complete caller off of Keel
 * symbols. The remaining vtable slots (server lifecycle, routing,
 * request/response accessors, SSE, WebSocket) stay NULL until each
 * call surface is migrated in follow-up commits.
 *
 * The keel backend treats its opaque vtable types as type-punning
 * facades:
 *   HlNetBackendCtx*   → struct kl_wrap (holds KlHttpServer + ownership flag)
 *   HlReqHandle*       → KlHttpConn*
 *   HlSuspendOp*       → KlAsyncOp*
 *
 * Consumers never see those casts; they just pass the opaque
 * pointers through.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/net_backend.h"
#include "hull/net/keel.h"
#include "keel/async.h"
#include "keel/http_server.h"

#include <stdlib.h>
#include <string.h>

/* ── Opaque wrapper layout ─────────────────────────────────────────── */

/*
 * struct kl_wrap is what HlNetBackendCtx* actually points at. We don't
 * embed KlHttpServer because we don't own it - serve.c already owns the
 * server and lends us a pointer.
 */
struct kl_wrap {
    KlHttpServer *server;     /* borrowed; never freed here */
};

/* ── Vtable methods ────────────────────────────────────────────────── */

static int keel_op_suspend(HlNetBackendCtx *ctx, HlReqHandle *req, HlSuspendOp *op)
{
    if (!ctx || !req || !op) return -1;
    struct kl_wrap *w = (struct kl_wrap *)ctx;
    return kl_async_suspend(w->server, (KlHttpConn *)req, (KlAsyncOp *)op);
}

static void keel_op_complete(HlNetBackendCtx *ctx, HlSuspendOp *op)
{
    if (!ctx || !op) return;
    struct kl_wrap *w = (struct kl_wrap *)ctx;
    kl_async_complete(w->server, (KlAsyncOp *)op);
}

/* ── Vtable + getter ───────────────────────────────────────────────── */

/*
 * The unmigrated slots are intentionally NULL. Adding a stub that
 * aborts would punish anyone who calls hl_net_backend()->server_init
 * after a build setting mismatch; the current contract is "if a slot
 * is NULL the surface isn't ready yet - keep using the direct path
 * for now." When a vtable method is wired in, its NULL goes away
 * here at the same time as its callers migrate.
 */
static const HlNetBackend keel_backend = {
    .name        = "keel",
    .op_suspend  = keel_op_suspend,
    .op_complete = keel_op_complete,
};

const HlNetBackend *hl_net_backend(void)
{
    return &keel_backend;
}

/* ── Convenience wrappers (declared in net_backend.h) ──────────────── */

int hl_net_op_suspend(HlNetBackendCtx *ctx, HlReqHandle *req, HlSuspendOp *op)
{
    return keel_op_suspend(ctx, req, op);
}

void hl_net_op_complete(HlNetBackendCtx *ctx, HlSuspendOp *op)
{
    keel_op_complete(ctx, op);
}

/* ── Wrap / unwrap ─────────────────────────────────────────────────── */

HlNetBackendCtx *hl_net_backend_keel_wrap(KlHttpServer *server)
{
    if (!server) return NULL;

    struct kl_wrap *w = (struct kl_wrap *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->server = server;
    return (HlNetBackendCtx *)w;
}

void hl_net_backend_keel_unwrap(HlNetBackendCtx *ctx)
{
    if (!ctx) return;
    struct kl_wrap *w = (struct kl_wrap *)ctx;
    /* w->server is borrowed - don't touch it. */
    free(w);
}
