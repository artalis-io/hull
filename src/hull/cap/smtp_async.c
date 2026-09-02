/*
 * smtp_async.c - model-2 async SMTP orchestration (shared ownership + lifecycle).
 * See include/hull/cap/smtp_async.h and docs/smtp_keel_slice2c_plan.md.
 *
 * Composes the submit layer, admission, per-worker TLS, and the in-flight
 * registry, and owns the frozen two-pass shutdown. References only the Hull seams
 * (hl_async_backend / hl_net_op_* / HlAsyncCtx), never a kl_ symbol (section 12).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_async.h"
#include "hull/cap/smtp_submit.h"
#include "hull/shared/async.h"          /* HlAsyncCtx (op/driver/free_driver), hl_async_ctx_* */
#include "hull/shared/async_backend.h"  /* hl_async_backend(), HlAsyncWorkFn */
#include "hull/net_backend.h"           /* hl_net_op_suspend/complete/cancel */
#include "hull/tls_transport.h"         /* KlTlsConfig, hl_tls_config_wire */

#include "hull/limits/core.h"           /* HL_SMTP_DEFAULT_TIMEOUT_MS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── the async op: the HlAsyncCtx driver + registry node ─────────────── */

struct HlSmtpAsyncOp {
    HlSmtpServerCtx    *server;
    HlSmtpSubmitCtx    *sctx;        /* the retained runtime ref; set at suspend */
    HlAsyncCtx         *actx;        /* continuation vehicle; NULL once torn down */
    HlNetBackendCtx    *net_ctx;
    void               *req_handle;  /* active connection (attached) */
    int                 detached;
    int                 audited;     /* audit-once guard */
    HlSmtpInflightNode  node;        /* registry link */
};

/* Allocation seam (test hook) for the op shell. */
#ifdef HL_SMTP_TEST_HOOKS
static void *(*smtp_async_test_alloc)(size_t) = 0;
static void  (*smtp_async_test_free)(void *)  = 0;
static void *ao_alloc(size_t n) { return smtp_async_test_alloc ? smtp_async_test_alloc(n) : malloc(n); }
static void  ao_free(void *p)   { if (smtp_async_test_free) smtp_async_test_free(p); else free(p); }
#else
#define ao_alloc(n) malloc(n)
#define ao_free(p)  free(p)
#endif

/* ── server ctx ──────────────────────────────────────────────────────── */

void hl_smtp_server_ctx_init(HlSmtpServerCtx *s, int workers,
                             const unsigned char *ca_buf, size_t ca_len,
                             void *alloc)
{
    hl_smtp_admission_init(&s->admission, workers);
    hl_smtp_inflight_init(&s->inflight);
    s->trust.ca_buf = ca_buf;
    s->trust.ca_len = ca_len;
    s->trust.alloc  = alloc;
    s->shutting_down = 0;
}

int hl_smtp_server_async_enabled(const HlSmtpServerCtx *s)
{
    return s && hl_smtp_admission_cap(&s->admission) > 0;
}

/* ── audit-once ──────────────────────────────────────────────────────── */

/* Emit the single completion audit for @p op if it has not been audited yet.
 * @p terminal is the section-3 tag ("cancelled") or NULL. Reads the message +
 * terminal from the submit ctx (valid until ctx_release). */
static void smtp_async_audit_once(HlSmtpAsyncOp *op, const char *terminal)
{
    if (op->audited || !op->sctx)
        return;
    HlSmtpResult r; HlSmtpMessage msg;
    if (hl_smtp_submit_ctx_terminal(op->sctx, &r) &&
        hl_smtp_submit_ctx_message(op->sctx, &msg)) {
        /* A Dop expiry (section 8) always tags terminal:post_resolution_deadline,
         * overriding the caller's tag (a raced external cancel would otherwise say
         * "cancelled"); else the caller's tag ("cancelled" on the sweep path, NULL
         * on normal completion). */
        const char *tag = r.deadline_expired ? "post_resolution_deadline" : terminal;
        hl_smtp_audit_complete(&msg, &r, NULL, tag);
    }
    op->audited = 1;
}

/* ── HlAsyncCtx free_driver: the single retained-ref release ──────────── */

/* Runs from the HlAsyncCtx machinery (on_resume or on_cancel), exactly once.
 * Unlinks from the registry (a no-op if the sweep already did), audits a
 * cancelled terminal iff this op was never normally completed (on the resume
 * path hl_smtp_async_finish already set audited), releases the ONE retained
 * runtime ref, and frees the op shell. */
static void smtp_async_free_driver(void *driver)
{
    HlSmtpAsyncOp *op = (HlSmtpAsyncOp *)driver;
    hl_smtp_inflight_remove(&op->node);
    /* If we reach here un-audited, the op ended by cancellation (the resume path
     * always audits first via finish), so tag terminal:cancelled. */
    smtp_async_audit_once(op, "cancelled");
    if (op->sctx)
        hl_smtp_submit_ctx_release(op->sctx);
    ao_free(op);
}

/* ── registry sweep release (pass 2) ─────────────────────────────────── */

/* Called by hl_smtp_inflight_sweep for each still-registered op (already
 * unlinked). Drives the parked continuation to its cancel terminal, which routes
 * through the machinery to smtp_async_free_driver (single release). A
 * suspend-failed op has no live actx (torn down at submit), so it releases
 * directly. */
static void smtp_async_sweep_release(void *owner)
{
    HlSmtpAsyncOp *op = (HlSmtpAsyncOp *)owner;
    if (op->actx) {
        if (op->detached)
            hl_async_ctx_cancel(op->actx);                       /* -> free_driver */
        else
            hl_net_op_cancel(op->net_ctx, (HlSuspendOp *)&op->actx->op); /* -> on_cancel -> free_driver */
    } else {
        smtp_async_free_driver(op);   /* no continuation to cancel */
    }
}

/* ── injected seams for the submit layer ─────────────────────────────── */

/* Seam A: enqueue onto the async pool (event-loop thread). */
static int smtp_async_pool_submit(void *pool, HlAsyncWorkFn work,
                                  HlAsyncWorkFn done, HlAsyncWorkFn cancel, void *user)
{
    return hl_async_backend()->pool_submit((HlAsyncBackendPool *)pool,
                                           work, done, cancel, user);
}

/* Seam B setup: wire the submit ctx into the driver, then park the continuation
 * (attached) or succeed with no net op (detached). */
static int smtp_async_suspend(void *suspend_user, HlSmtpSubmitCtx *sctx)
{
    HlSmtpAsyncOp *op = (HlSmtpAsyncOp *)suspend_user;
    op->sctx = sctx;                    /* driver now reaches the terminal + message */
    if (op->detached)
        return 0;
    return hl_net_op_suspend(op->net_ctx, (HlReqHandle *)op->req_handle,
                             (HlSuspendOp *)&op->actx->op);
}

/* Resume (pool done_fn -> resume): SUPPRESSED when the op was marked
 * non-resumable (a per-request cancel or shutdown pass 1) - the op is then left
 * parked for the sweep, so shutdown never resumes a coroutine/Promise. */
static void smtp_async_resume(void *resume_user)
{
    HlSmtpAsyncOp *op = (HlSmtpAsyncOp *)resume_user;
    if (!op->sctx || !hl_smtp_submit_ctx_resumable(op->sctx))
        return;
    if (op->detached)
        hl_async_ctx_resume_detached(op->actx);
    else
        hl_net_op_complete(op->net_ctx, (HlSuspendOp *)&op->actx->op);
}

/* The transport execute-phase (worker thread): resolve THIS thread's client TLS
 * context from the server's immutable CA trust, then run the SMTP conversation.
 * exec_user is the server ctx (for the trust). */
static int smtp_async_exec(const HlSmtpMessage *msg, int timeout_ms,
                           HlSmtpResult *out, HlSmtpCancelPollFn poll,
                           void *poll_user, void *exec_user)
{
    HlSmtpServerCtx *server = (HlSmtpServerCtx *)exec_user;
    void *tls_ctx = server->trust.ca_buf ? hl_smtp_tls_ctx_for(&server->trust) : NULL;
    KlTlsConfig cfg;
    KlTlsConfig *cfgp = NULL;
    if (tls_ctx) { hl_tls_config_wire(&cfg, tls_ctx); cfgp = &cfg; }
    /* Thread the worker's cancel poll into the transport pumps so a requested
     * cancel (shutdown pass 1 / per-request teardown) aborts the conversation
     * within a pump step instead of at the stage timeout. */
    return hl_smtp_execute(msg, cfgp, timeout_ms, poll, poll_user, out);
}

/* ── submit ──────────────────────────────────────────────────────────── */

/* EXACT owned snapshot of the four audited message fields. The submit layer takes
 * + FREES the inputs on some scheduling-failure paths, so the immediate audit must
 * not read the op's (freed) borrowed strings - own a full copy here, before
 * submit. strdup, not a bounded buffer: audit content must be byte-identical to
 * the sync/terminal audits, never silently truncated. */
typedef struct { char *host, *from, *to, *subject; } SmtpAuditOwned;
static char *audit_dup(const char *s) { return strdup(s ? s : ""); }
static void audit_owned_take(const HlSmtpMessage *m, SmtpAuditOwned *o)
{
    o->host = audit_dup(m->host); o->from = audit_dup(m->from);
    o->to = audit_dup(m->to);     o->subject = audit_dup(m->subject);
}
static void audit_owned_free(SmtpAuditOwned *o)
{
    free(o->host); free(o->from); free(o->to); free(o->subject);
}
/* Build an audit message from the owned copies (NULL-safe: a strdup OOM leaves ""
 * so the field key is still emitted, matching the other audit paths). */
static HlSmtpMessage audit_owned_msg(const SmtpAuditOwned *o)
{
    HlSmtpMessage m = { .host = o->host ? o->host : "", .from = o->from ? o->from : "",
                        .to = o->to ? o->to : "", .subject = o->subject ? o->subject : "" };
    return m;
}

void hl_smtp_async_submit(const HlSmtpAsyncReq *req, HlSmtpAsyncOutcome *out)
{
    memset(out, 0, sizeof *out);

    /* Own an EXACT copy of the audit fields BEFORE the submit layer frees inputs. */
    HlSmtpMessage view; hl_smtp_op_message(req->inputs, &view);
    SmtpAuditOwned owned; audit_owned_take(&view, &owned);
    HlSmtpMessage msg = audit_owned_msg(&owned);

    HlSmtpAsyncOp *op = (HlSmtpAsyncOp *)ao_alloc(sizeof *op);
    if (!op) {
        hl_smtp_op_free(req->inputs);
        /* Tear down the unparked continuation the binding created. */
        if (req->actx) { req->actx->cont->destroy(req->actx->cont); hl_async_ctx_free(req->actx); }
        audit_owned_free(&owned);
        out->disposition = HL_SMTP_ASYNC_RESOLVED;
        out->result.rc = -1; out->result.token = "connect_failed";
        return;
    }
    memset(op, 0, sizeof *op);
    op->server     = req->server;
    op->actx       = req->actx;
    op->net_ctx    = req->net_ctx;
    op->req_handle = req->req_handle;
    op->detached   = req->detached;

    /* The op is the continuation's driver; free_driver drops the retained ref. */
    req->actx->driver      = op;
    req->actx->free_driver = smtp_async_free_driver;

    HlSmtpSubmitReq sreq = {
        .inputs       = req->inputs,
        .admission    = &req->server->admission,
        .execute      = smtp_async_exec,
        .exec_user    = req->server,
        .pool         = req->pool,
        .pool_submit  = smtp_async_pool_submit,
        .suspend      = smtp_async_suspend,
        .suspend_user = op,
        .resume       = smtp_async_resume,
        .resume_user  = op,
    };
    HlSmtpSubmitOutcome so;
    hl_smtp_submit(&sreq, &so);

    if (so.disposition == HL_SMTP_SUBMIT_SUSPENDED) {
        op->sctx = so.ctx;   /* also set by the suspend seam; idempotent */
        hl_smtp_inflight_add(&req->server->inflight, &op->node, op,
                             smtp_async_sweep_release);
        audit_owned_free(&owned);   /* completion audit reads the live op, not this */
        out->disposition = HL_SMTP_ASYNC_SUSPENDED;
        out->op = op;
        return;
    }

    /* RESOLVED (immediate scheduling failure): audit exactly once, here. */
    out->disposition = HL_SMTP_ASYNC_RESOLVED;
    out->result      = so.result;
    out->schedule    = hl_smtp_schedule_str(so.schedule);

    if (so.ctx) {
        /* suspend_failed: the op is in the pool (retained for the sweep). The
         * continuation was never parked - tear it down now WITHOUT releasing the
         * submit ctx (the sweep owns that). Audit the scheduling failure now and
         * mark audited so the sweep's free_driver does not re-audit. */
        op->sctx = so.ctx;
        hl_smtp_audit_complete(&msg, &so.result, out->schedule, NULL);
        op->audited = 1;
        if (op->actx) {
            op->actx->cont->destroy(op->actx->cont);
            hl_async_ctx_free(op->actx);
            op->actx = NULL;   /* sweep release then goes direct (no live actx) */
        }
        hl_smtp_inflight_add(&req->server->inflight, &op->node, op,
                             smtp_async_sweep_release);
        audit_owned_free(&owned);
        /* Do NOT return op to the binding: it resolves immediately, no yield. */
        return;
    }

    /* pool_unavailable / cap_reached / queue_full: the submit layer already freed
     * the submit ctx + inputs. The continuation was never used - tear it down
     * (no cont->cancel: the binding returns a value, it did not yield). Audit once. */
    hl_smtp_audit_complete(&msg, &so.result, out->schedule, NULL);
    audit_owned_free(&owned);
    if (op->actx) {
        op->actx->cont->destroy(op->actx->cont);
        hl_async_ctx_free(op->actx);
    }
    ao_free(op);
}

void hl_smtp_async_finish(HlSmtpAsyncOp *op, HlSmtpResult *out)
{
    /* The single completion audit for the normal resume path (no schedule/terminal
     * tag). Marks audited so free_driver does not also emit a cancelled record. */
    HlSmtpResult r; memset(&r, 0, sizeof r); r.rc = -1;
    if (op->sctx)
        hl_smtp_submit_ctx_terminal(op->sctx, &r);
    smtp_async_audit_once(op, NULL);
    *out = r;
}

/* ── FROZEN two-pass shutdown ─────────────────────────────────────────── */

static void smtp_async_cancel_one(void *owner, void *user)
{
    (void)user;
    HlSmtpAsyncOp *op = (HlSmtpAsyncOp *)owner;
    if (op->sctx)
        hl_smtp_submit_ctx_cancel(op->sctx);   /* non-resumable + worker CANCEL_REQUESTED */
}

void hl_smtp_server_request_cancel_all(HlSmtpServerCtx *s)
{
    if (!s) return;
    s->shutting_down = 1;                       /* stop new submissions */
    hl_smtp_inflight_for_each(&s->inflight, smtp_async_cancel_one, NULL);
}

int hl_smtp_server_sweep(HlSmtpServerCtx *s)
{
    if (!s) return 0;
    return hl_smtp_inflight_sweep(&s->inflight);
}
