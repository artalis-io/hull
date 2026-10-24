/*
 * async.h - Runtime-agnostic async continuation layer
 *
 * Bridges Keel async primitives (KlAsyncOp, KlWatcher) with Hull
 * runtime continuations (Lua coroutines, JS promises).
 *
 * HlAsyncCont is a vtable implemented per runtime. HlAsyncCtx is the
 * runtime-agnostic glue that owns a KlAsyncOp, a driver, and a cont.
 * The ctx dispatches through the vtable - it never imports Lua or JS
 * headers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_ASYNC_H
#define HL_ASYNC_H

#include <keel/async.h>
#include "hull/shared/offheap.h"
#include "hull/cap/http_redirect_guard.h"

/* Forward declarations */
typedef struct HlAllocator       HlAllocator;
typedef struct HlNetBackendCtx   HlNetBackendCtx;
typedef struct HlAsyncBackendCtx HlAsyncBackendCtx;

/* ── HlAsyncTeardown - what a runtime must stop before it goes ────────
 *
 * Shutdown order (audit 14 G1 / audit 15): the Keel server is freed BEFORE
 * the runtime, because its teardown cancels the ops it holds and closes every
 * connection (a WebSocket's on_close included), and all of that calls back
 * into the runtime. What the runtime holds on the loop that Keel does NOT
 * know about - a detached op (an http.fetch, a hull.sleep, a tui.poll started
 * from a timer, a WebSocket callback or app.main), a WebSocket client - is
 * linked into the runtime's teardown list (HlRuntime.async_live) and stopped
 * by hl_async_teardown_run while the runtime and the loop are both alive.
 * Such an op used to outlive both: its continuation leaked (QuickJS then
 * asserted in JS_FreeRuntime) and its driver later touched a freed loop.
 *
 * Embedded in its owner; the owner unlinks it (hl_async_teardown_unlink)
 * whenever it retires normally. */
typedef struct HlAsyncTeardown HlAsyncTeardown;
struct HlAsyncTeardown {
    HlAsyncTeardown  *next;
    HlAsyncTeardown **pprev;          /* NULL = not linked */
    /* Stop the owner now. @p loop is the runtime's async context (NULL when
     * it has none). Called once, already unlinked; may free the owner. */
    void (*stop)(HlAsyncTeardown *self, HlAsyncBackendCtx *loop);
};

void hl_async_teardown_link(HlAsyncTeardown **list, HlAsyncTeardown *t);
void hl_async_teardown_unlink(HlAsyncTeardown *t);   /* no-op when unlinked */

/* Stop every linked entry until the list is empty (a stop may unlink others). */
void hl_async_teardown_run(HlAsyncTeardown **list, HlAsyncBackendCtx *loop);

/* ── HlAsyncCont - runtime continuation vtable ────────────────────── */

typedef struct HlAsyncCont {
    /*
     * Resume the handler after the async operation completes.
     * For Lua: lua_resume(co, ...). For JS: resolve promise + run jobs.
     *
     * Keel 3.x owns the send: resume runs inside kl_async_complete, which
     * drives the send (incl. would-block) after this returns. So resume only
     * FINALIZES the response - it does NOT drive the connection state machine:
     *   - completed (buffered): build the response on kl_http_conn_response(conn)
     *     and return; Keel sends it.
     *   - error: kl_http_conn_response(conn) + build a 5xx; Keel sends it.
     *   - stream end: kl_http_response_end_stream(kl_http_conn_response(conn)).
     *   - re-yield: the yielding path re-suspends (kl_async_suspend); the current
     *     complete then no-ops. resume writes nothing extra.
     *
     * `driver` is the Keel driver result (NULL for sleep, KlHttpClient*
     * for HTTP, etc.).
     */
    void (*resume)(struct HlAsyncCont *self, void *driver);

    /*
     * Cancel the handler without invoking - free runtime refs.
     * Called when the connection is closed while suspended, or on
     * server shutdown.
     */
    void (*cancel)(struct HlAsyncCont *self);

    /*
     * Free the cont struct itself. Called after resume or cancel.
     * Does NOT free the coroutine/promise ref - that's managed by
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
     * pointer (the cont implementation does the cast - keeps the
     * generic vtable runtime-independent).
     *
     * NULL on Lua conts (Lua tracks handler completion directly via
     * lua_resume status, not through a stored promise on the cont).
     *
     * Each JS cont type sets this slot to its own setter so the public
     * helper hl_js_async_cont_set_handler_promise can dispatch without
     * caring about the concrete type - avoids the vtable-identity
     * trick that used to live in async.c (which would silently
     * corrupt memory if a new cont type was added without updating
     * the dispatcher).
     */
    void (*set_handler_promise)(struct HlAsyncCont *self,
                                  void *ctx, void *promise);
} HlAsyncCont;

/* ── HlAsyncCtx - runtime-agnostic async glue ─────────────────────── */

typedef struct HlAsyncCtx {
    KlAsyncOp    op;            /* embedded - container_of to get ctx */
    KlHttpServer    *server;        /* TODO: retire once all consumers route via net_ctx */
    HlNetBackendCtx *net_ctx;   /* borrowed from rt->net_ctx */

    /* Keel driver - opaque to the ctx */
    void        *driver;        /* KlHttpClient*, NULL for sleep, etc. */
    void       (*free_driver)(void *driver);

    /* Runtime continuation - vtable dispatch, no runtime knowledge */
    HlAsyncCont *cont;          /* Lua, JS, or any future runtime */

    HlAllocator *alloc;
    int          detached;      /* 1 = no connection (timer callback) */

    /* A detached op's own deadline (op.deadline_ms is armed only by
     * hl_net_op_suspend, i.e. on the attached path): the Keel timer id + 1,
     * 0 = none. Set and cleared by the op's owner (http_async.c). Lua's
     * detached hull.sleep keeps its backend timer handle here. */
    int64_t      detached_timer;

    /* Teardown entry of a tracked detached op (hl_async_ctx_track). Every
     * path that retires the ctx unlinks it. */
    HlAsyncTeardown track;
    /* Optional: stop the op's driver (a timer, an in-flight request) before
     * a teardown cancels the ctx. Set by the op's owner. */
    void       (*on_shutdown)(struct HlAsyncCtx *ctx, HlAsyncBackendCtx *loop);

    /* Off-heap bytes the op's input copy holds on the submitting VM's
     * total (offheap.h): an http.fetch body Keel copied. Given back when
     * the ctx goes, on every path (audit 22). */
    HlOffheapHold hold;

    /* http.fetch's redirect policy, read by its per-hop hook (http_async.c):
     * the sealed HlHttpConfig, whether the chain began on https - such a
     * chain may not continue on http (audit 25) - and its first origin, which
     * a request with a body or a credential-like header may not leave
     * (audit 26). */
    HlHttpRedirectGuard http_guard;
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
HlAsyncCtx *hl_async_ctx_create(KlHttpServer *s, HlNetBackendCtx *net_ctx,
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
 * Cancel an async context WITHOUT resuming the handler: cont->cancel, then free
 * driver + cont, then free ctx. This is the exact body Keel's on_cancel path
 * runs, exposed so a caller tearing down a still-suspended op itself (the SMTP
 * shutdown sweep, for a DETACHED op that has no Keel async op to kl_async_cancel)
 * gets identical, single-owner teardown. For an ATTACHED op prefer
 * kl_async_cancel(server, &ctx->op) so Keel also retires the op from its active
 * list; use this directly only for the detached (connectionless) case.
 */
void hl_async_ctx_cancel(HlAsyncCtx *ctx);

/*
 * Link a DETACHED ctx into its runtime's teardown list (HlRuntime.async_live):
 * a runtime shutting down then cancels it (on_shutdown, then
 * hl_async_ctx_cancel) while the runtime and the loop are alive. Not for a
 * pool op (pool_free retires those) nor an attached one (the server's
 * teardown cancels those).
 */
void hl_async_ctx_track(HlAsyncCtx *ctx, HlAsyncTeardown **list);

/*
 * A pool op's cancel_fn: its work never ran and its done_fn never will.
 * Releases the continuation - for an ATTACHED op through the net backend
 * first, which unlinks the op from the server's active list (the op's
 * on_cancel then releases the continuation and leaves the ctx) - and frees
 * the ctx. Freeing the ctx while its op was still linked left the server's
 * teardown walking freed memory, and a detached op's continuation was never
 * released (audit 14 G1). Call it before freeing the driver (an op's
 * on_cancel reads it); it does not free the driver.
 */
void hl_async_ctx_drop_unrun(HlAsyncCtx *ctx);

/*
 * Timer callback for detached sleep: fires hl_async_ctx_resume_detached.
 * Used as the KlTimerFn for hull.sleep() in timer callbacks.
 */
void hl_detached_timer_fire(void *user_data);

#endif /* HL_ASYNC_H */
