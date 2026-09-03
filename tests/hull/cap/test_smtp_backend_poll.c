/*
 * test_smtp_backend_poll.c - REAL poll async-backend SMTP shutdown execution.
 *
 * Unlike test_smtp_async (which models the pool with a fake), this drives the
 * ACTUAL poll backend (src/hull/async/poll.c: real worker threads + the real
 * pool_free) through the submit layer, so the §15 "poll drops done_fn" divergence
 * and the exactly-once retained-ref release are exercised end to end, not modelled.
 * The keel backend's counterpart is covered live by e2e_smtp.sh (the default build
 * links the keel async backend).
 *
 * Scenario: submit an op, let a real worker thread run the (injected) transport to
 * terminal, then tear the pool down WITHOUT ticking the loop - poll_pool_free joins
 * the worker (terminal already published, worker ref dropped) but drops the
 * enqueued done_fn. The shutdown sweep (ctx_release) then drops the one retained
 * runtime ref; ASan/TSan prove no UAF / double-free / leak.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/shared/async_backend.h"
#include "hull/cap/smtp_submit.h"
#include "hull/cap/smtp_op.h"
#include "hull/cap/smtp_admit.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const HlAsyncBackend hl_async_backend_poll;

static HlAsyncBackendPool *g_pool;

/* Seam A wired to the REAL poll pool. */
static int poll_submit(void *pool, HlAsyncWorkFn work, HlAsyncWorkFn done,
                       HlAsyncWorkFn cancel, void *user)
{
    (void)pool;
    return hl_async_backend_poll.pool_submit(g_pool, work, done, cancel, user);
}

static int  g_done_calls;   /* done_fn (resume) invocations - must stay 0 (dropped) */
static void resume_cb(void *u) { (void)u; g_done_calls++; }
static int  suspend_ok = 1;
static int  suspend_cb(void *su, HlSmtpSubmitCtx *c) { (void)su; (void)c; return suspend_ok ? 0 : -1; }

/* Injected transport execute: runs on a REAL poll worker thread. A tiny sleep
 * guarantees the op is genuinely in-flight on the worker for a beat. */
static int exec_ok(const HlSmtpMessage *m, int t, HlSmtpResult *out,
                   HlSmtpCancelPollFn poll, void *pu, void *eu)
{
    (void)m; (void)t; (void)poll; (void)pu; (void)eu;
    usleep(2000);
    out->rc = 0; out->token = NULL; out->teardown_leaked = 0;
    return 0;
}

static HlSmtpOp *make_inputs(void)
{
    HlSmtpMessage m; memset(&m, 0, sizeof m);
    m.host = "h"; m.port = 25; m.from = "f"; m.to = "t"; m.subject = "s"; m.body = "b";
    return hl_smtp_op_create(&m, 1000);
}

UTEST(smtp_backend_poll, real_worker_terminal_then_done_dropped_release_once)
{
    HlAsyncBackendCtx *ctx = NULL;
    ASSERT_EQ(hl_async_backend_poll.init(&ctx, NULL), 0);
    ASSERT_EQ(hl_async_backend_poll.pool_create(&g_pool, ctx, 2, 16), 0);

    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);   /* cap 2 */
    g_done_calls = 0; suspend_ok = 1;

    HlSmtpSubmitReq req = {
        .inputs = make_inputs(), .admission = &adm,
        .execute = exec_ok, .exec_user = NULL,
        .pool = (void *)1 /* non-NULL gate */, .pool_submit = poll_submit,
        .suspend = suspend_cb, .resume = resume_cb,
    };
    HlSmtpSubmitOutcome out;
    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_SUSPENDED);
    ASSERT_TRUE(out.ctx != NULL);
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 1);   /* slot held while queued/running */

    /* Wait for the REAL worker thread to publish the terminal (bounded). */
    HlSmtpResult r;
    int got = 0;
    for (int i = 0; i < 2000; i++) {
        if (hl_smtp_submit_ctx_terminal(out.ctx, &r)) { got = 1; break; }
        usleep(1000);
    }
    ASSERT_TRUE(got);
    ASSERT_EQ(r.rc, 0);                                 /* worker ran the transport */
    ASSERT_EQ(hl_smtp_admission_inflight(&adm), 0);     /* lease released on the worker side */

    /* Simulate shutdown pass 1 for this op (mark non-resumable), like
     * request_cancel_all would - so the dropped/held done never resumes. */
    hl_smtp_submit_ctx_cancel(out.ctx);

    /* pool_free: joins the worker (terminal already published), and DROPS the
     * enqueued done_fn - we never ticked the loop, and poll_pool_free drains only
     * never-ran items via cancel_fn, not the completion queue. */
    hl_async_backend_poll.pool_free(g_pool);
    ASSERT_EQ(g_done_calls, 0);                         /* done_fn dropped by poll shutdown */

    /* Shutdown pass 2: release the one retained runtime ref exactly once. Under
     * ASan/TSan this is where a double-free / UAF would fire if the ownership were
     * wrong (worker ref already gone, done dropped). */
    hl_smtp_submit_ctx_release(out.ctx);

    hl_async_backend_poll.free(ctx);
}

/* Companion: when the loop IS ticked, the real poll backend DISPATCHES done_fn
 * (the non-shutdown path) - the resume fires exactly once, then the runtime
 * releases the ref. Confirms the drop above is a shutdown property, not a bug. */
UTEST(smtp_backend_poll, real_worker_done_dispatched_when_ticked)
{
    HlAsyncBackendCtx *ctx = NULL;
    ASSERT_EQ(hl_async_backend_poll.init(&ctx, NULL), 0);
    ASSERT_EQ(hl_async_backend_poll.pool_create(&g_pool, ctx, 2, 16), 0);

    HlSmtpAdmission adm; hl_smtp_admission_init(&adm, 4);
    g_done_calls = 0; suspend_ok = 1;

    HlSmtpSubmitReq req = {
        .inputs = make_inputs(), .admission = &adm,
        .execute = exec_ok, .exec_user = NULL,
        .pool = (void *)1, .pool_submit = poll_submit,
        .suspend = suspend_cb, .resume = resume_cb,
    };
    HlSmtpSubmitOutcome out;
    hl_smtp_submit(&req, &out);
    ASSERT_EQ((int)out.disposition, (int)HL_SMTP_SUBMIT_SUSPENDED);

    /* Tick the loop until the worker's done_fn is dispatched (bounded). */
    for (int i = 0; i < 2000 && g_done_calls == 0; i++)
        hl_async_backend_poll.tick(ctx, 5);
    ASSERT_EQ(g_done_calls, 1);                          /* dispatched exactly once */

    HlSmtpResult r;
    ASSERT_TRUE(hl_smtp_submit_ctx_terminal(out.ctx, &r));
    ASSERT_EQ(r.rc, 0);
    hl_smtp_submit_ctx_release(out.ctx);                 /* the runtime consumes + releases */

    hl_async_backend_poll.pool_free(g_pool);
    hl_async_backend_poll.free(ctx);
}

UTEST_MAIN();
