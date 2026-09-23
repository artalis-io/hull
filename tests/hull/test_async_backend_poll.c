/*
 * test_async_backend_poll.c - tests for the poll(2)/pthread-backed
 * HlAsyncBackend impl. Same shape as test_async_backend.c (which
 * exercises whichever backend the runtime selects via
 * hl_async_backend()), but pins the backend to `hl_async_backend_poll`
 * by name so it runs even on HTTP=1 builds where the runtime
 * selector returns the keel backend.
 *
 * Adds two pool-specific tests that the keel suite doesn't have -
 * those couldn't run there because keel's thread pool uses Keel's
 * KlHttpServer-rooted pipe for completion delivery and the test fixture
 * doesn't wire one. The poll backend is self-contained.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/async_backend.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The poll backend symbol - defined in src/hull/async/poll.c, never
 * returned by hl_async_backend() on HTTP=1 builds. */
extern const HlAsyncBackend hl_async_backend_poll;

/* ── Fixture ────────────────────────────────────────────────────────── */

typedef struct {
    const HlAsyncBackend *be;
    HlAsyncBackendCtx    *ctx;
} Fixture;

static int fixture_init(Fixture *f)
{
    f->be = &hl_async_backend_poll;
    return f->be->init(&f->ctx, NULL);
}

static void fixture_free(Fixture *f)
{
    if (f->be && f->ctx) f->be->free(f->ctx);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

UTEST(async_backend_poll, name_is_poll)
{
    ASSERT_STREQ(hl_async_backend_poll.name, "poll");
}

UTEST(async_backend_poll, init_and_free)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);
    /* HlAsyncBackendCtx is opaque; cast to void* so utest.h's
     * __typeof__(x + 0) doesn't do pointer arithmetic on an
     * incomplete type (GCC -Werror). */
    ASSERT_NE((void *)f.ctx, (void *)NULL);
    fixture_free(&f);
}

/* ── Time ──────────────────────────────────────────────────────────── */

UTEST(async_backend_poll, monotonic_ms_is_monotonic)
{
    uint64_t a = hl_async_backend_poll.monotonic_ms();
    for (volatile int i = 0; i < 100000; i++) {}
    uint64_t b = hl_async_backend_poll.monotonic_ms();
    ASSERT_TRUE(b >= a);
}

/* ── Timers ────────────────────────────────────────────────────────── */

static int timer_fired;
static void on_timer(void *u) { (void)u; timer_fired = 1; }

UTEST(async_backend_poll, timer_fires)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    timer_fired = 0;
    uint64_t h = f.be->timer_add(f.ctx, 10, on_timer, NULL);
    ASSERT_GT(h, (uint64_t)0);

    uint64_t start = f.be->monotonic_ms();
    while (!timer_fired && f.be->monotonic_ms() - start < 500)
        f.be->tick(f.ctx, 20);
    ASSERT_EQ(timer_fired, 1);

    fixture_free(&f);
}

UTEST(async_backend_poll, timer_cancel_prevents_fire)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    timer_fired = 0;
    uint64_t h = f.be->timer_add(f.ctx, 50, on_timer, NULL);
    ASSERT_GT(h, (uint64_t)0);
    f.be->timer_cancel(f.ctx, h);

    uint64_t start = f.be->monotonic_ms();
    while (f.be->monotonic_ms() - start < 120)
        f.be->tick(f.ctx, 20);
    ASSERT_EQ(timer_fired, 0);

    fixture_free(&f);
}

/* ── Multiple timers fire in deadline order ────────────────────────── */

static int order_log[8];
static int order_n;
static void log_timer(void *u) { order_log[order_n++] = (int)(intptr_t)u; }

UTEST(async_backend_poll, timers_fire_in_deadline_order)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    order_n = 0;
    /* Insert in reverse-deadline order; expect ascending fire order. */
    ASSERT_GT(f.be->timer_add(f.ctx, 60, log_timer, (void *)(intptr_t)3), (uint64_t)0);
    ASSERT_GT(f.be->timer_add(f.ctx, 20, log_timer, (void *)(intptr_t)1), (uint64_t)0);
    ASSERT_GT(f.be->timer_add(f.ctx, 40, log_timer, (void *)(intptr_t)2), (uint64_t)0);

    uint64_t start = f.be->monotonic_ms();
    while (order_n < 3 && f.be->monotonic_ms() - start < 500)
        f.be->tick(f.ctx, 20);

    ASSERT_EQ(order_n, 3);
    ASSERT_EQ(order_log[0], 1);
    ASSERT_EQ(order_log[1], 2);
    ASSERT_EQ(order_log[2], 3);

    fixture_free(&f);
}

/* ── Async op suspend / complete ───────────────────────────────────── */

static int op_resumed;
static int op_deadlined;
static void on_resume(HlAsyncOp *op)   { (void)op; op_resumed = 1; }
static void on_deadline(HlAsyncOp *op) { (void)op; op_deadlined = 1; }

UTEST(async_backend_poll, op_complete_fires_resume)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    op_resumed = 0; op_deadlined = 0;
    HlAsyncOp op = { .on_resume = on_resume, .on_deadline = on_deadline };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &op), 0);

    f.be->op_complete(f.ctx, &op);

    uint64_t start = f.be->monotonic_ms();
    while (!op_resumed && f.be->monotonic_ms() - start < 200)
        f.be->tick(f.ctx, 20);
    ASSERT_EQ(op_resumed, 1);
    ASSERT_EQ(op_deadlined, 0);

    fixture_free(&f);
}

UTEST(async_backend_poll, op_deadline_fires_when_not_completed)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    op_resumed = 0; op_deadlined = 0;
    HlAsyncOp op = {
        .deadline_ms = f.be->monotonic_ms() + 20,
        .on_resume   = on_resume,
        .on_deadline = on_deadline,
    };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &op), 0);

    uint64_t start = f.be->monotonic_ms();
    while (!op_deadlined && f.be->monotonic_ms() - start < 200)
        f.be->tick(f.ctx, 20);
    ASSERT_EQ(op_deadlined, 1);
    ASSERT_EQ(op_resumed, 0);

    fixture_free(&f);
}

/* ── Dispatch liveness: a watcher deregistered mid-tick is not fired ── */

/*
 * tick() snapshots the watcher table before poll(), then runs completions and
 * timers - which run APPLICATION code - and only then dispatches the ready
 * descriptors from that snapshot. The snapshot carries `user`, so if the
 * application closed the connection in between, dispatching the stale entry
 * hands a callback a pointer to freed memory.
 *
 * The object here is heap-allocated and really freed so ASan reports the
 * use-after-free rather than the test having to guess at it, and the pipe is
 * written to first so the descriptor is genuinely ready in the same tick.
 */
typedef struct { int alive; int fd; } DispatchVictim;

static DispatchVictim *victim;
static const HlAsyncBackend *victim_be;
static HlAsyncBackendCtx    *victim_ctx;
static int victim_cb_fired;

static void victim_watch_cb(int fd, unsigned ready, void *user)
{
    (void)fd; (void)ready;
    DispatchVictim *v = user;     /* freed by the completion below */
    victim_cb_fired = 1;
    if (v) v->alive = 0;          /* the use-after-free, if we get here */
}

/* Runs in step 4, before the step 6 dispatch, exactly as a resumed coroutine
 * calling conn:close() would. */
static void victim_closer(HlAsyncOp *op)
{
    (void)op;
    victim_be->watcher_del(victim_ctx, victim->fd);
    free(victim);
    victim = NULL;
}

UTEST(async_backend_poll, watcher_freed_by_a_completion_is_not_dispatched)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    int p[2];
    ASSERT_EQ(pipe(p), 0);

    victim = malloc(sizeof *victim);
    ASSERT_TRUE(victim != NULL);
    victim->alive = 1;
    victim->fd    = p[0];
    victim_be     = f.be;
    victim_ctx    = f.ctx;
    victim_cb_fired = 0;

    ASSERT_EQ(f.be->watcher_add(f.ctx, p[0], HL_ASYNC_READ,
                                victim_watch_cb, victim), 0);

    /* Make the descriptor ready, so it IS in the snapshot as readable. */
    ASSERT_EQ((int)write(p[1], "x", 1), 1);

    /* And arm a completion, which runs before the dispatch and closes it. */
    HlAsyncOp op = { .on_resume = victim_closer };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &op), 0);
    f.be->op_complete(f.ctx, &op);

    uint64_t start = f.be->monotonic_ms();
    while (victim && f.be->monotonic_ms() - start < 200)
        f.be->tick(f.ctx, 20);

    ASSERT_TRUE(victim == NULL);           /* the completion did run */
    ASSERT_EQ(victim_cb_fired, 0);         /* and the stale watcher did not */

    close(p[0]);
    close(p[1]);
    fixture_free(&f);
}

/* A watcher that is NOT touched must still be dispatched - otherwise the fix
 * above could be "never dispatch anything" and both halves would pass. */
static int live_cb_fired;
static void live_watch_cb(int fd, unsigned ready, void *user)
{
    (void)fd; (void)ready; (void)user;
    live_cb_fired = 1;
}

UTEST(async_backend_poll, an_untouched_watcher_is_still_dispatched)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    int p[2];
    ASSERT_EQ(pipe(p), 0);
    live_cb_fired = 0;

    ASSERT_EQ(f.be->watcher_add(f.ctx, p[0], HL_ASYNC_READ,
                                live_watch_cb, NULL), 0);
    ASSERT_EQ((int)write(p[1], "x", 1), 1);

    uint64_t start = f.be->monotonic_ms();
    while (!live_cb_fired && f.be->monotonic_ms() - start < 200)
        f.be->tick(f.ctx, 20);
    ASSERT_EQ(live_cb_fired, 1);

    f.be->watcher_del(f.ctx, p[0]);
    close(p[0]);
    close(p[1]);
    fixture_free(&f);
}

/* ── Pool: work fires + done lands on event loop ───────────────────── */

static int pool_work_count;
static int pool_done_count;
static pthread_t pool_done_tid;     /* TID where done_fn fired */
static void pool_work(void *u) { (void)u; pool_work_count++; }
static void pool_done(void *u) { (void)u; pool_done_tid = pthread_self(); pool_done_count++; }

UTEST(async_backend_poll, pool_runs_work_and_done)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    HlAsyncBackendPool *pool = NULL;
    ASSERT_EQ(f.be->pool_create(&pool, f.ctx, 2, 4), 0);
    ASSERT_NE((void *)pool, (void *)NULL);

    pool_work_count = 0;
    pool_done_count = 0;
    ASSERT_EQ(f.be->pool_submit(pool, pool_work, pool_done, NULL, NULL), 0);

    pthread_t main_tid = pthread_self();

    uint64_t start = f.be->monotonic_ms();
    while (pool_done_count == 0 && f.be->monotonic_ms() - start < 500)
        f.be->tick(f.ctx, 20);

    ASSERT_EQ(pool_work_count, 1);
    ASSERT_EQ(pool_done_count, 1);
    /* done_fn must fire on the event-loop (main) thread, not the worker. */
    ASSERT_TRUE(pthread_equal(pool_done_tid, main_tid));

    f.be->pool_free(pool);
    fixture_free(&f);
}

/* ── Pool: pending items get cancel_fn on pool_free ────────────────── */

static int cancel_count;
static void cancel_cb(void *u) { (void)u; cancel_count++; }

/* work_fn that blocks just long enough that the queue still has items
 * when we tear the pool down. Uses a tiny sleep loop rather than
 * synchronization primitives to keep the test self-contained. */
static int block_until;
static void slow_work(void *u) {
    (void)u;
    /* Spin briefly so subsequent submits stay queued. */
    uint64_t end = hl_async_backend_poll.monotonic_ms() + (uint64_t)block_until;
    while (hl_async_backend_poll.monotonic_ms() < end) {}
}

UTEST(async_backend_poll, pool_free_cancels_pending)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    HlAsyncBackendPool *pool = NULL;
    /* 1 worker so submissions queue up behind the first one. */
    ASSERT_EQ(f.be->pool_create(&pool, f.ctx, 1, 8), 0);

    cancel_count = 0;
    block_until = 80;
    /* First item blocks the worker; next two queue up. */
    ASSERT_EQ(f.be->pool_submit(pool, slow_work, NULL, cancel_cb, NULL), 0);
    ASSERT_EQ(f.be->pool_submit(pool, slow_work, NULL, cancel_cb, NULL), 0);
    ASSERT_EQ(f.be->pool_submit(pool, slow_work, NULL, cancel_cb, NULL), 0);

    /* Tear down before the queue drains - pending items should hit
     * cancel_cb. (Worker will finish its in-flight item then exit.) */
    f.be->pool_free(pool);

    /* At least one of the queued items should have been cancelled
     * (depending on timing, all 3 might be - guarantee is "not 0"). */
    ASSERT_TRUE(cancel_count >= 1);

    fixture_free(&f);
}

UTEST_MAIN();
