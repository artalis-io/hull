/*
 * test_async_backend.c — End-to-end tests for the Keel-backed
 * HlAsyncBackend impl. Exercises init/free, timers, monotonic time,
 * op_suspend/op_complete, and the run/stop loop.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/async_backend.h"

#include <string.h>

/* ── Fixture ────────────────────────────────────────────────────────── */

typedef struct {
    const HlAsyncBackend *be;
    HlAsyncBackendCtx    *ctx;
} Fixture;

static int fixture_init(Fixture *f)
{
    f->be = hl_async_backend();
    if (!f->be) return -1;
    return f->be->init(&f->ctx, NULL);
}

static void fixture_free(Fixture *f)
{
    if (f->be && f->ctx) f->be->free(f->ctx);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

UTEST(async_backend, getter_returns_non_null)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_NE(be, NULL);
    ASSERT_NE(be->name, NULL);
}

UTEST(async_backend, init_and_free)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);
    /* HlAsyncBackendCtx is opaque (forward-declared); cast to void* so
     * utest.h's __typeof__(x + 0) doesn't do pointer arithmetic on an
     * incomplete type (GCC -Werror). */
    ASSERT_NE((void *)f.ctx, (void *)NULL);
    fixture_free(&f);
}

UTEST(async_backend, name_is_keel)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_STREQ(be->name, "keel");
}

/* ── Time ──────────────────────────────────────────────────────────── */

UTEST(async_backend, monotonic_ms_is_monotonic)
{
    const HlAsyncBackend *be = hl_async_backend();
    uint64_t a = be->monotonic_ms();
    /* Tiny busy work to ensure clock advances. */
    for (volatile int i = 0; i < 100000; i++) {}
    uint64_t b = be->monotonic_ms();
    ASSERT_TRUE(b >= a);
}

/* ── Timers ────────────────────────────────────────────────────────── */

static int timer_fired;
static void on_timer(void *u) { (void)u; timer_fired = 1; }

UTEST(async_backend, timer_fires)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    timer_fired = 0;
    uint64_t h = f.be->timer_add(f.ctx, 10, on_timer, NULL);
    ASSERT_GT(h, (uint64_t)0);

    /* Drive the loop with a deadline-based stop predicate. */
    uint64_t start = f.be->monotonic_ms();
    while (!timer_fired && f.be->monotonic_ms() - start < 500) {
        /* run_until polls until the predicate returns 1 or stop is called */
        f.be->tick(f.ctx, 20);
        if (timer_fired) break;
    }
    ASSERT_EQ(timer_fired, 1);

    fixture_free(&f);
}

UTEST(async_backend, timer_cancel_prevents_fire)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    timer_fired = 0;
    uint64_t h = f.be->timer_add(f.ctx, 50, on_timer, NULL);
    ASSERT_GT(h, (uint64_t)0);
    f.be->timer_cancel(f.ctx, h);

    /* Wait twice as long as the original deadline; nothing should fire. */
    uint64_t start = f.be->monotonic_ms();
    while (f.be->monotonic_ms() - start < 120) {
        f.be->tick(f.ctx, 20);
    }
    ASSERT_EQ(timer_fired, 0);

    fixture_free(&f);
}

/* ── Async op suspend / complete ───────────────────────────────────── */

static int op_resumed;
static int op_deadlined;
static void on_resume(HlAsyncOp *op)   { (void)op; op_resumed = 1; }
static void on_deadline(HlAsyncOp *op) { (void)op; op_deadlined = 1; }

UTEST(async_backend, op_complete_fires_resume)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    op_resumed = 0; op_deadlined = 0;
    HlAsyncOp op = { .on_resume = on_resume, .on_deadline = on_deadline };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &op), 0);

    f.be->op_complete(f.ctx, &op);

    uint64_t start = f.be->monotonic_ms();
    while (!op_resumed && f.be->monotonic_ms() - start < 200) {
        f.be->tick(f.ctx, 20);
    }
    ASSERT_EQ(op_resumed, 1);
    ASSERT_EQ(op_deadlined, 0);

    fixture_free(&f);
}

UTEST(async_backend, op_deadline_fires_when_not_completed)
{
    Fixture f;
    ASSERT_EQ(fixture_init(&f), 0);

    op_resumed = 0; op_deadlined = 0;
    uint64_t deadline = f.be->monotonic_ms() + 20;
    HlAsyncOp op = {
        .deadline_ms = deadline,
        .on_resume   = on_resume,
        .on_deadline = on_deadline,
    };
    ASSERT_EQ(f.be->op_suspend(f.ctx, &op), 0);

    uint64_t start = f.be->monotonic_ms();
    while (!op_deadlined && f.be->monotonic_ms() - start < 200) {
        f.be->tick(f.ctx, 20);
    }
    ASSERT_EQ(op_deadlined, 1);
    ASSERT_EQ(op_resumed, 0);

    fixture_free(&f);
}

UTEST_MAIN();
