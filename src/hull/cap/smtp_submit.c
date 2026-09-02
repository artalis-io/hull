/*
 * smtp_submit.c - SMTP submit / ordering layer (reserve -> submit -> suspend).
 * See include/hull/cap/smtp_submit.h and docs/smtp_keel_slice2c_plan.md.
 *
 * Composes the admission, worker-op, and (injected) pool + suspend seams on the
 * event-loop thread. References no async backend and no runtime directly: seam A
 * (pool_submit) and seam B (suspend/resume) arrive as function pointers, so the
 * ordering is unit-testable with a fake backend.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_submit.h"

#include <stdlib.h>
#include <string.h>

/* The pool `user`: shared by the work/done/cancel callbacks. Holds the worker op
 * (for work/cancel dispatch), the admission lease (released on the worker side by
 * the terminal hook), and the injected resume (called by done_fn only). Lifetime
 * is the RUNTIME ref's lifetime: every pool callback runs while that ref is held,
 * and the caller frees the ctx at ctx_release (resume path or shutdown sweep). */
struct HlSmtpSubmitCtx {
    HlSmtpWorkerOp      *wop;
    HlSmtpAdmissionLease lease;        /* released by submit_on_terminal */
    HlSmtpResumeFn       resume;       /* NULL after a suspension-setup failure */
    void                *resume_user;
};

/* Allocation seam (test hook): a test can force ctx alloc failure. */
#ifdef HL_SMTP_TEST_HOOKS
static void *(*smtp_submit_test_alloc)(size_t) = 0;
static void  (*smtp_submit_test_free)(void *)  = 0;
static void *submit_alloc(size_t n) { return smtp_submit_test_alloc ? smtp_submit_test_alloc(n) : malloc(n); }
static void  submit_free(void *p)   { if (smtp_submit_test_free) smtp_submit_test_free(p); else free(p); }
#else
#define submit_alloc(n) malloc(n)
#define submit_free(p)  free(p)
#endif

/* ── pool callbacks (one shared `user` = the submit ctx) ─────────────── */

static void submit_work(void *u)   { HlSmtpSubmitCtx *c = (HlSmtpSubmitCtx *)u; hl_smtp_wop_run(c->wop); }
static void submit_cancel(void *u) { HlSmtpSubmitCtx *c = (HlSmtpSubmitCtx *)u; hl_smtp_wop_discard(c->wop); }

/* done_fn: resume-only. Owns no lease, ref, payload, or free. A no-op when the
 * continuation was never parked (resume NULLed on suspension failure). */
static void submit_done(void *u)
{
    HlSmtpSubmitCtx *c = (HlSmtpSubmitCtx *)u;
    if (c->resume)
        c->resume(c->resume_user);
}

/* Worker-side terminal hook: release the admission lease exactly once, on the
 * worker thread, after terminal publication and before the worker-ref drop. */
static void submit_on_terminal(HlSmtpWorkerOp *w, void *u)
{
    (void)w;
    HlSmtpSubmitCtx *c = (HlSmtpSubmitCtx *)u;
    hl_smtp_admission_release(&c->lease);
}

/* ── helpers ─────────────────────────────────────────────────────────── */

static void resolved_connect_failed(HlSmtpSubmitOutcome *out, HlSmtpSchedule sched)
{
    out->disposition = HL_SMTP_SUBMIT_RESOLVED;
    out->schedule    = sched;
    out->result.rc              = -1;
    out->result.token           = "connect_failed";
    out->result.teardown_leaked = 0;
    out->ctx = NULL;
}

/* ── entry ───────────────────────────────────────────────────────────── */

void hl_smtp_submit(const HlSmtpSubmitReq *req, HlSmtpSubmitOutcome *out)
{
    memset(out, 0, sizeof *out);

    /* Defensive: a malformed request resolves without touching the pool. */
    if (!req || !req->execute || !req->inputs) {
        if (req && req->inputs)
            hl_smtp_op_free(req->inputs);
        resolved_connect_failed(out, HL_SMTP_SCHED_NONE);
        return;
    }

    /* Active loop but no pool: immediate connect_failed (do NOT fall back to a
     * synchronous send here - the no-loop path is the caller's decision). */
    if (!req->pool || !req->pool_submit) {
        hl_smtp_op_free(req->inputs);
        resolved_connect_failed(out, HL_SMTP_SCHED_POOL_UNAVAILABLE);
        return;
    }

    HlSmtpSubmitCtx *ctx = (HlSmtpSubmitCtx *)submit_alloc(sizeof *ctx);
    if (!ctx) {
        hl_smtp_op_free(req->inputs);
        resolved_connect_failed(out, HL_SMTP_SCHED_NONE);  /* internal, no sched tag */
        return;
    }
    memset(ctx, 0, sizeof *ctx);
    hl_smtp_admission_lease_init(&ctx->lease);

    /* Reserve a capacity slot for the queued+running lifetime. */
    if (!hl_smtp_admission_try_reserve(req->admission, &ctx->lease)) {
        submit_free(ctx);
        hl_smtp_op_free(req->inputs);
        resolved_connect_failed(out, HL_SMTP_SCHED_CAP_REACHED);
        return;
    }

    /* Create the worker op (takes ownership of inputs). */
    HlSmtpWorkerOp *wop = hl_smtp_wop_create(req->inputs, req->execute, req->exec_user);
    if (!wop) {
        hl_smtp_admission_release(&ctx->lease);   /* give the slot back */
        submit_free(ctx);
        hl_smtp_op_free(req->inputs);             /* wop_create left inputs to us */
        resolved_connect_failed(out, HL_SMTP_SCHED_NONE);
        return;
    }
    ctx->wop         = wop;
    ctx->resume      = req->resume;
    ctx->resume_user = req->resume_user;
    hl_smtp_wop_set_on_terminal(wop, submit_on_terminal, ctx);

    /* Seam A: enqueue. On rejection the op never entered the pool, so we own it
     * fully - discard (publishes cancelled -> terminal hook releases the lease ->
     * drops the worker ref), then drop our runtime ref and free the ctx. */
    if (req->pool_submit(req->pool, submit_work, submit_done, submit_cancel, ctx) != 0) {
        hl_smtp_wop_discard(wop);
        hl_smtp_wop_runtime_release(wop);
        submit_free(ctx);
        resolved_connect_failed(out, HL_SMTP_SCHED_QUEUE_FULL);
        return;
    }

    /* The op is now in the pool (the pool owns the worker-ref lifecycle; the
     * worker may already have run and published under a fast backend). Seam B:
     * park the continuation. submit-then-suspend keeps the resume hop safe. */
    if (req->suspend && req->suspend(req->suspend_user, ctx) == 0) {
        out->disposition = HL_SMTP_SUBMIT_SUSPENDED;
        out->schedule    = HL_SMTP_SCHED_NONE;
        out->ctx         = ctx;
        return;
    }

    /* Suspension setup failed after a successful submit: the continuation was
     * never parked. Make the pool's later done callback a no-op (resume NULL) and
     * the worker non-resumable, request cancellation, and resolve now. The op
     * stays in the pool and the runtime ref stays retained; the caller registers
     * ctx and drops that ref at the shutdown sweep (ctx is returned). */
    ctx->resume = NULL;
    hl_smtp_wop_mark_unresumable(wop);
    hl_smtp_wop_request_cancel(wop);
    out->disposition = HL_SMTP_SUBMIT_RESOLVED;
    out->schedule    = HL_SMTP_SCHED_SUSPEND_FAILED;
    out->result.rc              = -1;
    out->result.token           = "connect_failed";
    out->result.teardown_leaked = 0;
    out->ctx = ctx;
}

/* ── caller-owned ctx handling ───────────────────────────────────────── */

int hl_smtp_submit_ctx_terminal(HlSmtpSubmitCtx *ctx, HlSmtpResult *out)
{
    return hl_smtp_wop_terminal(ctx->wop, out);
}

int hl_smtp_submit_ctx_resumable(const HlSmtpSubmitCtx *ctx)
{
    return hl_smtp_wop_is_resumable(ctx->wop);
}

void hl_smtp_submit_ctx_cancel(HlSmtpSubmitCtx *ctx)
{
    hl_smtp_wop_mark_unresumable(ctx->wop);
    hl_smtp_wop_request_cancel(ctx->wop);
}

void hl_smtp_submit_ctx_release(HlSmtpSubmitCtx *ctx)
{
    if (!ctx)
        return;
    hl_smtp_wop_runtime_release(ctx->wop);   /* drop the retained runtime ref */
    submit_free(ctx);
}
