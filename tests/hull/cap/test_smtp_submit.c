/*
 * test_smtp_submit.c - SMTP submit / ordering layer.
 *
 * Pins the reserve -> submit -> suspend discipline with a FAKE backend (injected
 * pool + suspend + resume seams), no live runtime, pool, or socket. The fake pool
 * can run work_fn synchronously and HOLD the completed done_fn until after a
 * simulated suspension (proving fast-completion-before-suspend is safe), or DROP
 * done_fn entirely (proving the shutdown sweep owns the retained runtime ref).
 *
 * Scenarios (the six the design calls out, plus the immediate-failure branches):
 *   - normal completion (work runs after suspend);
 *   - fast completion-before-suspend (work runs during submit, done held);
 *   - submit failure (queue full);
 *   - suspension-setup failure (submit ok, suspend rejects);
 *   - queued cancel_fn (pool shutdown before work ran);
 *   - dropped done_fn (poll shutdown after work ran);
 *   - pool unavailable / cap reached / ctx-alloc OOM.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include (compiled with -DHL_SMTP_TEST_HOOKS) for the ctx-alloc
 * seam; cap_smtp_submit.o is excluded from this test's link. */
#include "../../../src/hull/cap/smtp_submit.c"

#include "hull/cap/smtp_op.h"
#include "hull/cap/smtp_admit.h"

#include <stdlib.h>
#include <string.h>

/* ── fake pool (seam A) ──────────────────────────────────────────────── */
typedef struct {
    void (*work)(void *);
    void (*done)(void *);
    void (*cancel)(void *);
    void *user;
    int   submit_rc;          /* what pool_submit returns (<0 => queue full) */
    int   run_work_on_submit; /* run work_fn synchronously inside submit */
    int   submits;
} FakePool;
static FakePool g_pool;

static int fake_pool_submit(void *pool, void (*w)(void *), void (*d)(void *),
                            void (*c)(void *), void *u)
{
    (void)pool;
    g_pool.submits++;
    if (g_pool.submit_rc != 0)
        return g_pool.submit_rc;               /* rejected: item never entered pool */
    g_pool.work = w; g_pool.done = d; g_pool.cancel = c; g_pool.user = u;
    if (g_pool.run_work_on_submit)
        w(u);                                  /* fast completion before suspend */
    return 0;
}

/* ── fake suspend / resume (seam B) ──────────────────────────────────── */
static int  g_suspend_rc, g_suspend_calls;
static int  g_resume_calls;
static int fake_suspend(void *su, HlSmtpSubmitCtx *ctx)
{ (void)su; (void)ctx; g_suspend_calls++; return g_suspend_rc; }
static void fake_resume(void *ru) { (void)ru; g_resume_calls++; }

/* ── fake execute-phase (worker thread) ──────────────────────────────── */
static int g_exec_calls;
static int exec_success(const HlSmtpMessage *m, int t, HlSmtpResult *out,
                        HlSmtpCancelPollFn poll, void *pu, void *eu)
{
    (void)m; (void)t; (void)poll; (void)pu; (void)eu;
    g_exec_calls++;
    out->rc = 0; out->token = NULL; out->teardown_leaked = 0;
    return 0;
}

/* ── ctx-alloc seam observation ──────────────────────────────────────── */
static int g_ctx_allocs, g_ctx_frees, g_alloc_fail;
static void *obs_alloc(size_t n) { if (g_alloc_fail) return NULL; g_ctx_allocs++; return malloc(n); }
static void  obs_free(void *p)   { if (p) g_ctx_frees++; free(p); }

static void reset_all(void)
{
    memset(&g_pool, 0, sizeof g_pool);
    g_suspend_rc = 0; g_suspend_calls = 0; g_resume_calls = 0; g_exec_calls = 0;
    g_ctx_allocs = 0; g_ctx_frees = 0; g_alloc_fail = 0;
    smtp_submit_test_alloc = obs_alloc;
    smtp_submit_test_free  = obs_free;
}

static HlSmtpOp *make_inputs(void)
{
    HlSmtpMessage m; memset(&m, 0, sizeof m);
    m.host = "h"; m.port = 25; m.from = "f"; m.to = "t";
    m.subject = "s"; m.body = "b";
    return hl_smtp_op_create(&m, 1000);
}

/* Build a request with all seams wired; caller overrides the fake knobs. */
static HlSmtpSubmitReq make_req(HlSmtpAdmission *adm, void *pool)
{
    HlSmtpSubmitReq req;
    memset(&req, 0, sizeof req);
    req.inputs       = make_inputs();
    req.admission    = adm;
    req.execute      = exec_success;
    req.pool         = pool;
    req.pool_submit  = fake_pool_submit;
    req.suspend      = fake_suspend;
    req.resume       = fake_resume;
    return req;
}

/* ── tests ──────────────────────────────────────────────────────────── */

UTEST(smtp_submit, normal_completion_work_after_suspend)
{
    reset_all();
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);   /* cap 2 */
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_SUSPENDED);
    ASSERT_EQ((int)out.schedule, (int)HL_SMTP_SCHED_NONE);
    ASSERT_TRUE(out.ctx != NULL);
    ASSERT_EQ(g_suspend_calls, 1);
    ASSERT_EQ(g_exec_calls, 0);                 /* work not run yet */
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 1);  /* slot held */

    /* Worker runs, then the pool dispatches done. */
    g_pool.work(g_pool.user);
    ASSERT_EQ(g_exec_calls, 1);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);  /* lease released on worker side */
    g_pool.done(g_pool.user);
    ASSERT_EQ(g_resume_calls, 1);

    HlSmtpResult r; ASSERT_TRUE(hl_smtp_submit_ctx_terminal(out.ctx, &r));
    ASSERT_EQ(r.rc, 0);
    hl_smtp_submit_ctx_release(out.ctx);
    ASSERT_EQ(g_ctx_frees, 1);
}

UTEST(smtp_submit, fast_completion_before_suspend_holds_done)
{
    reset_all();
    g_pool.run_work_on_submit = 1;              /* work completes inside submit */
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_SUSPENDED);
    ASSERT_EQ(g_exec_calls, 1);                 /* worker already finished */
    ASSERT_EQ(g_suspend_calls, 1);
    ASSERT_EQ(g_resume_calls, 0);               /* done HELD: no resume before suspend */
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);

    /* Release the held done AFTER suspension: only now does resume fire. */
    g_pool.done(g_pool.user);
    ASSERT_EQ(g_resume_calls, 1);

    HlSmtpResult r; ASSERT_TRUE(hl_smtp_submit_ctx_terminal(out.ctx, &r));
    ASSERT_EQ(r.rc, 0);
    hl_smtp_submit_ctx_release(out.ctx);
    ASSERT_EQ(g_ctx_frees, 1);
}

UTEST(smtp_submit, submit_failure_queue_full)
{
    reset_all();
    g_pool.submit_rc = -1;                      /* queue full */
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_RESOLVED);
    ASSERT_EQ((int)out.schedule, (int)HL_SMTP_SCHED_QUEUE_FULL);
    ASSERT_TRUE(out.ctx == NULL);
    ASSERT_EQ(out.result.rc, -1);
    ASSERT_EQ(g_suspend_calls, 0);              /* never reached suspend */
    ASSERT_EQ(g_exec_calls, 0);                 /* discard opens no transport */
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);  /* lease released via discard */
    ASSERT_EQ(g_ctx_frees, 1);                  /* submit cleaned up its ctx */
}

UTEST(smtp_submit, suspend_failure_resolves_and_retains_ctx)
{
    reset_all();
    g_suspend_rc = -1;                          /* suspension setup fails */
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_RESOLVED);
    ASSERT_EQ((int)out.schedule, (int)HL_SMTP_SCHED_SUSPEND_FAILED);
    ASSERT_TRUE(out.ctx != NULL);               /* op is in the pool; caller sweeps */
    ASSERT_EQ(out.result.rc, -1);
    ASSERT_EQ(g_suspend_calls, 1);
    ASSERT_EQ(hl_smtp_submit_ctx_resumable(out.ctx), 0);  /* marked non-resumable */

    /* The worker still runs (from the pool): a cancel was requested, so it
     * publishes a cancelled terminal without opening the transport. */
    g_pool.work(g_pool.user);
    ASSERT_EQ(g_exec_calls, 0);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);  /* lease released */
    /* The pool later dispatches done; resume was NULLed, so it is a no-op. */
    g_pool.done(g_pool.user);
    ASSERT_EQ(g_resume_calls, 0);

    HlSmtpResult r; ASSERT_TRUE(hl_smtp_submit_ctx_terminal(out.ctx, &r));
    ASSERT_EQ(r.rc, -1);                          /* cancelled terminal */
    hl_smtp_submit_ctx_release(out.ctx);          /* the shutdown sweep */
    ASSERT_EQ(g_ctx_frees, 1);
}

UTEST(smtp_submit, queued_cancel_before_work_ran)
{
    reset_all();
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_SUSPENDED);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 1);

    /* Pool shutdown before the item ran: cancel_fn fires. */
    g_pool.cancel(g_pool.user);
    ASSERT_EQ(g_exec_calls, 0);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);  /* lease released via discard */
    ASSERT_EQ(g_resume_calls, 0);                    /* done never dispatched */

    HlSmtpResult r; ASSERT_TRUE(hl_smtp_submit_ctx_terminal(out.ctx, &r));
    ASSERT_EQ(r.rc, -1);
    hl_smtp_submit_ctx_release(out.ctx);             /* the shutdown sweep */
    ASSERT_EQ(g_ctx_frees, 1);
}

UTEST(smtp_submit, dropped_done_after_work_completed)
{
    reset_all();
    g_pool.run_work_on_submit = 1;              /* work completes during submit */
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_SUSPENDED);
    ASSERT_EQ(g_exec_calls, 1);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);

    /* Poll shutdown DROPS the enqueued done_fn: it is never invoked. The
     * retained runtime ref is dropped by the shutdown sweep. */
    ASSERT_EQ(g_resume_calls, 0);
    HlSmtpResult r; ASSERT_TRUE(hl_smtp_submit_ctx_terminal(out.ctx, &r));
    ASSERT_EQ(r.rc, 0);
    hl_smtp_submit_ctx_release(out.ctx);
    ASSERT_EQ(g_ctx_frees, 1);
    ASSERT_EQ(g_resume_calls, 0);                /* still no resume: done was dropped */
}

UTEST(smtp_submit, pool_unavailable)
{
    reset_all();
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    HlSmtpSubmitReq req = make_req(&adm, NULL);  /* active loop, no pool */
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_RESOLVED);
    ASSERT_EQ((int)out.schedule, (int)HL_SMTP_SCHED_POOL_UNAVAILABLE);
    ASSERT_TRUE(out.ctx == NULL);
    ASSERT_EQ(out.result.rc, -1);
    ASSERT_EQ(g_pool.submits, 0);
    ASSERT_EQ(g_ctx_allocs, 0);                  /* no ctx allocated before the gate */
    ASSERT_EQ(g_ctx_frees, 0);
}

UTEST(smtp_submit, cap_reached_admission_disabled)
{
    reset_all();
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 1);  /* W<=1 => cap 0 */
    ASSERT_EQ(hl_smtp_admission_cap(&adm), 0);
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_RESOLVED);
    ASSERT_EQ((int)out.schedule, (int)HL_SMTP_SCHED_CAP_REACHED);
    ASSERT_TRUE(out.ctx == NULL);
    ASSERT_EQ(g_pool.submits, 0);                /* never submitted */
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);
    ASSERT_EQ(g_ctx_frees, 1);                   /* ctx allocated then freed */
}

UTEST(smtp_submit, ctx_alloc_oom_resolves_without_sched_tag)
{
    reset_all();
    g_alloc_fail = 1;                            /* force ctx alloc failure */
    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    HlSmtpSubmitReq req = make_req(&adm, &g_pool);
    HlSmtpSubmitOutcome out;

    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_RESOLVED);
    ASSERT_EQ((int)out.schedule, (int)HL_SMTP_SCHED_NONE);  /* internal, no sched tag */
    ASSERT_TRUE(out.ctx == NULL);
    ASSERT_EQ(out.result.rc, -1);
    ASSERT_EQ(g_pool.submits, 0);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);
}

UTEST_MAIN();
