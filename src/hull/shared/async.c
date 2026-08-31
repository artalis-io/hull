/*
 * hull_async.c - Runtime-agnostic async glue layer
 *
 * Implements HlAsyncCtx lifecycle and the KlAsyncOp callbacks that
 * bridge Keel primitives to Hull runtime continuations via the
 * HlAsyncCont vtable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/async.h"
#include "hull/utils/alloc.h"
#include "hull/net_backend.h"
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

    /* Resume the runtime handler - it finalizes the response; Keel 3.x drives
     * the send itself after this returns (see HlAsyncCont.resume). */
    ctx->cont->resume(ctx->cont, ctx->driver);

    /* Clean up this async operation (always - even on re-yield,
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

HlAsyncCtx *hl_async_ctx_create(KlHttpServer *s, HlNetBackendCtx *net_ctx,
                                HlAllocator *alloc)
{
    /* Both may be NULL - detached callers don't need either field;
     * the actual loop is reached via the async backend ctx that the
     * caller's runtime carries separately. */

    HlAsyncCtx *ctx = hl_alloc_malloc(alloc, sizeof(HlAsyncCtx));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->server = s;
    ctx->net_ctx = net_ctx;
    ctx->alloc = alloc;

    /* Wire common callbacks - caller sets on_deadline (operation-specific) */
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
    /* For sleep, deadline = "timer fired" = success. The attached
     * path routes through the net backend to revive the suspended
     * connection. CLI builds (HTTP=0) never reach here - connection-
     * less sleeps go through the detached-mode timer path in
     * runtime/{lua,js}/async.c. */
#ifdef HL_ENABLE_HTTP_SERVER
    hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)op);
#else
    (void)ctx;
    (void)op;
#endif
}

void hl_async_ctx_resume_detached(HlAsyncCtx *ctx)
{
    if (!ctx || !ctx->cont) return;

    ctx->cont->resume(ctx->cont, ctx->driver);

    if (ctx->free_driver && ctx->driver)
        ctx->free_driver(ctx->driver);
    ctx->cont->destroy(ctx->cont);
    hl_alloc_free(ctx->alloc, ctx, sizeof(HlAsyncCtx));
}

void hl_detached_timer_fire(void *user_data)
{
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    hl_async_ctx_resume_detached(ctx);
}
