/**
 * @file async_backend.h
 * @brief Pluggable async / event-loop backend vtable.
 *
 * The lower-level half of Hull's net interface. Owns: the event loop,
 * monotonic time, timers, FD watchers, the thread pool, and async-op
 * suspension. Used by every capability that touches async — hull.sleep,
 * compute.async, gpu.async, db.async, http.fetch, request handlers.
 *
 * Two backends ship by default:
 *
 *   keel — wraps the vendored Keel library (kl_async_*, kl_timer_*,
 *          KlThreadPool, KlEventCtx). Default for HL_ENABLE_HTTP=1
 *          builds; same primitives that back the HTTP server.
 *
 *   poll — minimal poll(2) + pthread + software-timer-heap impl.
 *          Used by HL_ENABLE_HTTP=0 (CLI) builds where Keel is
 *          unlinked. ~600-800 lines, no external deps.
 *
 * Future backends (libuv for portability, io_uring for Linux perf)
 * slot into the same vtable.
 *
 * Design questions settled in this version:
 *
 *   1. HlAsyncOp pointer semantics: by-pointer. Callers embed the
 *      op struct in their own context (`HlAsyncCtx` already does
 *      this with Keel's `KlAsyncOp`). The backend treats the
 *      pointer as opaque; container_of recovers the caller's wrapper.
 *
 *   2. TLS: out of scope. The existing KlTls vtable (used by Keel's
 *      net backend) stays separate. Future TLS backends will be a
 *      third orthogonal vtable, not part of this interface.
 *
 *   3. Allocator: passed in via init(). Hull's HlAllocator wraps
 *      KlAllocator for keel-backed builds and stdlib malloc for poll.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_ASYNC_BACKEND_H
#define HL_ASYNC_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations — backend-opaque types. */
typedef struct HlAllocator        HlAllocator;
typedef struct HlAsyncBackendCtx  HlAsyncBackendCtx;  /* event-loop instance */
typedef struct HlAsyncBackendPool HlAsyncBackendPool; /* thread pool */

/* ── HlAsyncOp ──────────────────────────────────────────────────────
 *
 * A pending async operation. Callers embed this struct in their own
 * context (HlAsyncCtx already does — see hull/async.h). The backend
 * tracks it via three callbacks set by the caller before suspend:
 *
 *   on_resume   — fires on the event-loop thread when complete().
 *                 The caller's code resumes its coroutine/promise here.
 *
 *   on_deadline — fires on the event-loop thread when deadline_ms
 *                 monotonic time is reached. Used by hull.sleep
 *                 (success) and by HTTP timeouts (failure). NULL
 *                 disables the deadline.
 *
 *   on_cancel   — fires if the connection (HTTP) dies while suspended
 *                 or the backend tears down. Caller frees any pending
 *                 state. NULL = nothing to undo.
 *
 * deadline_ms is absolute monotonic milliseconds (compare against
 * backend->monotonic_ms()). 0 disables the deadline.
 *
 * The backend may add internal fields after these; do not declare
 * sub-types or peer at the struct beyond what's here.
 */
typedef struct HlAsyncOp HlAsyncOp;
struct HlAsyncOp {
    uint64_t deadline_ms;
    void    (*on_resume)  (HlAsyncOp *op);
    void    (*on_deadline)(HlAsyncOp *op);
    void    (*on_cancel)  (HlAsyncOp *op);

    /* Backend-private state. Do not access from caller code. */
    void    *_backend_state;
};

/* ── HlAsyncWatcher mask bits ──────────────────────────────────────── */

#define HL_ASYNC_READ    (1u << 0)
#define HL_ASYNC_WRITE   (1u << 1)

/* FD watcher callback. `ready` is a subset of (HL_ASYNC_READ |
 * HL_ASYNC_WRITE) indicating which operations are now possible.
 * Runs on the event-loop thread. */
typedef void (*HlAsyncWatcherFn)(int fd, unsigned ready, void *user);

/* Timer callback — runs on the event-loop thread when the deadline
 * fires. The handle returned by timer_add is invalidated. */
typedef void (*HlAsyncTimerFn)(void *user);

/* Thread-pool work callbacks. All three may be NULL.
 *
 *   work_fn   — runs on a worker thread; blocking I/O is OK here.
 *   done_fn   — runs on the event-loop thread after work_fn returns.
 *               Safe to call backend->op_complete from here.
 *   cancel_fn — runs on the event-loop thread if the item is drained
 *               by pool_free without ever executing work_fn.
 */
typedef void (*HlAsyncWorkFn)(void *user);

/* Stop predicate for run_until. Returns 1 to break out of the loop. */
typedef int  (*HlAsyncStopFn)(void *user);

/* ── The vtable ────────────────────────────────────────────────────── */

typedef struct HlAsyncBackend {
    /* Human-readable identifier ("keel", "poll", ...). */
    const char *name;

    /* ── Event loop lifecycle ─────────────────────────────────────── */

    /* Init backend context. Returns 0 on success, -1 on error.
     * `alloc` may be NULL (use stdlib malloc/free). */
    int    (*init)(HlAsyncBackendCtx **out, HlAllocator *alloc);

    /* Free all backend resources. Pending watchers/timers/ops are
     * dropped silently; for graceful shutdown call stop() and drain
     * via run_until first. */
    void   (*free)(HlAsyncBackendCtx *ctx);

    /* Run the event loop until stop() is called or the predicate
     * returns 1. block determines whether each iteration may sleep
     * waiting for events.
     *
     * Returns 0 on clean exit, -1 on fatal backend error. */
    int    (*run)(HlAsyncBackendCtx *ctx);
    int    (*run_until)(HlAsyncBackendCtx *ctx,
                        HlAsyncStopFn stop, void *user);

    /* Request loop exit. Safe to call from any thread, any callback. */
    void   (*stop)(HlAsyncBackendCtx *ctx);

    /* ── Time ────────────────────────────────────────────────────── */

    /* Monotonic clock in milliseconds. Backend-agnostic — same value
     * across backends so deadlines are comparable. */
    uint64_t (*monotonic_ms)(void);

    /* ── Timers ──────────────────────────────────────────────────── */

    /* Schedule cb(user) to fire after ms milliseconds, on the event
     * loop thread. Returns an opaque handle (>0) for cancellation,
     * or 0 on allocation failure. */
    uint64_t (*timer_add)(HlAsyncBackendCtx *ctx,
                          uint64_t ms,
                          HlAsyncTimerFn cb, void *user);

    /* Cancel a timer by handle. No-op if already fired. */
    void   (*timer_cancel)(HlAsyncBackendCtx *ctx, uint64_t handle);

    /* ── FD watchers ─────────────────────────────────────────────── */

    /* Register cb(fd, ready, user) to fire when fd is ready for the
     * operations in `mask`. Re-arms automatically after each fire.
     *
     * Returns 0 on success, -1 on error. */
    int    (*watcher_add)(HlAsyncBackendCtx *ctx, int fd, unsigned mask,
                          HlAsyncWatcherFn cb, void *user);

    /* Change the readiness mask for an existing watcher. */
    int    (*watcher_mod)(HlAsyncBackendCtx *ctx, int fd, unsigned mask);

    /* Deregister. Pending callbacks for this fd are dropped. */
    void   (*watcher_del)(HlAsyncBackendCtx *ctx, int fd);

    /* ── Thread pool ─────────────────────────────────────────────── */

    /* Create a pool with `num_workers` worker threads and a work
     * queue capped at `queue_capacity` items. The pool wakes the
     * event loop via an internal pipe/eventfd. */
    int    (*pool_create)(HlAsyncBackendPool **out, HlAsyncBackendCtx *ctx,
                          int num_workers, int queue_capacity);

    /* Drain + join. Pending work that hasn't started yet runs cancel_fn
     * (if non-NULL); in-flight work is awaited before return. */
    void   (*pool_free)(HlAsyncBackendPool *pool);

    /* Submit a work item. Returns 0 on success, -1 if the queue is
     * full (caller decides whether to retry or fail). */
    int    (*pool_submit)(HlAsyncBackendPool *pool,
                          HlAsyncWorkFn work_fn,
                          HlAsyncWorkFn done_fn,
                          HlAsyncWorkFn cancel_fn,
                          void *user);

    /* ── Async-op suspension ─────────────────────────────────────── */

    /* Track op as in-flight. Fires op->on_deadline if the deadline
     * elapses before complete() is called; fires op->on_cancel if
     * the backend tears down. Returns 0 on success, -1 on error.
     *
     * The caller owns the op struct — backend stores no copy. */
    int    (*op_suspend)(HlAsyncBackendCtx *ctx, HlAsyncOp *op);

    /* Resume an in-flight op. Schedules op->on_resume to fire on the
     * event-loop thread. Safe to call from any thread (notably from
     * a pool worker's done_fn — though done_fn already runs on the
     * event-loop thread, this works regardless). */
    void   (*op_complete)(HlAsyncBackendCtx *ctx, HlAsyncOp *op);
} HlAsyncBackend;

/* ── Backend getter ────────────────────────────────────────────────── */

/* Returns the compiled-in async backend. Never NULL — every Hull
 * build links exactly one async backend.
 *
 *   HL_ENABLE_HTTP=1 → keel (default)
 *   HL_ENABLE_HTTP=0 → poll (CLI builds)
 */
const HlAsyncBackend *hl_async_backend(void);

#endif /* HL_ASYNC_BACKEND_H */
