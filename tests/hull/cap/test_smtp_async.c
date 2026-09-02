/*
 * test_smtp_async.c - model-2 async SMTP orchestration: the two-pass shutdown.
 *
 * Focus (the ownership + lifecycle that must not need a live pool/server):
 *   - PASS 1 request_cancel_all marks every in-flight op non-resumable and flips
 *     its worker to CANCEL_REQUESTED - BEFORE the worker runs (i.e. before the
 *     pool join), so a running transport would observe the shutdown promptly;
 *   - PASS 2 sweep releases each retained runtime ref EXACTLY ONCE;
 *   - an op that completed + was removed on the normal path is NOT swept (the
 *     Keel done-drained case), while one still registered IS (the poll
 *     done-dropped case): each releases exactly once, no double-release;
 *   - the resume guard: a non-resumable op's resume is a no-op.
 *
 * Ops here use actx == NULL, so the sweep release routes straight through
 * smtp_async_free_driver (no live continuation) - the real HlAsyncCtx/net_op
 * teardown is covered end to end by e2e_smtp under both backends.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include (compiled -DHL_SMTP_TEST_HOOKS) for the op-alloc seam and
 * the static helpers (free_driver / sweep_release / resume). cap_smtp_async.o is
 * excluded from this test's link. */
#include "../../../src/hull/cap/smtp_async.c"

#include "hull/cap/smtp_submit.h"
#include "hull/cap/smtp_op.h"

#include <stdlib.h>
#include <string.h>

/* ── submit-layer fake backend (to mint real HlSmtpSubmitCtx values) ──── */
typedef struct {
    void (*work)(void *);
    void (*done)(void *);
    void (*cancel)(void *);
    void *user;
} FakePool;
static FakePool g_pool;
static int fake_pool_submit(void *pool, void (*w)(void *), void (*d)(void *),
                            void (*c)(void *), void *u)
{
    (void)pool;
    g_pool.work = w; g_pool.done = d; g_pool.cancel = c; g_pool.user = u;
    return 0;   /* accepted; work runs later (op stays QUEUED until we run it) */
}
static int  g_suspend_rc;
static int fake_suspend(void *su, HlSmtpSubmitCtx *ctx) { (void)su; (void)ctx; return g_suspend_rc; }
static int  g_resume_calls;
static void fake_resume(void *ru) { (void)ru; g_resume_calls++; }
static int exec_success(const HlSmtpMessage *m, int t, HlSmtpResult *out,
                        HlSmtpCancelPollFn poll, void *pu, void *eu)
{
    (void)m; (void)t; (void)poll; (void)pu; (void)eu;
    out->rc = 0; out->token = NULL; out->teardown_leaked = 0;
    return 0;
}

/* op-shell alloc/free observation (proves release-exactly-once). */
static int g_ao_allocs, g_ao_frees;
static void *ao_obs_alloc(size_t n) { g_ao_allocs++; return malloc(n); }
static void  ao_obs_free(void *p)   { if (p) g_ao_frees++; free(p); }

static HlSmtpOp *make_inputs(void)
{
    HlSmtpMessage m; memset(&m, 0, sizeof m);
    m.host = "h"; m.port = 25; m.from = "f"; m.to = "t"; m.subject = "s"; m.body = "b";
    return hl_smtp_op_create(&m, 1000);
}

/* Mint a suspended submit ctx via the submit layer's fake backend, capturing the
 * pool work fn so the test can "run the worker" (simulate the pool join) later. */
typedef struct { HlSmtpSubmitCtx *sctx; void (*work)(void *); void *work_user; } Minted;
static Minted mint_suspended(HlSmtpAdmission *adm)
{
    memset(&g_pool, 0, sizeof g_pool);
    g_suspend_rc = 0;
    HlSmtpSubmitReq req = {
        .inputs = make_inputs(), .admission = adm,
        .execute = exec_success, .pool = &g_pool, .pool_submit = fake_pool_submit,
        .suspend = fake_suspend, .resume = fake_resume,
    };
    HlSmtpSubmitOutcome so;
    hl_smtp_submit(&req, &so);
    Minted m = { so.ctx, g_pool.work, g_pool.user };
    return m;
}

/* Wrap a minted submit ctx into a registered async op (actx == NULL). */
static HlSmtpAsyncOp *register_op(HlSmtpServerCtx *s, HlSmtpSubmitCtx *sctx)
{
    HlSmtpAsyncOp *op = (HlSmtpAsyncOp *)ao_alloc(sizeof *op);
    memset(op, 0, sizeof *op);
    op->server = s;
    op->sctx   = sctx;
    op->actx   = NULL;       /* no live continuation in the unit harness */
    op->detached = 1;
    hl_smtp_inflight_add(&s->inflight, &op->node, op, smtp_async_sweep_release);
    return op;
}

static void setup_hooks(void)
{
    smtp_async_test_alloc = ao_obs_alloc;
    smtp_async_test_free  = ao_obs_free;
    g_ao_allocs = 0; g_ao_frees = 0; g_resume_calls = 0;
}

/* ── tests ──────────────────────────────────────────────────────────── */

UTEST(smtp_async, request_cancel_all_marks_before_worker_runs)
{
    setup_hooks();
    HlSmtpServerCtx s;
    hl_smtp_server_ctx_init(&s, 4, NULL, 0, NULL);   /* cap 2 */
    ASSERT_TRUE(hl_smtp_server_async_enabled(&s));

    Minted a = mint_suspended(&s.admission);
    Minted b = mint_suspended(&s.admission);
    ASSERT_TRUE(a.sctx != NULL); ASSERT_TRUE(b.sctx != NULL);
    HlSmtpAsyncOp *oa = register_op(&s, a.sctx);
    HlSmtpAsyncOp *ob = register_op(&s, b.sctx);
    ASSERT_EQ(hl_smtp_inflight_count(&s.inflight), 2);

    /* Both workers are still QUEUED (not run). PASS 1 must flip them to
     * CANCEL_REQUESTED and mark the continuations non-resumable - proving a
     * running transport would observe the shutdown BEFORE the pool join. */
    hl_smtp_server_request_cancel_all(&s);
    ASSERT_EQ(s.shutting_down, 1);
    ASSERT_EQ(hl_smtp_submit_ctx_resumable(a.sctx), 0);
    ASSERT_EQ(hl_smtp_submit_ctx_resumable(b.sctx), 0);
    /* The worker op reached CANCEL_REQUESTED via the submit ctx cancel. Prove it
     * by running the worker: with a cancel already requested it must NOT run the
     * transport (exec), publishing a cancelled terminal instead. */
    a.work(a.work_user);   /* "the pool join runs the worker" */
    b.work(b.work_user);
    HlSmtpResult ra, rb;
    ASSERT_TRUE(hl_smtp_submit_ctx_terminal(a.sctx, &ra));
    ASSERT_EQ(ra.rc, -1);   /* cancelled terminal, transport never ran */
    ASSERT_TRUE(hl_smtp_submit_ctx_terminal(b.sctx, &rb));
    ASSERT_EQ(rb.rc, -1);
    ASSERT_EQ(g_resume_calls, 0);   /* pass 1 suppressed any resume */

    /* Registry still holds both (pass 1 is registry-preserving). */
    ASSERT_EQ(hl_smtp_inflight_count(&s.inflight), 2);

    /* PASS 2: sweep releases each op + submit ctx exactly once. */
    int swept = hl_smtp_server_sweep(&s);
    ASSERT_EQ(swept, 2);
    ASSERT_EQ(g_ao_frees, 2);        /* both op shells freed once each */
    ASSERT_EQ(hl_smtp_inflight_count(&s.inflight), 0);
    (void)oa; (void)ob;
    smtp_async_test_free = 0; smtp_async_test_alloc = 0;
}

UTEST(smtp_async, normally_removed_op_is_not_swept)
{
    /* Backend-parity at the unit level: op A completed on the normal path (its
     * free_driver ran, removing it from the registry - the Keel done-drained
     * case); op B stayed registered (the poll done-dropped case). The sweep
     * releases only B, and each op releases exactly once overall. */
    setup_hooks();
    HlSmtpServerCtx s;
    hl_smtp_server_ctx_init(&s, 8, NULL, 0, NULL);   /* cap 4 */

    Minted a = mint_suspended(&s.admission);
    Minted b = mint_suspended(&s.admission);
    HlSmtpAsyncOp *oa = register_op(&s, a.sctx);
    register_op(&s, b.sctx);
    ASSERT_EQ(hl_smtp_inflight_count(&s.inflight), 2);

    /* A completes normally: run its worker (success), then its free_driver (what
     * the resume path's machinery would invoke) - unlinks + releases + frees. */
    a.work(a.work_user);
    smtp_async_free_driver(oa);       /* stands in for on_resume -> free_driver */
    ASSERT_EQ(hl_smtp_inflight_count(&s.inflight), 1);   /* A gone */
    ASSERT_EQ(g_ao_frees, 1);                              /* A freed once */

    /* B is still registered: the sweep releases exactly it. */
    b.work(b.work_user);
    int swept = hl_smtp_server_sweep(&s);
    ASSERT_EQ(swept, 1);
    ASSERT_EQ(g_ao_frees, 2);          /* B freed once; A not double-freed */
    ASSERT_EQ(hl_smtp_inflight_count(&s.inflight), 0);
    smtp_async_test_free = 0; smtp_async_test_alloc = 0;
}

UTEST(smtp_async, request_cancel_all_idempotent)
{
    setup_hooks();
    HlSmtpServerCtx s;
    hl_smtp_server_ctx_init(&s, 4, NULL, 0, NULL);
    Minted a = mint_suspended(&s.admission);
    register_op(&s, a.sctx);

    hl_smtp_server_request_cancel_all(&s);
    hl_smtp_server_request_cancel_all(&s);   /* second pass: no-op, still registered */
    ASSERT_EQ(hl_smtp_inflight_count(&s.inflight), 1);
    ASSERT_EQ(hl_smtp_submit_ctx_resumable(a.sctx), 0);

    a.work(a.work_user);
    ASSERT_EQ(hl_smtp_server_sweep(&s), 1);
    ASSERT_EQ(g_ao_frees, 1);
    smtp_async_test_free = 0; smtp_async_test_alloc = 0;
}

UTEST(smtp_async, resume_guard_noops_when_nonresumable)
{
    setup_hooks();
    HlSmtpServerCtx s;
    hl_smtp_server_ctx_init(&s, 4, NULL, 0, NULL);
    Minted a = mint_suspended(&s.admission);
    HlSmtpAsyncOp *op = register_op(&s, a.sctx);

    /* Mark non-resumable (as pass 1 does), then invoke the resume seam directly:
     * it must NOT touch the (NULL) actx - it returns early. */
    hl_smtp_submit_ctx_cancel(a.sctx);
    smtp_async_resume(op);            /* no crash, no resume */
    ASSERT_EQ(g_resume_calls, 0);

    a.work(a.work_user);
    ASSERT_EQ(hl_smtp_server_sweep(&s), 1);
    ASSERT_EQ(g_ao_frees, 1);
    smtp_async_test_free = 0; smtp_async_test_alloc = 0;
}

UTEST_MAIN();
