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
typedef struct HlAllocator     HlAllocator;
typedef struct HlNetBackendCtx HlNetBackendCtx;

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

    /*
     * JS-only: attach the outer handler-Promise to this cont.
     *
     * Called by hl_js_dispatch after detecting a PENDING handler
     * return, and by JS conts transferring the handler-Promise to a
     * newly-created cont when the handler re-yields inside microtasks.
     *
     * `ctx` is a `JSContext*`, `promise` is a `JSValue` passed by
     * pointer (the cont implementation does the cast — keeps the
     * generic vtable runtime-independent).
     *
     * NULL on Lua conts (Lua tracks handler completion directly via
     * lua_resume status, not through a stored promise on the cont).
     *
     * Each JS cont type sets this slot to its own setter so the public
     * helper hl_js_async_cont_set_handler_promise can dispatch without
     * caring about the concrete type — avoids the vtable-identity
     * trick that used to live in async.c (which would silently
     * corrupt memory if a new cont type was added without updating
     * the dispatcher).
     */
    void (*set_handler_promise)(struct HlAsyncCont *self,
                                  void *ctx, void *promise);
} HlAsyncCont;

/* ── HlAsyncCtx — runtime-agnostic async glue ─────────────────────── */

typedef struct HlAsyncCtx {
    KlAsyncOp    op;            /* embedded — container_of to get ctx */
    KlServer    *server;        /* TODO: retire once all consumers route via net_ctx */
    HlNetBackendCtx *net_ctx;   /* borrowed from rt->net_ctx */

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
 * Then call hl_net_op_suspend(net_ctx, conn, &ctx->op).
 *
 * `net_ctx` is borrowed from the runtime (rt->net_ctx). It may be
 * NULL only for connectionless / detached callers; suspend-based
 * paths must pass a real backend ctx.
 */
HlAsyncCtx *hl_async_ctx_create(KlServer *s, HlNetBackendCtx *net_ctx,
                                HlAllocator *alloc);

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
