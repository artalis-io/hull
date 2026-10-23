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
#include <stdatomic.h>
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

typedef struct {
    Fixture *fixture;
    atomic_int finished;
    int failed;
    int fired; /* Only the event-loop thread touches this field. */
} TimerRace;

static void count_race_timer(void *user)
{
    TimerRace *race = user;
    race->fired++;
}

static void *submit_race_timers(void *user)
{
    TimerRace *race = user;
    uint64_t previous = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t id = race->fixture->be->timer_add(race->fixture->ctx, 0,
                                                  count_race_timer, race);
        if (id == 0 || id <= previous) race->failed = 1;
        previous = id;
    }
    atomic_store(&race->finished, 1);
    return NULL;
}

/* ASan/TSan exercise a timer being freed while its submitter returns. */
UTEST(async_backend_poll, cross_thread_zero_delay_timers)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);
    TimerRace race = {.fixture = &f};
    atomic_init(&race.finished, 0);
    pthread_t submitter;
    ASSERT_EQ(pthread_create(&submitter, NULL, submit_race_timers, &race), 0);
    while (!atomic_load(&race.finished)) f.be->tick(f.ctx, 1);
    ASSERT_EQ(pthread_join(submitter, NULL), 0);
    f.be->tick(f.ctx, 0);
    ASSERT_EQ(race.failed, 0);
    ASSERT_EQ(race.fired, 10000);
    fixture_free(&f);
}

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

/* ── A 0 ms timer re-added by its own callback waits a tick ─────────
 * Audit 24: step 5 fired every timer due at the time it looked, so a 0 ms
 * timer re-added from its callback (a self-re-spawning task, two tasks
 * waking each other) fired again in the same tick without end - the tick
 * never returned to completions, watchers or run_until's stop check. The
 * cap keeps the old behaviour finite so the test fails instead of hanging. */

typedef struct {
    Fixture *f;
    int      fired;
} Respawn;

static void respawn_timer(void *u)
{
    Respawn *r = u;
    if (++r->fired < 100000) r->f->be->timer_add(r->f->ctx, 0, respawn_timer, r);
}

static int respawn_stop_calls;
static int respawn_stop(void *u)
{
    Respawn *r = u;
    respawn_stop_calls++;
    return r->fired >= 3;
}

UTEST(async_backend_poll, zero_ms_timer_readded_by_callback_waits_a_tick)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);
    Respawn r = {.f = &f};
    ASSERT_GT(f.be->timer_add(f.ctx, 0, respawn_timer, &r), (uint64_t)0);
    f.be->tick(f.ctx, 0);
    ASSERT_EQ(r.fired, 1);
    f.be->tick(f.ctx, 0);
    ASSERT_EQ(r.fired, 2);

    /* run_until's stop check runs between ticks, so it sees the respawner. */
    respawn_stop_calls = 0;
    ASSERT_EQ(f.be->run_until(f.ctx, respawn_stop, &r), 0);
    ASSERT_EQ(r.fired, 3);
    ASSERT_GT(respawn_stop_calls, 0);
    fixture_free(&f);
}

/* Older due timers still fire in the tick a callback adds a new one in,
 * ahead of it even at an equal deadline (the heap breaks ties by id). */
static Fixture *tie_fixture;
static void tie_add_new(void *u)
{
    log_timer(u);
    tie_fixture->be->timer_add(tie_fixture->ctx, 0, log_timer, (void *)(intptr_t)9);
}

UTEST(async_backend_poll, older_due_timers_fire_before_a_new_one)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);
    tie_fixture = &f;
    order_n = 0;
    ASSERT_GT(f.be->timer_add(f.ctx, 0, tie_add_new, (void *)(intptr_t)1), (uint64_t)0);
    ASSERT_GT(f.be->timer_add(f.ctx, 0, log_timer, (void *)(intptr_t)2), (uint64_t)0);
    ASSERT_GT(f.be->timer_add(f.ctx, 0, log_timer, (void *)(intptr_t)3), (uint64_t)0);
    f.be->tick(f.ctx, 0);
    ASSERT_EQ(order_n, 3);
    ASSERT_EQ(order_log[0], 1);
    ASSERT_EQ(order_log[1], 2);
    ASSERT_EQ(order_log[2], 3);
    f.be->tick(f.ctx, 0);
    ASSERT_EQ(order_n, 4);
    ASSERT_EQ(order_log[3], 9);
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

/* ── op_cancel retracts a queued completion ──────────────────────────── */

/*
 * op_complete DEFERS: it queues the resume for the next tick. An owner that
 * frees the storage the op lives in between those two points left the backend
 * holding a pointer into freed memory and had no way to withdraw it. op_cancel
 * is that way; this asserts it actually drops the queued entry.
 */
static int cancelled_op_resumed;
static void on_resume_should_not_fire(HlAsyncOp *op)
{
    (void)op;
    cancelled_op_resumed = 1;
}

UTEST(async_backend_poll, op_cancel_drops_an_already_queued_completion)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);
    ASSERT_TRUE(f.be->op_cancel != NULL);

    cancelled_op_resumed = 0;
    HlAsyncOp op = { .on_resume = on_resume_should_not_fire };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &op), 0);

    f.be->op_complete(f.ctx, &op);   /* queued, not yet run */
    f.be->op_cancel(f.ctx, &op);     /* withdrawn before the tick */

    for (int i = 0; i < 5; i++) f.be->tick(f.ctx, 5);
    ASSERT_EQ(cancelled_op_resumed, 0);

    /* Idempotent, and safe on an op that was never suspended. */
    f.be->op_cancel(f.ctx, &op);
    HlAsyncOp never = { .on_resume = on_resume_should_not_fire };
    f.be->op_cancel(f.ctx, &never);

    fixture_free(&f);
}

/* A completion can cancel an op whose resume sits later in the SAME batch -
 * one tick() has already detached from the queue. op_cancel searched only
 * the queue, so the later entry still ran against the op's storage: freed,
 * or (as here) reused for a new wait, which it then resumed spuriously. */
static Fixture   *g_batch_fix;
static HlAsyncOp *g_batch_victim;
static void on_resume_cancels_and_reuses_victim(HlAsyncOp *op)
{
    (void)op;
    g_batch_fix->be->op_cancel(g_batch_fix->ctx, g_batch_victim);
    /* The owner reuses the storage for a new, still pending wait. */
    g_batch_victim->on_resume = on_resume_should_not_fire;
    (void)g_batch_fix->be->op_suspend(g_batch_fix->ctx, g_batch_victim);
}

UTEST(async_backend_poll, op_cancel_reaches_the_batch_being_drained)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    cancelled_op_resumed = 0;
    HlAsyncOp first  = { .on_resume = on_resume_cancels_and_reuses_victim };
    HlAsyncOp victim = { .on_resume = on_resume_should_not_fire };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &first), 0);
    ASSERT_EQ(f.be->op_suspend(f.ctx, &victim), 0);
    g_batch_fix = &f;
    g_batch_victim = &victim;

    f.be->op_complete(f.ctx, &first);
    f.be->op_complete(f.ctx, &victim);   /* same batch, after `first` */
    for (int i = 0; i < 5; i++) f.be->tick(f.ctx, 5);
    ASSERT_EQ(cancelled_op_resumed, 0);

    f.be->op_cancel(f.ctx, &victim);      /* the reused wait */
    fixture_free(&f);
}

/* The retraction must be specific: cancelling one op must not silently eat
 * another op's pending resume. */
static int other_op_resumed;
static void on_resume_other(HlAsyncOp *op) { (void)op; other_op_resumed = 1; }

UTEST(async_backend_poll, op_cancel_does_not_drop_a_different_ops_completion)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    cancelled_op_resumed = 0;
    other_op_resumed = 0;
    HlAsyncOp a = { .on_resume = on_resume_should_not_fire };
    HlAsyncOp b = { .on_resume = on_resume_other };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &a), 0);
    ASSERT_EQ(f.be->op_suspend(f.ctx, &b), 0);

    f.be->op_complete(f.ctx, &a);
    f.be->op_complete(f.ctx, &b);
    f.be->op_cancel(f.ctx, &a);

    uint64_t start = f.be->monotonic_ms();
    while (!other_op_resumed && f.be->monotonic_ms() - start < 200)
        f.be->tick(f.ctx, 20);
    ASSERT_EQ(other_op_resumed, 1);
    ASSERT_EQ(cancelled_op_resumed, 0);

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

/* ── Pool: a finished item's done_fn still runs at pool_free ─────────
 *
 * Audit 14 G1: work that had completed but whose done_fn the loop had not
 * run yet sat in the completion queue, and poll_free dropped it without a
 * done_fn - an unawaited db.async in app.main leaked its continuation and
 * QuickJS aborted in JS_FreeRuntime. pool_free now delivers it, as Keel's
 * pool does. */
static atomic_int finished_work;
static int late_done_count;
static void finishing_work(void *u) { (void)u; atomic_fetch_add(&finished_work, 1); }
static void late_done(void *u) { (void)u; late_done_count++; }

UTEST(async_backend_poll, pool_free_delivers_finished_done)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    HlAsyncBackendPool *pool = NULL;
    ASSERT_EQ(f.be->pool_create(&pool, f.ctx, 1, 4), 0);

    atomic_store(&finished_work, 0);
    late_done_count = 0;
    ASSERT_EQ(f.be->pool_submit(pool, finishing_work, late_done, NULL, NULL), 0);

    /* The work runs; the loop is never ticked, so its done_fn stays queued. */
    uint64_t start = f.be->monotonic_ms();
    while (atomic_load(&finished_work) == 0 && f.be->monotonic_ms() - start < 2000) {}
    ASSERT_EQ(atomic_load(&finished_work), 1);
    EXPECT_EQ(late_done_count, 0);

    f.be->pool_free(pool);
    EXPECT_EQ(late_done_count, 1);

    /* Nothing of the pool's is left for a later tick. */
    EXPECT_EQ(f.be->tick(f.ctx, 0), 0);
    EXPECT_EQ(late_done_count, 1);
    fixture_free(&f);
}

UTEST_MAIN();
