/*
 * test_smtp_saturation.c - live SMTP admission saturation on the REAL async pool.
 *
 * Drives the ACTUAL poll async backend (src/hull/async/poll.c: real worker threads
 * + real pool_submit / pool_free / tick) through the real admission + submit +
 * in-flight-registry layers - NOT a fake submit seam. The injected transport
 * execute-phase is a controlled slow peer: it blocks post-resolution until the test
 * releases it OR the worker op signals cancel (wop_poll_cancel), exactly as a real
 * held SMTP conversation is interrupted at shutdown.
 *
 * Admission cap = max(1, floor(W/2)) (smtp_admit.c): W=2 -> 1, W=4 -> 2, always
 * leaving >= 1 worker for db / compute. The tests prove SMTP admits exactly the
 * cap, the next request resolves promptly as connect_failed / schedule cap_reached,
 * a non-SMTP job still completes on a free worker while SMTP is saturated, every
 * admission lease returns exactly once, the registry returns to zero, and
 * cancellation/shutdown while saturated neither hangs nor leaks (ASan/TSan).
 *
 * Evidence (worker count, headroom completion bound, peak/final inflight, schedule
 * tag) is written to stderr per test.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/shared/async_backend.h"
#include "hull/cap/smtp_submit.h"
#include "hull/cap/smtp_op.h"
#include "hull/cap/smtp_admit.h"
#include "hull/cap/smtp_inflight.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

extern const HlAsyncBackend hl_async_backend_poll;
#define BK hl_async_backend_poll

static HlAsyncBackendCtx  *g_ctx;
static HlAsyncBackendPool *g_pool;

/* ── controlled slow peer (the injected execute) ─────────────────────── */

static atomic_int g_hold;            /* 1 = hold every admitted worker post-resolution */
static atomic_int g_workers_parked;  /* how many workers are currently held (peak observe) */

static int sat_exec(const HlSmtpMessage *m, int timeout_ms, HlSmtpResult *out,
                    HlSmtpCancelPollFn poll, void *pu, void *eu)
{
    (void)m; (void)timeout_ms; (void)eu;
    atomic_fetch_add(&g_workers_parked, 1);
    /* Held "post-resolution" until released OR the op is cancelled (poll goes
     * true). Bounded by the per-test SIGALRM watchdog. */
    while (atomic_load(&g_hold) && !(poll && poll(pu)))
        usleep(500);
    atomic_fetch_sub(&g_workers_parked, 1);
    int cancelled = poll && poll(pu);
    out->rc              = cancelled ? -1 : 0;
    out->token           = cancelled ? "cancelled" : NULL;
    out->teardown_leaked = 0;
    return out->rc;
}

/* ── seams: REAL pool submit + injected suspend/resume ───────────────── */

static int sat_pool_submit(void *pool, void (*work)(void *), void (*done)(void *),
                           void (*cancel)(void *), void *user)
{
    (void)pool;
    return BK.pool_submit(g_pool, work, done, cancel, user);
}

typedef struct {
    HlSmtpSubmitCtx   *ctx;
    HlSmtpInflightNode node;
    HlSmtpInflight    *reg;
    atomic_int         resume_calls;    /* pool done_fn -> resume invocations */
    atomic_int         teardown_calls;  /* ctx torn down (resume path OR sweep): must == 1 */
    int                idx;
} SatOp;

static int sat_suspend(void *su, HlSmtpSubmitCtx *ctx) { (void)su; (void)ctx; return 0; }

/* Resume (drained by tick on the loop thread): on the normal path, consume the
 * terminal, unlink from the registry, and release the ctx exactly once. Suppressed
 * (like production) when the op was marked non-resumable by a cancel/shutdown. */
static void sat_resume(void *ru)
{
    SatOp *op = (SatOp *)ru;
    atomic_fetch_add(&op->resume_calls, 1);
    if (!op->ctx || !hl_smtp_submit_ctx_resumable(op->ctx))
        return;
    hl_smtp_inflight_remove(&op->node);
    hl_smtp_submit_ctx_release(op->ctx);
    op->ctx = NULL;
    atomic_fetch_add(&op->teardown_calls, 1);
}

/* Registry sweep release (shutdown pass 2, after pool_free): drive the op to its
 * cancel terminal and release the ctx exactly once. */
static void sat_sweep_release(void *owner)
{
    SatOp *op = (SatOp *)owner;
    if (!op->ctx)
        return;
    hl_smtp_submit_ctx_cancel(op->ctx);   /* mark unresumable + request cancel */
    hl_smtp_submit_ctx_release(op->ctx);
    op->ctx = NULL;
    atomic_fetch_add(&op->teardown_calls, 1);
}

/* Shutdown pass 1 (registry-preserving): request cancel of every held op so the
 * worker's execute observes it and aborts promptly. */
static void sat_cancel_each(void *owner, void *user)
{
    (void)user;
    SatOp *op = (SatOp *)owner;
    if (op->ctx)
        hl_smtp_submit_ctx_cancel(op->ctx);
}

static HlSmtpOp *sat_inputs(void)
{
    HlSmtpMessage m; memset(&m, 0, sizeof m);
    m.host = "held.invalid"; m.port = 25;
    m.from = "f@x"; m.to = "t@x"; m.subject = "s"; m.body = "b";
    return hl_smtp_op_create(&m, 5000);
}

/* Submit one SMTP op; on SUSPENDED register it for the sweep. Returns the outcome
 * disposition; fills *sched. */
static int sat_submit(HlSmtpAdmission *adm, HlSmtpInflight *reg, SatOp *op,
                      HlSmtpSchedule *sched)
{
    op->reg = reg;
    atomic_store(&op->resume_calls, 0);
    atomic_store(&op->teardown_calls, 0);
    HlSmtpSubmitReq req = {
        .inputs = sat_inputs(), .admission = adm,
        .execute = sat_exec, .exec_user = NULL,
        .pool = (void *)1, .pool_submit = sat_pool_submit,
        .suspend = sat_suspend, .suspend_user = op,
        .resume = sat_resume, .resume_user = op,
    };
    HlSmtpSubmitOutcome out;
    hl_smtp_submit(&req, &out);
    *sched = out.schedule;
    if (out.disposition == HL_SMTP_SUBMIT_SUSPENDED) {
        op->ctx = out.ctx;
        hl_smtp_inflight_add(reg, &op->node, op, sat_sweep_release);
    } else {
        op->ctx = out.ctx;   /* NULL on cap_reached */
    }
    return out.disposition;
}

/* ── headroom job: a non-SMTP job on the SAME real pool ──────────────── */

static atomic_int g_headroom_ran;
static void headroom_work(void *u)   { (void)u; atomic_store(&g_headroom_ran, 1); }
static void headroom_done(void *u)   { (void)u; }
static void headroom_cancel(void *u) { (void)u; }

/* ── watchdog ────────────────────────────────────────────────────────── */

static void sat_watchdog(int sig)
{
    (void)sig;
    static const char msg[] = "FATAL: saturation watchdog fired (hang)\n";
    ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1); (void)w;
    _exit(70);
}

static uint64_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Drain pool completions (resume/done) until predicate or a bounded number of
 * ticks. Returns 1 if the predicate held. */
static int drain_until_zero(HlSmtpInflight *reg)
{
    for (int i = 0; i < 2000; i++) {          /* <= ~2000 * 5ms = 10s, watchdog-bounded */
        if (hl_smtp_inflight_count(reg) == 0)
            return 1;
        BK.tick(g_ctx, 5);
    }
    return hl_smtp_inflight_count(reg) == 0;
}

/* ── the saturation scenario, parameterized by worker count W ────────── */

static void saturation_scenario(int *utest_result, int W)
{
    const int cap = (W / 2) < 1 ? 1 : (W / 2);   /* mirror max(1, floor(W/2)) */

    void (*prev)(int) = signal(SIGALRM, sat_watchdog); alarm(40);

    ASSERT_EQ(BK.init(&g_ctx, NULL), 0);
    ASSERT_EQ(BK.pool_create(&g_pool, g_ctx, W, 64), 0);

    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, W);
    ASSERT_EQ(hl_smtp_admission_cap(&adm), cap);
    HlSmtpInflight reg; hl_smtp_inflight_init(&reg);

    atomic_store(&g_hold, 1);
    atomic_store(&g_workers_parked, 0);

    SatOp *ops = calloc((size_t)cap, sizeof *ops);
    ASSERT_TRUE(ops != NULL);

    /* Admit exactly `cap` SMTP ops; each holds a worker post-resolution. */
    for (int i = 0; i < cap; i++) {
        HlSmtpSchedule sc;
        ops[i].idx = i;
        ASSERT_EQ(sat_submit(&adm, &reg, &ops[i], &sc), HL_SMTP_SUBMIT_SUSPENDED);
        ASSERT_EQ((int)sc, (int)HL_SMTP_SCHED_NONE);
    }
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), cap);   /* exactly cap admitted */
    ASSERT_EQ(hl_smtp_inflight_count(&reg), cap);

    /* Wait until every admitted worker is genuinely parked in execute. */
    while (atomic_load(&g_workers_parked) < cap) usleep(500);
    int peak_inflight = hl_smtp_admission_inflight(&adm);

    /* The next SMTP request is rejected promptly: connect_failed / cap_reached,
     * WITHOUT entering the pool (no worker consumed). */
    SatOp extra; memset(&extra, 0, sizeof extra);
    HlSmtpSchedule sc_extra;
    ASSERT_EQ(sat_submit(&adm, &reg, &extra, &sc_extra), HL_SMTP_SUBMIT_RESOLVED);
    ASSERT_EQ((int)sc_extra, (int)HL_SMTP_SCHED_CAP_REACHED);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), cap);   /* unchanged: not admitted */
    ASSERT_EQ(hl_smtp_inflight_count(&reg), cap);

    /* While SMTP is saturated, a non-SMTP (db/compute-style) job still runs on a
     * free worker (cap < W leaves headroom) and completes within a bound. */
    atomic_store(&g_headroom_ran, 0);
    uint64_t t0 = now_ms();
    ASSERT_EQ(BK.pool_submit(g_pool, headroom_work, headroom_done, headroom_cancel, NULL), 0);
    while (!atomic_load(&g_headroom_ran) && (now_ms() - t0) < 5000) usleep(500);
    uint64_t headroom_ms = now_ms() - t0;
    ASSERT_EQ(atomic_load(&g_headroom_ran), 1);         /* completed while saturated */
    ASSERT_TRUE(headroom_ms < 5000);

    /* Release the peers: every held worker finishes, publishes its terminal, and
     * releases its admission lease. Drain resume callbacks. */
    atomic_store(&g_hold, 0);
    ASSERT_TRUE(drain_until_zero(&reg));                /* registry returns to zero */
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);     /* every lease returned */
    for (int i = 0; i < cap; i++)
        ASSERT_EQ(atomic_load(&ops[i].teardown_calls), 1);  /* each ctx released exactly once */

    /* Clean teardown. */
    hl_smtp_inflight_for_each(&reg, sat_cancel_each, NULL);
    BK.pool_free(g_pool);
    hl_smtp_inflight_sweep(&reg);                       /* already empty: 0 */
    BK.free(g_ctx);
    free(ops);

    alarm(0); signal(SIGALRM, prev);

    fprintf(stderr,
            "[saturation evidence] W=%d cap=%d peak_inflight=%d final_inflight=%d "
            "headroom_ms=%llu schedule_next=%s\n",
            W, cap, peak_inflight, hl_smtp_admission_inflight(&adm),
            (unsigned long long)headroom_ms, hl_smtp_schedule_str(sc_extra));
}

UTEST(smtp_saturation, w2_admits_exactly_one)
{
    saturation_scenario(utest_result, 2);
}

UTEST(smtp_saturation, w4_admits_exactly_two)
{
    saturation_scenario(utest_result, 4);
}

/* Cancellation / shutdown WHILE saturated: the workers are held, then the two-pass
 * shutdown runs (request-cancel-all -> pool_free -> sweep). The cancel interrupts
 * the held workers (execute observes wop_poll_cancel), so pool_free joins without
 * hanging; every lease returns and every ctx is released exactly once, with no
 * duplicate teardown and no runtime-ref leak (ASan/TSan). */
UTEST(smtp_saturation, shutdown_while_saturated_no_hang_no_leak)
{
    const int W = 4, cap = 2;

    void (*prev)(int) = signal(SIGALRM, sat_watchdog); alarm(40);

    ASSERT_EQ(BK.init(&g_ctx, NULL), 0);
    ASSERT_EQ(BK.pool_create(&g_pool, g_ctx, W, 64), 0);

    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, W);
    HlSmtpInflight reg; hl_smtp_inflight_init(&reg);
    atomic_store(&g_hold, 1);
    atomic_store(&g_workers_parked, 0);

    SatOp ops[2];
    for (int i = 0; i < cap; i++) {
        memset(&ops[i], 0, sizeof ops[i]);
        HlSmtpSchedule sc; ops[i].idx = i;
        ASSERT_EQ(sat_submit(&adm, &reg, &ops[i], &sc), HL_SMTP_SUBMIT_SUSPENDED);
    }
    while (atomic_load(&g_workers_parked) < cap) usleep(500);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), cap);

    /* Two-pass shutdown while saturated (the workers are still held). */
    hl_smtp_inflight_for_each(&reg, sat_cancel_each, NULL);  /* pass 1: request cancel */
    BK.pool_free(g_pool);                                    /* joins the (now-aborting) workers */
    int swept = hl_smtp_inflight_sweep(&reg);                /* pass 2: release each once */
    BK.free(g_ctx);

    alarm(0); signal(SIGALRM, prev);

    ASSERT_EQ(swept, cap);                          /* every registered op swept */
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0); /* no leaked capacity */
    ASSERT_EQ(hl_smtp_inflight_count(&reg), 0);
    for (int i = 0; i < cap; i++)
        ASSERT_EQ(atomic_load(&ops[i].teardown_calls), 1);   /* released exactly once */

    fprintf(stderr,
            "[saturation evidence] shutdown-while-saturated W=%d cap=%d swept=%d "
            "final_inflight=%d\n", W, cap, swept, hl_smtp_admission_inflight(&adm));
}

UTEST_MAIN()
