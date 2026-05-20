/*
 * async/keel.c — HlAsyncBackend implementation backed by Keel.
 *
 * Wraps Keel's event loop, timers, watchers, thread pool, and
 * monotonic time behind the HlAsyncBackend vtable. The async backend
 * is logically independent of any HTTP server — Keel just happens to
 * provide a convenient implementation today. Future siblings under
 * this directory: async/poll.c (libc poll for HL_ENABLE_HTTP=0
 * builds, Phase 3d-4), and any libuv / io_uring backends that come
 * later.
 *
 * Notes on the op_suspend/op_complete contract:
 *
 *   Keel's KlAsyncOp is tied to a KlConn (used by HTTP request
 *   suspension). For the runtime-agnostic op_suspend path — which
 *   may be called without an HTTP connection (timer callbacks,
 *   CLI mode) — this backend tracks ops itself and uses Keel's
 *   timer system to fire on_deadline and on_resume. The actual
 *   connection-bound suspension path stays in cap/http_async.c /
 *   runtime/{lua,js}/dispatch.c, which talk to Keel directly. That
 *   layer migrates onto the vtable in Phase 3d-3 once HlReqHandle
 *   accessors are wired through HlNetBackend.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/async_backend.h"
#include "hull/async/keel.h"
#include "hull/alloc.h"

#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/thread_pool.h>
#include <keel/connection.h>   /* kl_monotonic_ms */
#include <keel/allocator.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Backend context ────────────────────────────────────────────────── */

struct HlAsyncBackendCtx {
    /* The active event loop. Either points to `kel_storage` (owned
     * mode: backend->init created it) or to an external KlEventCtx
     * passed via hl_async_backend_keel_wrap (borrowed mode: the
     * caller — typically KlServer — owns it). */
    KlEventCtx   *kel;
    KlEventCtx    kel_storage;
    KlAllocator   kalloc;       /* used only in owned mode */
    HlAllocator  *alloc;        /* borrowed; may be NULL */
    int           stop_flag;    /* run() / run_until() exit hint */
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
 * consumers share it with whoever owns the loop (typically KlServer). */
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
    /* No kel_event_ctx_free here — borrowed. */
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
    if (ctx) ctx->stop_flag = 1;
}

/* ── Time ──────────────────────────────────────────────────────────── */

static uint64_t keel_monotonic_ms(void) { return kl_monotonic_ms(); }

/* ── Timers ────────────────────────────────────────────────────────── */

/* Bridge between HlAsyncTimerFn and Keel's KlTimerFn — same signature.
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

/* ── Async-op suspension (backend-tracked, no KlConn) ─────────────── */

/* Each suspended op gets a Keel timer that fires the deadline. The
 * timer handle lives in _backend_state so op_complete can cancel it
 * before scheduling on_resume.
 *
 * op_resume scheduling: rather than calling on_resume synchronously,
 * we schedule a 0ms timer so it always fires from the event-loop
 * thread (matching the contract that on_resume runs on-loop). */

typedef struct KeelOpState {
    HlAsyncBackendCtx *ctx;
    int64_t  deadline_timer;  /* Keel id; -1 = no timer scheduled */
    int      resumed;         /* op_complete called */
} KeelOpState;

static void keel_op_deadline_timer(void *ud)
{
    HlAsyncOp *op = ud;
    KeelOpState *s = op->_backend_state;
    if (s->resumed) return;            /* op_complete raced us */
    s->deadline_timer = 0;
    if (op->on_deadline) op->on_deadline(op);
    /* The caller is expected to free the op or set it up for re-use
     * inside on_deadline. We free the state here; the op struct
     * itself belongs to the caller. */
    free(s);
    op->_backend_state = NULL;
}

static void keel_op_resume_timer(void *ud)
{
    HlAsyncOp *op = ud;
    KeelOpState *s = op->_backend_state;
    if (op->on_resume) op->on_resume(op);
    free(s);
    op->_backend_state = NULL;
}

static int keel_op_suspend(HlAsyncBackendCtx *ctx, HlAsyncOp *op)
{
    if (!ctx || !op) return -1;
    KeelOpState *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->ctx = ctx;
    s->deadline_timer = -1;
    op->_backend_state = s;

    if (op->deadline_ms > 0 && op->on_deadline) {
        uint64_t now = kl_monotonic_ms();
        uint64_t delay = (op->deadline_ms > now) ? op->deadline_ms - now : 0;
        int64_t h = kl_timer_add(ctx->kel, delay, keel_op_deadline_timer, op);
        if (h < 0) {
            free(s);
            op->_backend_state = NULL;
            return -1;
        }
        s->deadline_timer = h;
    }
    return 0;
}

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
    if (h < 0) {
        /* Best-effort: fire synchronously if scheduling fails. */
        if (op->on_resume) op->on_resume(op);
        free(s);
        op->_backend_state = NULL;
    }
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
};

/*
 * Backend selection.
 *
 * HL_ENABLE_HTTP=1 (default): the keel backend wins. HTTP-server-y
 *   code shares the same KlEventCtx that backs kl_server_*.
 *
 * HL_ENABLE_HTTP=0 (CLI builds, Phase 3d-5): the poll backend wins;
 *   keel.c isn't even compiled in (Makefile drops async/keel.c from
 *   ASYNC_BACKEND_SRCS when HL_ENABLE_HTTP=0), and the link drops
 *   libkeel.a.
 *
 * The vtable is picked at compile time so the hot-path call
 * (hl_async_backend()->whatever) inlines through one indirection,
 * not two.
 */
extern const HlAsyncBackend hl_async_backend_poll;

const HlAsyncBackend *hl_async_backend(void)
{
#ifdef HL_ENABLE_HTTP
    return &hl_async_backend_keel;
#else
    return &hl_async_backend_poll;
#endif
}
