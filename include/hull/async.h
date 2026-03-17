/*
 * async.h — Runtime-agnostic async continuation layer
 *
 * Bridges Keel async primitives (KlAsyncOp, KlWatcher) with Hull
 * runtime continuations (Lua coroutines, JS promises).
 *
 * HlAsyncCont is a vtable implemented per runtime. HlAsyncCtx is the
 * runtime-agnostic glue that owns a KlAsyncOp, a driver, and a cont.
 * The ctx dispatches through the vtable — it never imports Lua or JS
 * headers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_ASYNC_H
#define HL_ASYNC_H

#include <keel/async.h>

/* Forward declarations */
typedef struct HlAllocator HlAllocator;

/* ── HlAsyncCont — runtime continuation vtable ────────────────────── */

typedef struct HlAsyncCont {
    /*
     * Resume the handler after the async operation completes.
     * For Lua: lua_resume(co, ...). For JS: resolve promise + run jobs.
     * Must set conn->state to KL_CONN_SENDING (completed),
     * KL_CONN_SUSPENDED (re-yield), or KL_CONN_CLOSED (error/stream).
     *
     * `driver` is the Keel driver result (NULL for sleep, KlHttpClient*
     * for HTTP, etc.).
     */
    void (*resume)(struct HlAsyncCont *self, void *driver);

    /*
     * Cancel the handler without invoking — free runtime refs.
     * Called when the connection is closed while suspended, or on
     * server shutdown.
     */
    void (*cancel)(struct HlAsyncCont *self);

    /*
     * Free the cont struct itself. Called after resume or cancel.
     * Does NOT free the coroutine/promise ref — that's managed by
     * the runtime struct (HlLua/HlJS).
     */
    void (*destroy)(struct HlAsyncCont *self);
} HlAsyncCont;

/* ── HlAsyncCtx — runtime-agnostic async glue ─────────────────────── */

typedef struct HlAsyncCtx {
    KlAsyncOp    op;            /* embedded — container_of to get ctx */
    KlServer    *server;

    /* Keel driver — opaque to the ctx */
    void        *driver;        /* KlHttpClient*, NULL for sleep, etc. */
    void       (*free_driver)(void *driver);

    /* Runtime continuation — vtable dispatch, no runtime knowledge */
    HlAsyncCont *cont;          /* Lua, JS, or any future runtime */

    HlAllocator *alloc;
    int          detached;      /* 1 = no connection (timer callback) */
} HlAsyncCtx;

/*
 * Create an async context. Wires up the KlAsyncOp callbacks
 * (on_resume, on_cancel). Caller must set:
 *   ctx->cont, ctx->driver, ctx->free_driver, ctx->op.deadline_ms,
 *   and ctx->op.on_deadline (operation-specific).
 * Then call kl_async_suspend(server, conn, &ctx->op).
 */
HlAsyncCtx *hl_async_ctx_create(KlServer *s, HlAllocator *alloc);

/*
 * Free an async context without calling callbacks.
 * Use only on creation failure before kl_async_suspend.
 */
void hl_async_ctx_free(HlAsyncCtx *ctx);

/*
 * Sleep-specific on_deadline callback: deadline = "timer fired" = success.
 * Calls kl_async_complete() to resume the handler.
 */
void hl_async_on_deadline_sleep(KlAsyncOp *op, void *user_data);

/*
 * Resume a detached async context (no connection).
 * Calls cont->resume, cleans up driver and cont, frees ctx.
 * Used by timer callbacks and other connectionless async operations.
 */
void hl_async_ctx_resume_detached(HlAsyncCtx *ctx);

/*
 * Timer callback for detached sleep: fires hl_async_ctx_resume_detached.
 * Used as the KlTimerFn for hull.sleep() in timer callbacks.
 */
void hl_detached_timer_fire(void *user_data);

#endif /* HL_ASYNC_H */
