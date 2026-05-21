/*
 * async/poll.c — HlAsyncBackend implementation using POSIX poll(2)
 *                + pthreads. No external dependencies.
 *
 * The keel backend (async/keel.c) routes everything through Keel's
 * KlEventCtx; this one is freestanding: a software timer min-heap, a
 * flat FD-watcher table, a pthread-pool, and a self-pipe used to
 * wake poll() from foreign threads. Same vtable shape, no Keel link.
 *
 * Used by HL_ENABLE_HTTP=0 (CLI) builds where Keel is unlinked
 * (Phase 3d-5 wires the actual selection); the rest of Hull doesn't
 * notice the swap because all access already goes through
 * hl_async_backend().
 *
 * Threading model
 * ───────────────
 *
 *   * One event-loop thread drives ticks. All on_resume / on_deadline /
 *     on_cancel / done_fn / watcher callbacks fire on this thread.
 *
 *   * Worker threads (pool) run work_fn. When work_fn returns they
 *     enqueue a completion (done_fn + user) on the ctx's cross-thread
 *     completion queue and wake the event loop via the self-pipe.
 *
 *   * Any thread can call timer_add, op_complete, pool_submit, stop —
 *     each takes the ctx lock and (when relevant) writes the wakeup
 *     pipe so a blocking poll() returns immediately.
 *
 * Memory
 * ──────
 *
 * Uses stdlib malloc/free regardless of `alloc` (matches keel impl);
 * HlAllocator routing is future work tracked in the design doc's
 * "open questions" section.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/async_backend.h"
#include "hull/alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Forward decls (struct types defined below) ────────────────────── */

typedef struct PollTimer      PollTimer;
typedef struct PollWatcher    PollWatcher;
typedef struct PollCompletion PollCompletion;

/* ── Timer (min-heap entry, addressed by stable id) ─────────────────── */

struct PollTimer {
    uint64_t       deadline_ms;
    uint64_t       id;            /* monotonically assigned, never reused */
    HlAsyncTimerFn cb;
    void          *user;
    int            cancelled;     /* set by timer_cancel; freed on next pop */
};

/* ── FD watcher ─────────────────────────────────────────────────────── */

struct PollWatcher {
    int               fd;
    unsigned          mask;
    HlAsyncWatcherFn  cb;
    void             *user;
};

/* ── Cross-thread completion ────────────────────────────────────────── */

/*
 * Pool done_fn dispatch + op_complete go through here. The event-loop
 * drain runs them all on the event-loop thread before returning from
 * tick(), so callers can assume on_resume / done_fn are on-loop.
 */
struct PollCompletion {
    HlAsyncWorkFn fn;
    void         *user;
};

/* ── Backend context ────────────────────────────────────────────────── */

struct HlAsyncBackendCtx {
    HlAllocator *alloc;          /* borrowed; may be NULL */

    /* Cross-thread wakeup: write any byte to wakeup_pipe[1] to make
     * the next poll() return immediately. Drained at the top of tick. */
    int                wakeup_pipe[2];

    /* Single big lock — Hull doesn't have hot enough async traffic to
     * justify striping today, and the design doc lists fine-grained
     * locking as future work. */
    pthread_mutex_t    lock;

    int                stop_flag;       /* run() / run_until() exit hint */

    /* Timer min-heap */
    PollTimer        **timers;          /* heap of pointers — stable ids */
    size_t             timer_count;
    size_t             timer_cap;
    uint64_t           next_timer_id;   /* always > 0; 0 = invalid handle */

    /* FD watcher table — flat array, linear scan. Typical Hull apps
     * have at most a few thousand FDs (server has its own table). */
    PollWatcher       *watchers;
    size_t             watcher_count;
    size_t             watcher_cap;

    /* Cross-thread completion queue */
    PollCompletion    *completions;
    size_t             completion_count;
    size_t             completion_cap;
};

/* ── Monotonic clock ────────────────────────────────────────────────── */

static uint64_t poll_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ── Wakeup pipe helpers ────────────────────────────────────────────── */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void wakeup_write(HlAsyncBackendCtx *ctx)
{
    if (ctx->wakeup_pipe[1] < 0) return;
    char b = 'x';
    /* EAGAIN means the pipe already has a wakeup pending — that's fine,
     * one byte is enough. Other errors are ignored: a missed wakeup
     * only delays handling by one tick. */
    (void)write(ctx->wakeup_pipe[1], &b, 1);
}

static void wakeup_drain(HlAsyncBackendCtx *ctx)
{
    if (ctx->wakeup_pipe[0] < 0) return;
    char buf[64];
    while (read(ctx->wakeup_pipe[0], buf, sizeof buf) > 0)
        ;   /* drain until EAGAIN */
}

/* ── Init / free ────────────────────────────────────────────────────── */

static int poll_init(HlAsyncBackendCtx **out, HlAllocator *alloc)
{
    if (!out) return -1;
    HlAsyncBackendCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return -1;

    ctx->alloc = alloc;
    ctx->wakeup_pipe[0] = -1;
    ctx->wakeup_pipe[1] = -1;
    ctx->next_timer_id  = 1;       /* reserve 0 as the invalid handle */

    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        free(ctx);
        return -1;
    }

    if (pipe(ctx->wakeup_pipe) != 0
        || set_nonblocking(ctx->wakeup_pipe[0]) != 0
        || set_nonblocking(ctx->wakeup_pipe[1]) != 0) {
        if (ctx->wakeup_pipe[0] >= 0) close(ctx->wakeup_pipe[0]);
        if (ctx->wakeup_pipe[1] >= 0) close(ctx->wakeup_pipe[1]);
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return -1;
    }

    *out = ctx;
    return 0;
}

static void poll_free(HlAsyncBackendCtx *ctx)
{
    if (!ctx) return;
    /* Drop pending state — graceful shutdown is the caller's job
     * (call stop() + drain via tick before free). */
    for (size_t i = 0; i < ctx->timer_count; i++)
        free(ctx->timers[i]);
    free(ctx->timers);
    free(ctx->watchers);
    free(ctx->completions);

    if (ctx->wakeup_pipe[0] >= 0) close(ctx->wakeup_pipe[0]);
    if (ctx->wakeup_pipe[1] >= 0) close(ctx->wakeup_pipe[1]);

    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

/* ── Stop flag ──────────────────────────────────────────────────────── */

static void poll_stop(HlAsyncBackendCtx *ctx)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->stop_flag = 1;
    pthread_mutex_unlock(&ctx->lock);
    wakeup_write(ctx);
}

/* ── Timer min-heap ────────────────────────────────────────────────── */

/*
 * Pointer-heap so timer ids are stable across heap moves: the
 * PollTimer lives at a fixed allocation, only the heap slot changes.
 * timer_cancel marks the entry without disturbing the heap; the
 * cancelled flag is checked when popping.
 *
 * The locking contract is "ctx->lock held by caller" for sift_* and
 * heap_push/pop; timer_add/cancel take it themselves and wake the
 * loop so a foreign-thread caller doesn't oversleep poll().
 */

static void heap_swap(HlAsyncBackendCtx *ctx, size_t a, size_t b)
{
    PollTimer *tmp = ctx->timers[a];
    ctx->timers[a] = ctx->timers[b];
    ctx->timers[b] = tmp;
}

static void heap_sift_up(HlAsyncBackendCtx *ctx, size_t idx)
{
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (ctx->timers[parent]->deadline_ms <= ctx->timers[idx]->deadline_ms)
            break;
        heap_swap(ctx, parent, idx);
        idx = parent;
    }
}

static void heap_sift_down(HlAsyncBackendCtx *ctx, size_t idx)
{
    size_t n = ctx->timer_count;
    for (;;) {
        size_t left = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        size_t smallest = idx;
        if (left < n && ctx->timers[left]->deadline_ms < ctx->timers[smallest]->deadline_ms)
            smallest = left;
        if (right < n && ctx->timers[right]->deadline_ms < ctx->timers[smallest]->deadline_ms)
            smallest = right;
        if (smallest == idx) break;
        heap_swap(ctx, idx, smallest);
        idx = smallest;
    }
}

static int heap_push(HlAsyncBackendCtx *ctx, PollTimer *t)
{
    if (ctx->timer_count == ctx->timer_cap) {
        size_t ncap = ctx->timer_cap ? ctx->timer_cap * 2 : 16;
        PollTimer **n = realloc(ctx->timers, ncap * sizeof(*n));
        if (!n) return -1;
        ctx->timers = n;
        ctx->timer_cap = ncap;
    }
    ctx->timers[ctx->timer_count++] = t;
    heap_sift_up(ctx, ctx->timer_count - 1);
    return 0;
}

/* Pop the smallest-deadline entry. Caller takes ownership and frees. */
static PollTimer *heap_pop(HlAsyncBackendCtx *ctx)
{
    if (ctx->timer_count == 0) return NULL;
    PollTimer *top = ctx->timers[0];
    ctx->timer_count--;
    if (ctx->timer_count > 0) {
        ctx->timers[0] = ctx->timers[ctx->timer_count];
        heap_sift_down(ctx, 0);
    }
    return top;
}

/* ── FD watcher table ──────────────────────────────────────────────── */

/* Linear search by fd. Hull's typical app has at most a few thousand
 * FDs; the server keeps its own table and watchers here are mostly
 * for HTTP client connections + ws upgrades. */
static ssize_t watcher_find(HlAsyncBackendCtx *ctx, int fd)
{
    for (size_t i = 0; i < ctx->watcher_count; i++)
        if (ctx->watchers[i].fd == fd) return (ssize_t)i;
    return -1;
}

static int poll_watcher_add(HlAsyncBackendCtx *ctx, int fd, unsigned mask,
                              HlAsyncWatcherFn cb, void *user)
{
    if (!ctx || fd < 0 || !cb) return -1;
    pthread_mutex_lock(&ctx->lock);
    if (watcher_find(ctx, fd) >= 0) {
        /* Same fd already registered — caller should use watcher_mod
         * to change mask; refuse rather than silently leak the prior
         * cb. */
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }
    if (ctx->watcher_count == ctx->watcher_cap) {
        size_t ncap = ctx->watcher_cap ? ctx->watcher_cap * 2 : 8;
        PollWatcher *n = realloc(ctx->watchers, ncap * sizeof(*n));
        if (!n) {
            pthread_mutex_unlock(&ctx->lock);
            return -1;
        }
        ctx->watchers = n;
        ctx->watcher_cap = ncap;
    }
    ctx->watchers[ctx->watcher_count++] = (PollWatcher){
        .fd = fd, .mask = mask, .cb = cb, .user = user,
    };
    pthread_mutex_unlock(&ctx->lock);
    wakeup_write(ctx);
    return 0;
}

static int poll_watcher_mod(HlAsyncBackendCtx *ctx, int fd, unsigned mask)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->lock);
    ssize_t idx = watcher_find(ctx, fd);
    if (idx < 0) {
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }
    ctx->watchers[idx].mask = mask;
    pthread_mutex_unlock(&ctx->lock);
    wakeup_write(ctx);
    return 0;
}

static void poll_watcher_del(HlAsyncBackendCtx *ctx, int fd)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    ssize_t idx = watcher_find(ctx, fd);
    if (idx >= 0) {
        /* Swap-with-last + shrink count. Order doesn't matter. */
        ctx->watchers[idx] = ctx->watchers[ctx->watcher_count - 1];
        ctx->watcher_count--;
    }
    pthread_mutex_unlock(&ctx->lock);
}

/* ── Tick ──────────────────────────────────────────────────────────── */

/*
 * One iteration:
 *   1. Compute timeout (next-timer-deadline, capped by caller's request).
 *   2. Build pollfd[] from watcher table + wakeup_pipe.
 *   3. poll().
 *   4. Drain completion queue.
 *   5. Fire any expired timers.
 *   6. Dispatch any ready FDs.
 *
 * Callbacks fire WITHOUT the ctx lock held (they may call back into
 * timer_add / op_complete / etc.). We snapshot the relevant lists
 * under the lock, release, then invoke. New entries arriving during a
 * callback are picked up next tick.
 */
static int poll_tick(HlAsyncBackendCtx *ctx, int timeout_ms)
{
    if (!ctx) return -1;

    /* ── Step 1+2: snapshot watchers + compute timeout under the lock. */
    pthread_mutex_lock(&ctx->lock);

    /* Cap timeout at the next-timer deadline so we don't oversleep. */
    int effective_timeout = timeout_ms;
    if (ctx->timer_count > 0) {
        uint64_t now = poll_monotonic_ms();
        uint64_t next = ctx->timers[0]->deadline_ms;
        int t = (next > now) ? (int)(next - now) : 0;
        if (effective_timeout < 0 || t < effective_timeout)
            effective_timeout = t;
    }

    /* +1 for the wakeup pipe slot. Allocate on the heap; typical
     * watcher count is small but we don't want to over-promise stack
     * frames. */
    size_t nfds = ctx->watcher_count + 1;
    struct pollfd *pfds = calloc(nfds, sizeof *pfds);
    if (!pfds) {
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }

    /* Slot 0: wakeup pipe read end. */
    pfds[0].fd     = ctx->wakeup_pipe[0];
    pfds[0].events = POLLIN;

    /* Snapshot watcher fd+mask so a callback that mutates the table
     * doesn't invalidate our pfd iteration. */
    typedef struct { int fd; unsigned mask; HlAsyncWatcherFn cb; void *user; } Snap;
    Snap *snaps = calloc(ctx->watcher_count, sizeof *snaps);
    if (ctx->watcher_count > 0 && !snaps) {
        free(pfds);
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }
    for (size_t i = 0; i < ctx->watcher_count; i++) {
        PollWatcher *w = &ctx->watchers[i];
        snaps[i] = (Snap){ w->fd, w->mask, w->cb, w->user };
        pfds[i + 1].fd = w->fd;
        short ev = 0;
        if (w->mask & HL_ASYNC_READ)  ev |= POLLIN;
        if (w->mask & HL_ASYNC_WRITE) ev |= POLLOUT;
        pfds[i + 1].events = ev;
    }
    pthread_mutex_unlock(&ctx->lock);

    /* ── Step 3: poll(). */
    int rc = poll(pfds, (nfds_t)nfds, effective_timeout);
    if (rc < 0 && errno == EINTR) {
        free(pfds);
        free(snaps);
        return 0;
    }
    if (rc < 0) {
        free(pfds);
        free(snaps);
        return -1;
    }

    /* ── Step 4: drain wakeup pipe + completion queue. */
    if (pfds[0].revents & POLLIN) wakeup_drain(ctx);

    /* Run completions on the event-loop thread. Drain to a local list
     * so callbacks that submit more work don't recurse infinitely
     * inside this tick. */
    pthread_mutex_lock(&ctx->lock);
    PollCompletion *batch = ctx->completions;
    size_t batch_n  = ctx->completion_count;
    ctx->completions       = NULL;
    ctx->completion_count  = 0;
    ctx->completion_cap    = 0;
    pthread_mutex_unlock(&ctx->lock);

    for (size_t i = 0; i < batch_n; i++) {
        if (batch[i].fn) batch[i].fn(batch[i].user);
    }
    free(batch);

    /* ── Step 5: fire expired timers. Pop under the lock, fire after. */
    for (;;) {
        pthread_mutex_lock(&ctx->lock);
        if (ctx->timer_count == 0
            || ctx->timers[0]->deadline_ms > poll_monotonic_ms()) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
        PollTimer *t = heap_pop(ctx);
        pthread_mutex_unlock(&ctx->lock);
        if (!t->cancelled && t->cb) t->cb(t->user);
        free(t);
    }

    /* ── Step 6: dispatch ready FDs from the snapshot.
     * Walk the snapshot length (nfds - 1 entries; the wakeup pipe
     * occupies slot 0), not the live watcher count — the table may
     * have grown or shrunk while a completion ran. */
    size_t snap_n = nfds - 1;
    for (size_t i = 0; i < snap_n; i++) {
        short re = pfds[i + 1].revents;
        if (re == 0) continue;
        unsigned ready = 0;
        if (re & (POLLIN | POLLHUP | POLLERR)) ready |= HL_ASYNC_READ;
        if (re & POLLOUT) ready |= HL_ASYNC_WRITE;
        if (ready && snaps[i].cb) snaps[i].cb(snaps[i].fd, ready, snaps[i].user);
    }

    free(pfds);
    free(snaps);
    return 0;
}

static int poll_run(HlAsyncBackendCtx *ctx)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->lock);
    ctx->stop_flag = 0;
    pthread_mutex_unlock(&ctx->lock);
    for (;;) {
        pthread_mutex_lock(&ctx->lock);
        int stopped = ctx->stop_flag;
        pthread_mutex_unlock(&ctx->lock);
        if (stopped) return 0;
        if (poll_tick(ctx, 1000) < 0) return -1;
    }
}

static int poll_run_until(HlAsyncBackendCtx *ctx, HlAsyncStopFn stop, void *user)
{
    if (!ctx || !stop) return -1;
    pthread_mutex_lock(&ctx->lock);
    ctx->stop_flag = 0;
    pthread_mutex_unlock(&ctx->lock);
    for (;;) {
        pthread_mutex_lock(&ctx->lock);
        int stopped = ctx->stop_flag;
        pthread_mutex_unlock(&ctx->lock);
        if (stopped || stop(user)) return 0;
        if (poll_tick(ctx, 100) < 0) return -1;
    }
}

static uint64_t poll_timer_add(HlAsyncBackendCtx *ctx, uint64_t ms,
                                HlAsyncTimerFn cb, void *user)
{
    if (!ctx || !cb) return 0;
    PollTimer *t = calloc(1, sizeof *t);
    if (!t) return 0;
    t->cb          = cb;
    t->user        = user;
    t->deadline_ms = poll_monotonic_ms() + ms;

    pthread_mutex_lock(&ctx->lock);
    t->id = ctx->next_timer_id++;
    if (heap_push(ctx, t) != 0) {
        pthread_mutex_unlock(&ctx->lock);
        free(t);
        return 0;
    }
    pthread_mutex_unlock(&ctx->lock);
    /* Foreign-thread caller may have shrunk the wait deadline — wake
     * poll() so it doesn't oversleep. Cheap; one byte. */
    wakeup_write(ctx);
    return t->id;
}

static void poll_timer_cancel(HlAsyncBackendCtx *ctx, uint64_t handle)
{
    if (!ctx || handle == 0) return;
    pthread_mutex_lock(&ctx->lock);
    /* Linear scan — Hull's timer count is small (low hundreds at most),
     * and timer cancellations are rare. Switching to a side index would
     * pay for itself only at much higher counts. */
    for (size_t i = 0; i < ctx->timer_count; i++) {
        if (ctx->timers[i]->id == handle) {
            ctx->timers[i]->cancelled = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
}

/* ── Thread pool ───────────────────────────────────────────────────── */

/*
 * Bounded ring buffer + N worker threads. Workers pop from the ring,
 * call work_fn, then enqueue done_fn onto the ctx's completion queue
 * so it fires on the event-loop thread (drained at the top of tick).
 *
 * Pending items at pool_free time get cancel_fn() called inline on
 * the freeing thread — matches Keel's semantics.
 */

typedef struct PollWorkItem {
    HlAsyncWorkFn work_fn;
    HlAsyncWorkFn done_fn;
    HlAsyncWorkFn cancel_fn;
    void         *user;
} PollWorkItem;

struct HlAsyncBackendPool {
    HlAsyncBackendCtx *ctx;          /* borrowed; for wakeup_write + completion queue */

    pthread_mutex_t    queue_lock;
    pthread_cond_t     queue_cv;     /* signalled on new item OR shutdown */

    PollWorkItem      *queue;
    size_t             queue_cap;
    size_t             queue_head;   /* next-to-pop */
    size_t             queue_tail;   /* next-to-push */
    size_t             queue_count;
    int                shutdown;

    pthread_t         *workers;
    int                num_workers;
};

/* Enqueue a completion on the ctx (called from worker thread).
 * Grows the completion array under ctx->lock. */
static void completion_enqueue(HlAsyncBackendCtx *ctx,
                                HlAsyncWorkFn fn, void *user)
{
    if (!fn) return;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->completion_count == ctx->completion_cap) {
        size_t ncap = ctx->completion_cap ? ctx->completion_cap * 2 : 16;
        PollCompletion *n = realloc(ctx->completions, ncap * sizeof *n);
        if (!n) {
            /* OOM: drop the completion. Caller's done_fn won't fire;
             * the corresponding work has already happened. Same
             * failure mode as a queue overflow. */
            pthread_mutex_unlock(&ctx->lock);
            return;
        }
        ctx->completions = n;
        ctx->completion_cap = ncap;
    }
    ctx->completions[ctx->completion_count++] = (PollCompletion){ fn, user };
    pthread_mutex_unlock(&ctx->lock);
    wakeup_write(ctx);
}

static void *worker_main(void *arg)
{
    HlAsyncBackendPool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->queue_lock);
        while (p->queue_count == 0 && !p->shutdown)
            pthread_cond_wait(&p->queue_cv, &p->queue_lock);

        if (p->queue_count == 0 && p->shutdown) {
            pthread_mutex_unlock(&p->queue_lock);
            return NULL;
        }

        PollWorkItem item = p->queue[p->queue_head];
        p->queue_head = (p->queue_head + 1) % p->queue_cap;
        p->queue_count--;
        pthread_mutex_unlock(&p->queue_lock);

        if (item.work_fn) item.work_fn(item.user);
        completion_enqueue(p->ctx, item.done_fn, item.user);
    }
}

static int poll_pool_create(HlAsyncBackendPool **out, HlAsyncBackendCtx *ctx,
                              int num_workers, int queue_capacity)
{
    if (!out || !ctx || num_workers <= 0 || queue_capacity <= 0) return -1;

    HlAsyncBackendPool *p = calloc(1, sizeof *p);
    if (!p) return -1;
    p->ctx          = ctx;
    p->queue_cap    = (size_t)queue_capacity;
    p->num_workers  = num_workers;

    p->queue   = calloc(p->queue_cap, sizeof *p->queue);
    p->workers = calloc((size_t)num_workers, sizeof *p->workers);
    if (!p->queue || !p->workers) goto fail;

    if (pthread_mutex_init(&p->queue_lock, NULL) != 0) goto fail;
    if (pthread_cond_init(&p->queue_cv, NULL) != 0) {
        pthread_mutex_destroy(&p->queue_lock);
        goto fail;
    }

    /* Spawn workers. If any fails, signal shutdown + join the ones
     * that came up. */
    int started = 0;
    for (; started < num_workers; started++) {
        if (pthread_create(&p->workers[started], NULL, worker_main, p) != 0) {
            pthread_mutex_lock(&p->queue_lock);
            p->shutdown = 1;
            pthread_cond_broadcast(&p->queue_cv);
            pthread_mutex_unlock(&p->queue_lock);
            for (int j = 0; j < started; j++) pthread_join(p->workers[j], NULL);
            pthread_cond_destroy(&p->queue_cv);
            pthread_mutex_destroy(&p->queue_lock);
            goto fail;
        }
    }

    *out = p;
    return 0;

fail:
    free(p->queue);
    free(p->workers);
    free(p);
    return -1;
}

static void poll_pool_free(HlAsyncBackendPool *p)
{
    if (!p) return;

    /* Signal shutdown + drain any pending items via cancel_fn. */
    pthread_mutex_lock(&p->queue_lock);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->queue_cv);

    /* Snapshot pending items so we can call cancel_fn outside the lock
     * (the callbacks may take their own locks). */
    size_t pending_n = p->queue_count;
    PollWorkItem *pending = pending_n ? calloc(pending_n, sizeof *pending) : NULL;
    for (size_t i = 0; i < pending_n; i++) {
        pending[i] = p->queue[(p->queue_head + i) % p->queue_cap];
    }
    p->queue_count = 0;
    pthread_mutex_unlock(&p->queue_lock);

    /* Wait for workers to drain in-flight + see shutdown. */
    for (int i = 0; i < p->num_workers; i++)
        pthread_join(p->workers[i], NULL);

    /* Fire cancel_fn for items that never ran. Done inline on the
     * calling thread — Keel's pool does the same. */
    for (size_t i = 0; i < pending_n; i++) {
        if (pending[i].cancel_fn) pending[i].cancel_fn(pending[i].user);
    }
    free(pending);

    pthread_cond_destroy(&p->queue_cv);
    pthread_mutex_destroy(&p->queue_lock);
    free(p->queue);
    free(p->workers);
    free(p);
}

static int poll_pool_submit(HlAsyncBackendPool *p,
                              HlAsyncWorkFn work_fn,
                              HlAsyncWorkFn done_fn,
                              HlAsyncWorkFn cancel_fn,
                              void *user)
{
    if (!p) return -1;
    pthread_mutex_lock(&p->queue_lock);
    if (p->shutdown || p->queue_count == p->queue_cap) {
        pthread_mutex_unlock(&p->queue_lock);
        return -1;
    }
    p->queue[p->queue_tail] = (PollWorkItem){
        .work_fn = work_fn, .done_fn = done_fn, .cancel_fn = cancel_fn,
        .user = user,
    };
    p->queue_tail = (p->queue_tail + 1) % p->queue_cap;
    p->queue_count++;
    pthread_cond_signal(&p->queue_cv);
    pthread_mutex_unlock(&p->queue_lock);
    return 0;
}

/* ── Async-op suspension ───────────────────────────────────────────── */

/*
 * The op state — including the live deadline-timer handle — is owned
 * exclusively by the event-loop thread. Two threads need to interact
 * with it:
 *
 *   - poll_op_deadline_timer fires from the timer system (event-loop
 *     thread → safe).
 *
 *   - poll_op_complete may be called from ANY thread per the vtable
 *     contract. It marshals onto the event-loop thread via the
 *     completion queue, so the actual state mutation
 *     (poll_op_complete_eventloop) runs single-threaded next to the
 *     deadline timer.
 *
 * That marshal removes every race: there are no atomics, no locks,
 * no `resumed` flag, no fragile coordination — the only state-mutating
 * code paths both run on the same thread, in the same tick. The
 * completion queue is drained before timers in poll_tick, so when both
 * fire in the same tick the resume wins (matching keel-backend
 * semantics where op_complete cancels the deadline timer).
 *
 * Tradeoff vs the keel backend: op_complete has one tick of latency
 * (gets queued, fires on next tick). The keel backend does the same
 * thing via a 0ms timer, so this is consistent.
 */

typedef struct PollOpState {
    HlAsyncBackendCtx *ctx;
    uint64_t           deadline_timer;  /* 0 = no timer scheduled */
} PollOpState;

static void poll_op_deadline_timer(void *ud)
{
    HlAsyncOp *op = ud;
    PollOpState *s = op->_backend_state;
    if (!s) return;                     /* op_complete already cleaned up */
    if (op->on_deadline) op->on_deadline(op);
    free(s);
    op->_backend_state = NULL;
}

static void poll_op_complete_eventloop(void *ud)
{
    HlAsyncOp *op = ud;
    PollOpState *s = op->_backend_state;
    if (!s) return;                     /* deadline already fired + cleaned up */
    if (s->deadline_timer) poll_timer_cancel(s->ctx, s->deadline_timer);
    if (op->on_resume) op->on_resume(op);
    free(s);
    op->_backend_state = NULL;
}

static int poll_op_suspend(HlAsyncBackendCtx *ctx, HlAsyncOp *op)
{
    if (!ctx || !op) return -1;
    PollOpState *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->ctx = ctx;
    op->_backend_state = s;

    if (op->deadline_ms > 0 && op->on_deadline) {
        uint64_t now = poll_monotonic_ms();
        uint64_t delay = (op->deadline_ms > now) ? op->deadline_ms - now : 0;
        uint64_t h = poll_timer_add(ctx, delay, poll_op_deadline_timer, op);
        if (h == 0) {
            free(s);
            op->_backend_state = NULL;
            return -1;
        }
        s->deadline_timer = h;
    }
    return 0;
}

static void poll_op_complete(HlAsyncBackendCtx *ctx, HlAsyncOp *op)
{
    if (!ctx || !op) return;
    /* Marshal to the event-loop thread; the actual mutation runs
     * single-threaded in poll_op_complete_eventloop. */
    completion_enqueue(ctx, poll_op_complete_eventloop, op);
}

/* ── Exported vtable ───────────────────────────────────────────────── */

const HlAsyncBackend hl_async_backend_poll = {
    .name             = "poll",
    .init             = poll_init,
    .free             = poll_free,
    .tick             = poll_tick,
    .run              = poll_run,
    .run_until        = poll_run_until,
    .stop             = poll_stop,
    .monotonic_ms     = poll_monotonic_ms,
    .timer_add        = poll_timer_add,
    .timer_cancel     = poll_timer_cancel,
    .watcher_add      = poll_watcher_add,
    .watcher_mod      = poll_watcher_mod,
    .watcher_del      = poll_watcher_del,
    .pool_create      = poll_pool_create,
    .pool_free        = poll_pool_free,
    .pool_submit      = poll_pool_submit,
    .op_suspend       = poll_op_suspend,
    .op_complete      = poll_op_complete,
};

/* ── Backend selection ─────────────────────────────────────────────── */

/*
 * Lives here (poll.c is always compiled) rather than in keel.c (dropped
 * when neither HTTP half is on). Compile-time pick so the hot-path
 * call (hl_async_backend()->whatever) costs one indirection.
 *
 *   either HTTP half on : keel wins. The same KlEventCtx backs both
 *                          KlServer (HL_ENABLE_HTTP_SERVER) and
 *                          kl_client_* (HL_ENABLE_HTTP_CLIENT).
 *   both halves off     : poll wins. async/keel.c isn't compiled and
 *                          libkeel.a isn't linked.
 *
 * HL_ENABLE_HTTP is the legacy "any HTTP at all" macro — still
 * defined by the Makefile when either granular flag is on, and that's
 * exactly the condition under which keel.c compiles. Keep using it
 * here for symmetry with the build.
 */
#ifdef HL_ENABLE_HTTP
extern const HlAsyncBackend hl_async_backend_keel;
#endif

const HlAsyncBackend *hl_async_backend(void)
{
#ifdef HL_ENABLE_HTTP
    return &hl_async_backend_keel;
#else
    return &hl_async_backend_poll;
#endif
}

/* ── HlNetBackend stubs for HL_ENABLE_HTTP_SERVER=0 builds ──────────
 *
 * The real symbols live in src/hull/net/keel.c, which compiles only
 * when HL_ENABLE_HTTP_SERVER=1 (the only situation where the
 * connection-bound suspend pair is meaningfully invoked). Other
 * builds — CLI, pure compute, or even client-only HTTPS — reach the
 * symbols transitively (worker_db/wasm/gpu.c, cap/http_async.c, the
 * runtime async paths), but the call sites are gated by
 * `if (active_conn)` / `if (!ctx->detached)` checks that never fire
 * without a server. Stubs let the link succeed.
 */
#ifndef HL_ENABLE_HTTP_SERVER
#include "hull/net_backend.h"

int hl_net_op_suspend(HlNetBackendCtx *ctx, HlReqHandle *req, HlSuspendOp *op)
{
    (void)ctx; (void)req; (void)op;
    return -1;       /* CLI mode: no net backend to suspend on */
}

void hl_net_op_complete(HlNetBackendCtx *ctx, HlSuspendOp *op)
{
    (void)ctx; (void)op;
}
#endif
