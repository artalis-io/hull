/*
 * hull_async.c — Runtime-agnostic async glue layer
 *
 * Implements HlAsyncCtx lifecycle and the KlAsyncOp callbacks that
 * bridge Keel primitives to Hull runtime continuations via the
 * HlAsyncCont vtable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/async.h"
#include "hull/alloc.h"
#include <string.h>

/* ── KlAsyncOp callbacks ──────────────────────────────────────────── */

/*
 * Called by kl_async_complete() when the async operation finishes.
 * Resumes the runtime handler via the cont vtable, then cleans up.
 */
static void hl_async_on_resume(KlAsyncOp *op, void *user_data)
{
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    (void)op;

    /* Resume the runtime handler — sets conn->state appropriately */
    ctx->cont->resume(ctx->cont, ctx->driver);

    /* Clean up this async operation (always — even on re-yield,
     * because a new HlAsyncCtx was created by the yielding function) */
    if (ctx->free_driver && ctx->driver)
        ctx->free_driver(ctx->driver);
    ctx->cont->destroy(ctx->cont);
    hl_alloc_free(ctx->alloc, ctx, sizeof(HlAsyncCtx));
}

/*
 * Called when the connection is closed while the handler is suspended,
 * or during server shutdown. Cancels the runtime handler without
 * invoking it, then cleans up.
 */
static void hl_async_on_cancel(KlAsyncOp *op, void *user_data)
{
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    (void)op;

    ctx->cont->cancel(ctx->cont);

    if (ctx->free_driver && ctx->driver)
        ctx->free_driver(ctx->driver);
    ctx->cont->destroy(ctx->cont);
    hl_alloc_free(ctx->alloc, ctx, sizeof(HlAsyncCtx));
}

/* ── Public API ───────────────────────────────────────────────────── */

HlAsyncCtx *hl_async_ctx_create(KlServer *s, HlAllocator *alloc)
{
    if (!s) return NULL;

    HlAsyncCtx *ctx = hl_alloc_malloc(alloc, sizeof(HlAsyncCtx));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->server = s;
    ctx->alloc = alloc;

    /* Wire common callbacks — caller sets on_deadline (operation-specific) */
    ctx->op.on_resume = hl_async_on_resume;
    ctx->op.on_cancel = hl_async_on_cancel;
    ctx->op.user_data = ctx;

    return ctx;
}

void hl_async_ctx_free(HlAsyncCtx *ctx)
{
    if (!ctx) return;
    hl_alloc_free(ctx->alloc, ctx, sizeof(HlAsyncCtx));
}

void hl_async_on_deadline_sleep(KlAsyncOp *op, void *user_data)
{
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    /* For sleep, deadline = "timer fired" = success.
     * kl_async_complete calls op->on_resume, which resumes the handler. */
    kl_async_complete(ctx->server, op);
}
