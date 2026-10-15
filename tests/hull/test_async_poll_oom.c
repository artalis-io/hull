/* Allocation failures must reject new work, never lose accepted completions.
 * Compile the production backend with only its allocator calls replaced.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "utest.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <sched.h>

static atomic_int allocation_budget = -1;
static atomic_int live_allocations;
static atomic_int denied_allocations;

static int allow_allocation(void)
{
    int budget = atomic_load(&allocation_budget);
    while (budget >= 0) {
        if (!budget) {
            atomic_fetch_add(&denied_allocations, 1);
            return 0;
        }
        if (atomic_compare_exchange_weak(&allocation_budget, &budget, budget - 1))
            break;
    }
    return 1;
}
static void *fault_calloc(size_t n, size_t size)
{
    if (!allow_allocation()) return NULL;
    void *p = calloc(n, size);
    if (p) atomic_fetch_add(&live_allocations, 1);
    return p;
}
static void *fault_realloc(void *p, size_t size)
{
    if (!allow_allocation()) return NULL;
    int was_null = p == NULL;
    void *out = realloc(p, size);
    if (out && was_null) atomic_fetch_add(&live_allocations, 1);
    return out;
}
static void fault_free(void *p)
{
    if (p) atomic_fetch_sub(&live_allocations, 1);
    free(p);
}
#define calloc fault_calloc
#define realloc fault_realloc
#define free fault_free
#include "../../src/hull/async/poll.c"
#undef calloc
#undef realloc
#undef free

struct async_poll_oom {
    HlAsyncBackendCtx *ctx;
    HlAsyncBackendPool *pool;
    int resumed, done, cancelled, deadlined;
    atomic_int work_started;
    pthread_t done_thread;
};
UTEST_F_SETUP(async_poll_oom)
{
    atomic_init(&utest_fixture->work_started, 0);
    atomic_store(&allocation_budget, -1);
    atomic_store(&denied_allocations, 0);
    ASSERT_EQ(atomic_load(&live_allocations), 0);
    ASSERT_EQ(poll_init(&utest_fixture->ctx, NULL), 0);
}
UTEST_F_TEARDOWN(async_poll_oom)
{
    atomic_store(&allocation_budget, -1);
    poll_pool_free(utest_fixture->pool);
    poll_free(utest_fixture->ctx);
    ASSERT_EQ(atomic_load(&live_allocations), 0);
}
typedef struct {
    HlAsyncOp op;
    struct async_poll_oom *fixture;
} TestOp;
static void resumed(HlAsyncOp *op)
{
    ((TestOp *)op)->fixture->resumed++;
}
static void done(void *user)
{
    struct async_poll_oom *f = user;
    f->done++;
    f->done_thread = pthread_self();
}
static void cancelled(void *user)
{
    ((struct async_poll_oom *)user)->cancelled++;
}

UTEST_F(async_poll_oom, suspend_rejects_failed_reservation)
{
    /* Fail either the op-state allocation or its completion reservation. */
    for (int budget = 0; budget < 2; budget++) {
        TestOp op = {.op.on_resume = resumed, .fixture = utest_fixture};
        atomic_store(&allocation_budget, budget);
        ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &op.op), -1);
        ASSERT_TRUE(op.op._backend_state == NULL);
        atomic_store(&allocation_budget, -1);
    }
}

UTEST_F(async_poll_oom, complete_needs_no_allocation_and_survives_failed_tick)
{
    TestOp op = {.op.on_resume = resumed, .fixture = utest_fixture};
    ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &op.op), 0);
    atomic_store(&allocation_budget, 0);
    poll_op_complete(utest_fixture->ctx, &op.op);
    poll_op_complete(utest_fixture->ctx, &op.op); /* duplicate is coalesced */
    ASSERT_EQ(atomic_load(&denied_allocations), 0);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), -1);
    ASSERT_EQ(utest_fixture->resumed, 0);
    atomic_store(&allocation_budget, -1);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(utest_fixture->resumed, 1);
    ASSERT_TRUE(op.op._backend_state == NULL);
}

UTEST_F(async_poll_oom, deadline_allocation_failure_releases_reservation)
{
    for (int budget = 2; budget < 4; budget++) {
        TestOp op = {.op.on_deadline = resumed, .fixture = utest_fixture};
        op.op.deadline_ms = poll_monotonic_ms() + 10000;
        atomic_store(&allocation_budget, budget);
        ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &op.op), -1);
        ASSERT_TRUE(op.op._backend_state == NULL);
        atomic_store(&allocation_budget, -1);
    }
}

UTEST_F(async_poll_oom, submit_rejects_failed_reservation_without_callbacks)
{
    ASSERT_EQ(poll_pool_create(&utest_fixture->pool, utest_fixture->ctx, 1, 2), 0);
    atomic_store(&allocation_budget, 0);
    ASSERT_EQ(poll_pool_submit(utest_fixture->pool, cancelled, done, cancelled,
                              utest_fixture), -1);
    poll_pool_free(utest_fixture->pool);
    utest_fixture->pool = NULL;
    ASSERT_EQ(utest_fixture->done, 0);
    ASSERT_EQ(utest_fixture->cancelled, 0);
    ASSERT_EQ(atomic_load(&denied_allocations), 1);
}

static void exhaust_allocations(void *user)
{
    (void)user;
    atomic_store(&allocation_budget, 0);
}
UTEST_F(async_poll_oom, worker_completion_is_delivered_after_memory_exhaustion)
{
    ASSERT_EQ(poll_pool_create(&utest_fixture->pool, utest_fixture->ctx, 1, 2), 0);
    ASSERT_EQ(poll_pool_submit(utest_fixture->pool, exhaust_allocations, done,
                              cancelled, utest_fixture), 0);
    /* Join proves the worker completed its enqueue while allocations failed. */
    pthread_mutex_lock(&utest_fixture->pool->queue_lock);
    while (utest_fixture->pool->queue_count) {
        pthread_mutex_unlock(&utest_fixture->pool->queue_lock);
        sched_yield();
        pthread_mutex_lock(&utest_fixture->pool->queue_lock);
    }
    pthread_mutex_unlock(&utest_fixture->pool->queue_lock);
    /* pool_free delivers it, on this thread, with no allocation (audit 14
     * G1: it used to stay queued for a tick that teardown never ran). */
    poll_pool_free(utest_fixture->pool);
    utest_fixture->pool = NULL;
    ASSERT_EQ(atomic_load(&denied_allocations), 0);
    ASSERT_EQ(utest_fixture->done, 1);
    ASSERT_TRUE(pthread_equal(utest_fixture->done_thread, pthread_self()));
    atomic_store(&allocation_budget, -1);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(utest_fixture->done, 1);
}

static void wait_for_shutdown(void *user)
{
    struct async_poll_oom *f = user;
    atomic_store(&f->work_started, 1);
    for (;;) {
        pthread_mutex_lock(&f->pool->queue_lock);
        int shutdown = f->pool->shutdown;
        pthread_mutex_unlock(&f->pool->queue_lock);
        if (shutdown) return;
        sched_yield();
    }
}
UTEST_F(async_poll_oom, pending_pool_work_is_cancelled_without_allocation)
{
    ASSERT_EQ(poll_pool_create(&utest_fixture->pool, utest_fixture->ctx, 1, 2), 0);
    HlAsyncBackendPool *p = utest_fixture->pool;
    ASSERT_EQ(poll_pool_submit(p, wait_for_shutdown, done, cancelled, utest_fixture), 0);
    while (!atomic_load(&utest_fixture->work_started)) sched_yield();
    ASSERT_EQ(poll_pool_submit(p, NULL, done, cancelled, utest_fixture), 0);
    ASSERT_EQ(poll_pool_submit(p, NULL, done, cancelled, utest_fixture), 0);
    atomic_store(&allocation_budget, 0);
    poll_pool_free(p);
    utest_fixture->pool = NULL;
    ASSERT_EQ(atomic_load(&denied_allocations), 0);
    ASSERT_EQ(utest_fixture->cancelled, 2);
    ASSERT_EQ(utest_fixture->done, 1);   /* the running item, delivered by pool_free */
    atomic_store(&allocation_budget, -1);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(utest_fixture->done, 1);
}

UTEST_F(async_poll_oom, cancel_and_context_free_release_reserved_records)
{
    TestOp op = {.op.on_resume = resumed, .fixture = utest_fixture};
    ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &op.op), 0);
    atomic_store(&allocation_budget, 0);
    poll_op_cancel(utest_fixture->ctx, &op.op); /* unqueued reservation */
    atomic_store(&allocation_budget, -1);
    ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &op.op), 0);
    poll_op_complete(utest_fixture->ctx, &op.op);
    atomic_store(&allocation_budget, 0);
    poll_op_cancel(utest_fixture->ctx, &op.op); /* queued reservation */
    poll_free(utest_fixture->ctx);
    utest_fixture->ctx = NULL;
    ASSERT_EQ(atomic_load(&denied_allocations), 0);
    ASSERT_EQ(utest_fixture->resumed, 0);
}

static struct async_poll_oom *nested_fixture;
static HlAsyncOp *nested_child, *nested_victim;
static int nested_tick_result, nested_suspend_result;
static void cancel_outer_batch(HlAsyncOp *op)
{
    (void)op;
    poll_op_cancel(nested_fixture->ctx, nested_victim);
    nested_suspend_result = poll_op_suspend(nested_fixture->ctx, nested_victim);
}
static void run_nested_tick(HlAsyncOp *op)
{
    (void)op;
    poll_op_complete(nested_fixture->ctx, nested_child);
    nested_tick_result = poll_tick(nested_fixture->ctx, 0);
}
UTEST_F(async_poll_oom, nested_tick_cancel_does_not_resume_a_reused_op)
{
    HlAsyncOp first = {.on_resume = run_nested_tick};
    HlAsyncOp child = {.on_resume = cancel_outer_batch};
    TestOp victim = {.op.on_resume = resumed, .fixture = utest_fixture};
    nested_fixture = utest_fixture;
    nested_child = &child;
    nested_victim = &victim.op;
    ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &first), 0);
    ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &child), 0);
    ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &victim.op), 0);
    poll_op_complete(utest_fixture->ctx, &first);
    poll_op_complete(utest_fixture->ctx, &victim.op);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(nested_tick_result, 0);
    ASSERT_EQ(nested_suspend_result, 0);
    ASSERT_EQ(utest_fixture->resumed, 0);
    poll_op_complete(utest_fixture->ctx, &victim.op);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(utest_fixture->resumed, 1);
}

static int deadline_reuse_result;
static void queue_mid_tick(void *user)
{
    TestOp *op = user;
    poll_op_complete(op->fixture->ctx, &op->op);
}
static void deadline_reuses_op(HlAsyncOp *base)
{
    TestOp *op = (TestOp *)base;
    op->fixture->deadlined++;
    op->op.deadline_ms = 0;
    deadline_reuse_result = poll_op_suspend(op->fixture->ctx, &op->op);
}
UTEST_F(async_poll_oom, deadline_withdraws_mid_tick_completion_before_reuse)
{
    TestOp op = {.op.on_resume = resumed, .op.on_deadline = deadline_reuses_op,
                 .fixture = utest_fixture};
    ASSERT_GT(poll_timer_add(utest_fixture->ctx, 0, queue_mid_tick, &op), (uint64_t)0);
    op.op.deadline_ms = poll_monotonic_ms();
    ASSERT_EQ(poll_op_suspend(utest_fixture->ctx, &op.op), 0);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(utest_fixture->deadlined, 1);
    ASSERT_EQ(deadline_reuse_result, 0);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(utest_fixture->resumed, 0);
    poll_op_complete(utest_fixture->ctx, &op.op);
    ASSERT_EQ(poll_tick(utest_fixture->ctx, 0), 0);
    ASSERT_EQ(utest_fixture->resumed, 1);
}

UTEST_MAIN();
