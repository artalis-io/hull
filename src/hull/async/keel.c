/*
 * async/keel.c - HlAsyncBackend implementation backed by Keel.
 *
 * Wraps Keel's event loop, timers, watchers, thread pool, and
 * monotonic time behind the HlAsyncBackend vtable. The async backend
 * is logically independent of any HTTP server - Keel just happens to
 * provide a convenient implementation today. Future siblings under
 * this directory: async/poll.c (libc poll for HL_ENABLE_HTTP=0
 * builds), and any libuv / io_uring backends that come
 * later.
 *
 * Notes on the op_suspend/op_complete contract:
 *
 *   Keel's KlAsyncOp is tied to a KlHttpConn (used by HTTP request
 *   suspension). For the runtime-agnostic op_suspend path - which
 *   may be called without an HTTP connection (timer callbacks,
 *   CLI mode) - this backend tracks ops itself and uses Keel's
 *   timer system to fire on_deadline and on_resume. The actual
 *   connection-bound suspension path stays in cap/http_async.c /
 *   runtime/{lua,js}/dispatch.c, which talk to Keel directly. That
 *   layer migrates onto the vtable once HlReqHandle
 *   accessors are wired through HlNetBackend.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/async_backend.h"
#include "hull/async/keel.h"
#include "hull/utils/alloc.h"

#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/thread_pool.h>
#include <keel/clock.h>             /* kl_monotonic_ms (moved here in Keel 3.x) */
#include <keel/http_connection.h>
#include <keel/allocator.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Backend context ────────────────────────────────────────────────── */

struct HlAsyncBackendCtx {
    /* The active event loop. Either points to `kel_storage` (owned
     * mode: backend->init created it) or to an external KlEventCtx
     * passed via hl_async_backend_keel_wrap (borrowed mode: the
     * caller - typically KlHttpServer - owns it). */
    KlEventCtx   *kel;
    KlEventCtx    kel_storage;
    KlAllocator   kalloc;       /* used only in owned mode */
    HlAllocator  *alloc;        /* borrowed; may be NULL */
    /* Atomic because stop() is documented as callable from any thread, and
     * the run loops read it every iteration. A plain int here is a data race
     * by the letter of the standard, and the kind a compiler is entitled to
     * hoist out of the loop entirely. */
    _Atomic int   stop_flag;    /* run() / run_until() exit hint */
    int           borrowed;     /* 1 = wrap; free() must not destroy kel */
};

/* ── Init / free ────────────────────────────────────────────────────── */

static int keel_init(HlAsyncBackendCtx **out, HlAllocator *alloc)
{
    (void)alloc;
    if (!out) return -1;
    HlAsyncBackendCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return -1;
    ctx->alloc = alloc;
    /* Keel requires an allocator (it doesn't accept NULL). Use its
     * default malloc/free wrapper; future work could route through
     * HlAllocator instead. */
    ctx->kalloc = kl_allocator_default();
    if (kl_event_ctx_init(&ctx->kel_storage, &ctx->kalloc) != 0) {
        free(ctx);
        return -1;
    }
    ctx->kel = &ctx->kel_storage;
    ctx->borrowed = 0;
    *out = ctx;
    return 0;
}

static void keel_free(HlAsyncBackendCtx *ctx)
{
    if (!ctx) return;
    if (!ctx->borrowed) kl_event_ctx_free(&ctx->kel_storage);
    free(ctx);
}

/* Borrowed-mode constructor: wrap an existing KlEventCtx so vtable
 * consumers share it with whoever owns the loop (typically KlHttpServer). */
HlAsyncBackendCtx *hl_async_backend_keel_wrap(KlEventCtx *ev)
{
    if (!ev) return NULL;
    HlAsyncBackendCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->kel = ev;
    ctx->borrowed = 1;
    return ctx;
}

void hl_async_backend_keel_unwrap(HlAsyncBackendCtx *ctx)
{
    if (!ctx) return;
    /* No kel_event_ctx_free here - borrowed. */
    free(ctx);
}

/* ── Loop driver ───────────────────────────────────────────────────── */

/* Drive a single iteration: wait up to timeout_ms for FD events,
 * dispatch any watchers, then fire expired timers. Keel's
 * kl_event_ctx_run only handles the FD/watcher half; timers need
 * explicit kl_timer_fire (the server does the same pattern). */
/* Drive a single iteration: wait up to timeout_ms for FD events,
 * dispatch any watchers, then fire expired timers. kl_event_ctx_run
 * internally fires timers too, but calling it again here is a no-op
 * (timer heap is empty by then) and keeps the contract crisp: after
 * tick() returns, all ready timers have fired. */
static int keel_tick(HlAsyncBackendCtx *ctx, int timeout_ms)
{
    if (!ctx) return -1;
    int rc = kl_event_ctx_run(ctx->kel, 16, timeout_ms);
    if (rc < 0) return -1;
    kl_timer_fire(ctx->kel);
    return 0;
}

static int keel_run(HlAsyncBackendCtx *ctx)
{
    if (!ctx) return -1;
    ctx->stop_flag = 0;
    while (!ctx->stop_flag) {
        if (keel_tick(ctx, 1000) < 0) return -1;
    }
    return 0;
}

static int keel_run_until(HlAsyncBackendCtx *ctx,
                          HlAsyncStopFn stop, void *user)
{
    if (!ctx || !stop) return -1;
    ctx->stop_flag = 0;
    while (!ctx->stop_flag && !stop(user)) {
        if (keel_tick(ctx, 100) < 0) return -1;
    }
    return 0;
}

static void keel_stop(HlAsyncBackendCtx *ctx)
{
    if (!ctx) return;
    ctx->stop_flag = 1;
    /* Not woken. Keel's event context exposes no cross-thread wakeup, so a
     * stop() from another thread is not observed until the loop's current
     * poll returns - up to 1000 ms in run(), 100 ms in run_until(). The poll
     * backend writes its self-pipe and returns immediately.
     *
     * Left as a latency difference rather than papered over: every caller
     * today stops from a signal handler or from on-loop code, where the
     * wait is zero, and inventing a second wakeup channel for a case nobody
     * has would be more machinery than the problem. */
}

/* ── Time ──────────────────────────────────────────────────────────── */

static uint64_t keel_monotonic_ms(void) { return kl_monotonic_ms(); }

/* ── Timers ────────────────────────────────────────────────────────── */

/* Bridge between HlAsyncTimerFn and Keel's KlTimerFn - same signature.
 *
 * Handle encoding: Keel returns timer IDs >= 0 (0 is a valid handle).
 * Our vtable contract reserves 0 as "invalid"; shift Keel's id by +1
 * so we can use 0 as failure. */
static uint64_t keel_timer_add(HlAsyncBackendCtx *ctx, uint64_t ms,
                                HlAsyncTimerFn cb, void *user)
{
    if (!ctx || !cb) return 0;
    int64_t h = kl_timer_add(ctx->kel, ms, cb, user);
    return (h >= 0) ? (uint64_t)h + 1 : 0;
}

static void keel_timer_cancel(HlAsyncBackendCtx *ctx, uint64_t handle)
{
    if (!ctx || handle == 0) return;
    kl_timer_cancel(ctx->kel, (int64_t)(handle - 1));
}

/* ── FD watchers ───────────────────────────────────────────────────── */

/* KlEventMask matches HL_ASYNC_READ/WRITE bits today; if Keel ever
 * reorders the enum we'd insert a translation helper here. */
static int keel_watcher_add(HlAsyncBackendCtx *ctx, int fd, unsigned mask,
                             HlAsyncWatcherFn cb, void *user)
{
    if (!ctx || !cb) return -1;
    KlEventMask kmask = 0;
    if (mask & HL_ASYNC_READ)  kmask |= KL_EVENT_READ;
    if (mask & HL_ASYNC_WRITE) kmask |= KL_EVENT_WRITE;
    /* KlWatcherFn signature: void (*)(int fd, KlEventMask, void *).
     * HlAsyncWatcherFn:      void (*)(int fd, unsigned, void *).
     * KlEventMask is an enum (int-compatible); the bits line up. */
    return kl_watcher_add(ctx->kel, fd, kmask, (KlWatcherFn)cb, user);
}

static int keel_watcher_mod(HlAsyncBackendCtx *ctx, int fd, unsigned mask)
{
    if (!ctx) return -1;
    KlEventMask kmask = 0;
    if (mask & HL_ASYNC_READ)  kmask |= KL_EVENT_READ;
    if (mask & HL_ASYNC_WRITE) kmask |= KL_EVENT_WRITE;
    return kl_watcher_mod(ctx->kel, fd, kmask);
}

static void keel_watcher_del(HlAsyncBackendCtx *ctx, int fd)
{
    if (ctx) kl_watcher_del(ctx->kel, fd);
}

/* ── Thread pool ───────────────────────────────────────────────────── */

struct HlAsyncBackendPool {
    KlThreadPool *kpool;
};

static int keel_pool_create(HlAsyncBackendPool **out, HlAsyncBackendCtx *ctx,
                             int num_workers, int queue_capacity)
{
    if (!out || !ctx) return -1;
    HlAsyncBackendPool *p = calloc(1, sizeof *p);
    if (!p) return -1;
    KlThreadPoolConfig cfg = {
        .num_workers     = num_workers,
        .queue_capacity  = queue_capacity,
        .alloc           = NULL,
    };
    p->kpool = kl_thread_pool_create(ctx->kel, &cfg);
    if (!p->kpool) {
        free(p);
        return -1;
    }
    *out = p;
    return 0;
}

static void keel_pool_free(HlAsyncBackendPool *p)
{
    if (!p) return;
    if (p->kpool) kl_thread_pool_free(p->kpool);
    free(p);
}

static int keel_pool_submit(HlAsyncBackendPool *p,
                             HlAsyncWorkFn work_fn,
                             HlAsyncWorkFn done_fn,
                             HlAsyncWorkFn cancel_fn,
                             void *user)
{
    if (!p || !p->kpool) return -1;
    KlWorkItem item = {
        .work_fn   = work_fn,
        .done_fn   = done_fn,
        .cancel_fn = cancel_fn,
        .user_data = user,
    };
    return kl_thread_pool_submit(p->kpool, &item);
}

/* ── Async-op suspension (backend-tracked, no KlHttpConn) ─────────────── */

/* Each suspended op gets a Keel timer that fires the deadline. The
 * timer handle lives in _backend_state so op_complete can cancel it
 * before scheduling on_resume.
 *
 * op_resume scheduling: rather than calling on_resume synchronously,
 * we schedule a 0ms timer so it always fires from the event-loop
 * thread (matching the contract that on_resume runs on-loop). */

typedef struct KeelOpState {
    int64_t  deadline_timer;  /* Keel id; -1 = no timer scheduled */
    int64_t  resume_timer;    /* Keel id; -1 = none. Retractable by op_cancel */
    int      resumed;         /* op_complete called */
} KeelOpState;

/* Both timers DETACH and free the per-op state BEFORE running the callback,
 * and the order is load-bearing rather than tidy.
 *
 * A callback commonly resumes a coroutine, which runs on to its next park and
 * re-suspends THIS SAME op - installing fresh state. Freeing afterwards
 * discarded that fresh state and left _backend_state NULL, so the next
 * op_complete found nothing to resume and the coroutine parked forever (and
 * the new state leaked). Detaching first means a re-suspend inside the
 * callback owns its state outright and nothing here can clobber it.
 *
 * No consumer noticed until hull/ssh: http.fetch, compute.async and
 * gpu.async each park ONCE per operation, so none of them re-enters. A byte
 * stream does it on every read and every write.
 *
 * It is ONE function rather than the same three lines written out at each
 * site because writing them out is how the rule was lost: the reorder landed
 * on both timers and was missed on op_complete's scheduling-failure fallback,
 * which fired first and nulled afterwards for another release. A caller that
 * cannot fire without detaching cannot get the order wrong. */
static void keel_op_detach(HlAsyncOp *op)
{
    KeelOpState *s = op->_backend_state;
    op->_backend_state = NULL;
    free(s);                           /* NULL-safe */
}

static void keel_op_deadline_timer(void *ud)
{
    HlAsyncOp *op = ud;
    KeelOpState *s = op->_backend_state;
    if (!s) return;                    /* already completed and cleaned up */
    /* op_complete raced us: it has already scheduled the resume timer, and
     * THAT timer owns the state. Returning without detaching is correct; the
     * state is not leaked, it is someone else's to release. */
    if (s->resumed) return;
    keel_op_detach(op);
    if (op->on_deadline) op->on_deadline(op);
}

static void keel_op_resume_timer(void *ud)
{
    HlAsyncOp *op = ud;
    keel_op_detach(op);
    if (op->on_resume) op->on_resume(op);
}

static int keel_op_suspend(HlAsyncBackendCtx *ctx, HlAsyncOp *op)
{
    if (!ctx || !op) return -1;
    /* Already suspended. Overwriting would leak the old state and orphan its
     * deadline timer, which still points at this op. */
    if (op->_backend_state) return -1;
    KeelOpState *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->deadline_timer = -1;
    s->resume_timer   = -1;
    op->_backend_state = s;

    if (op->deadline_ms > 0 && op->on_deadline) {
        uint64_t now = kl_monotonic_ms();
        uint64_t delay = (op->deadline_ms > now) ? op->deadline_ms - now : 0;
        int64_t h = kl_timer_add(ctx->kel, delay, keel_op_deadline_timer, op);
        if (h < 0) {
            keel_op_detach(op);        /* one release path, see above */
            return -1;
        }
        s->deadline_timer = h;
    }
    return 0;
}

/* EVENT-LOOP THREAD ONLY - see the vtable comment. This reaches
 * kl_timer_add and kl_timer_cancel, and Keel's event context carries no
 * lock: a call from a worker would race the loop reallocating the timer
 * heap. The poll backend marshals instead, which is why the two look
 * different here. */
static void keel_op_complete(HlAsyncBackendCtx *ctx, HlAsyncOp *op)
{
    if (!ctx || !op) return;
    KeelOpState *s = op->_backend_state;
    if (!s) return;                    /* already completed */
    s->resumed = 1;
    if (s->deadline_timer >= 0) {
        kl_timer_cancel(ctx->kel, s->deadline_timer);
        s->deadline_timer = -1;
    }
    /* Schedule on_resume to fire on the event-loop thread. 0ms == ASAP. */
    int64_t h = kl_timer_add(ctx->kel, 0, keel_op_resume_timer, op);
    if (h >= 0) s->resume_timer = h;
    if (h < 0) {
        /* Best-effort: fire synchronously if scheduling fails. Through the
         * same detach helper as the timers, which is the whole point of it
         * being a helper - this branch had the inverted order for a release
         * because it was written out by hand. */
        keel_op_detach(op);
        if (op->on_resume) op->on_resume(op);
    }
}

/* Retract an op. See the vtable comment: op_complete defers through a 0 ms
 * timer, so an owner freeing the storage `op` lives in needs a way to
 * withdraw that timer before it fires against freed memory. */
static void keel_op_cancel(HlAsyncBackendCtx *ctx, HlAsyncOp *op)
{
    if (!ctx || !op) return;
    KeelOpState *s = op->_backend_state;
    if (!s) return;
    if (s->deadline_timer >= 0) kl_timer_cancel(ctx->kel, s->deadline_timer);
    if (s->resume_timer   >= 0) kl_timer_cancel(ctx->kel, s->resume_timer);
    keel_op_detach(op);
}

/* ── Vtable ────────────────────────────────────────────────────────── */

const HlAsyncBackend hl_async_backend_keel = {
    .name             = "keel",
    .init             = keel_init,
    .free             = keel_free,
    .tick             = keel_tick,
    .run              = keel_run,
    .run_until        = keel_run_until,
    .stop             = keel_stop,
    .monotonic_ms     = keel_monotonic_ms,
    .timer_add        = keel_timer_add,
    .timer_cancel     = keel_timer_cancel,
    .watcher_add      = keel_watcher_add,
    .watcher_mod      = keel_watcher_mod,
    .watcher_del      = keel_watcher_del,
    .pool_create      = keel_pool_create,
    .pool_free        = keel_pool_free,
    .pool_submit      = keel_pool_submit,
    .op_suspend       = keel_op_suspend,
    .op_complete      = keel_op_complete,
    .op_cancel        = keel_op_cancel,
};

/* Strong override of the weak hl_async_backend() default in async/poll.c: when
 * this TU is linked (any HTTP build, or the composed http feature -
 * docs/keel_feature.md), the Keel event loop wins. keel.c is dropped on
 * HL_ENABLE_HTTP=0, so the weak poll default in poll.c stands there instead. This
 * makes the backend choice a LINK fact rather than a compile-time #ifdef, which
 * is what lets Keel move into the http feature. Byte-identical on a full base. */
const HlAsyncBackend *hl_async_backend(void)
{
    return &hl_async_backend_keel;
}
