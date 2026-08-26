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
 * KlServer-rooted pipe for completion delivery and the test fixture
 * doesn't wire one. The poll backend is self-contained.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/async_backend.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

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
