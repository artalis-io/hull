/**
 * @file cap/smtp_submit.h
 * @brief SMTP submit / ordering layer: reserve -> submit -> suspend.
 *
 * The event-loop-thread orchestration that turns a validated, deep-copied
 * #HlSmtpOp into either an ASYNCHRONOUS suspension (the worker runs the SMTP
 * conversation on a bounded pool thread while the runtime continuation is parked)
 * or an IMMEDIATE resolved failure carrying a frozen scheduling reason for audit.
 * It composes the three primitives below without owning any of them:
 *
 *   - #HlSmtpAdmission  - per-server capacity, reserved via a per-op lease;
 *   - #HlSmtpWorkerOp   - the ownership core (state machine + two-owner refcount);
 *   - the async pool    - seam A, injected as #HlSmtpPoolSubmitFn;
 *   - runtime suspend   - seam B, injected as #HlSmtpSuspendFn / #HlSmtpResumeFn.
 *
 * Both seams are INJECTED (function pointers), so the whole submit/ordering
 * discipline is unit-testable with a fake backend and no live runtime: a test can
 * hold a completed done_fn until after a simulated suspension (proving
 * fast-completion-before-suspend is safe) or drop it entirely (proving the
 * shutdown sweep owns the retained runtime ref). The runtime binding provides the
 * real seams and audits the immediate result exactly once.
 *
 * FROZEN ordering (docs/smtp_keel_slice2c_plan.md sections 5, 9):
 *   - submit-then-suspend: the worker may finish before the continuation parks;
 *     the pool never resumes until its done callback fires, which the submit layer
 *     wires but never invokes;
 *   - the admission lease is released on the TERMINAL-PRODUCING side (the terminal
 *     hook: work_fn, cancel_fn, or the submit-side discard), independent of done_fn
 *     (which is resume-only and may be dropped);
 *   - a suspension setup that fails AFTER a successful submit marks the op
 *     non-resumable, requests cancellation, and resolves immediately; the retained
 *     runtime ref is dropped by the caller's shutdown sweep, not here.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_SUBMIT_H
#define HL_CAP_SMTP_SUBMIT_H

#include "hull/cap/smtp.h"          /* HlSmtpResult */
#include "hull/cap/smtp_op.h"       /* HlSmtpOp */
#include "hull/cap/smtp_worker.h"   /* HlSmtpExecFn */
#include "hull/cap/smtp_admit.h"    /* HlSmtpAdmission */

/* Opaque submit context: the pool `user` shared by the work/done/cancel
 * callbacks, holding the worker op, the admission lease, and the injected resume.
 * Handed back to the caller whenever the op reached the pool (SUSPENDED, or a
 * post-submit suspension failure); the caller then owns it for the resume path
 * and the shutdown sweep and releases it exactly once via
 * hl_smtp_submit_ctx_release. */
typedef struct HlSmtpSubmitCtx HlSmtpSubmitCtx;

/* Seam A (injected): enqueue onto the async pool. Mirrors
 * HlAsyncBackend::pool_submit with an opaque pool so the ordering layer is
 * testable without a real backend. `work_fn` runs on a worker thread, `done_fn`
 * on the event-loop thread AFTER work_fn returns (may be dropped at pool
 * shutdown), `cancel_fn` on the event-loop thread for an item drained before it
 * ran; all three receive the same `user`. Returns 0 on success, <0 if the queue
 * rejected the item (queue full: the item never entered the pool). */
typedef int (*HlSmtpPoolSubmitFn)(void *pool,
                                  void (*work_fn)(void *),
                                  void (*done_fn)(void *),
                                  void (*cancel_fn)(void *),
                                  void *user);

/* Seam B setup (injected): park the runtime continuation bound to this submit
 * ctx (production: hl_net_op_suspend on the request's net_ctx). Returns 0 on
 * success, <0 if the suspension setup failed. */
typedef int (*HlSmtpSuspendFn)(void *suspend_user, HlSmtpSubmitCtx *ctx);

/* Resume (injected): the pool's done callback calls this to wake the parked
 * continuation (production: hl_net_op_complete). Resume-only: it owns no lease,
 * ref, payload, or free. */
typedef void (*HlSmtpResumeFn)(void *resume_user);

typedef struct {
    HlSmtpOp        *inputs;      /* ownership moves in (freed here on failure) */
    HlSmtpAdmission *admission;   /* per-server capacity (cap 0 => admission off) */
    HlSmtpExecFn     execute;     /* transport execute-phase (worker thread) */
    void            *exec_user;

    void               *pool;         /* opaque async pool; NULL => pool_unavailable */
    HlSmtpPoolSubmitFn  pool_submit;  /* seam A */

    HlSmtpSuspendFn     suspend;      /* seam B setup */
    void               *suspend_user;

    HlSmtpResumeFn      resume;       /* pool done_fn -> resume */
    void               *resume_user;
} HlSmtpSubmitReq;

typedef enum {
    HL_SMTP_SUBMIT_SUSPENDED = 0, /* async: worker running, continuation parked */
    HL_SMTP_SUBMIT_RESOLVED,      /* immediate: result is final now */
} HlSmtpSubmitDisposition;

/* Scheduling-failure reason (audit metadata only; the public token is always
 * connect_failed). NONE = no scheduling-failure tag (a suspended op, or an
 * internal resource failure that is not a scheduling-policy decision). */
typedef enum {
    HL_SMTP_SCHED_NONE = 0,
    HL_SMTP_SCHED_POOL_UNAVAILABLE, /* active loop but no pool */
    HL_SMTP_SCHED_CAP_REACHED,      /* admission cap reached */
    HL_SMTP_SCHED_QUEUE_FULL,       /* pool_submit rejected the item */
    HL_SMTP_SCHED_SUSPEND_FAILED,   /* submit ok but suspension setup failed */
} HlSmtpSchedule;

typedef struct {
    HlSmtpSubmitDisposition disposition;
    HlSmtpSchedule          schedule;  /* audit reason; NONE when SUSPENDED */
    HlSmtpResult            result;    /* valid iff RESOLVED */
    HlSmtpSubmitCtx        *ctx;       /* non-NULL iff the op reached the pool
                                          (SUSPENDED, or RESOLVED+suspend_failed):
                                          the caller owns it for resume + sweep and
                                          releases it via ctx_release. NULL when the
                                          op never entered the pool (submit already
                                          cleaned up). */
} HlSmtpSubmitOutcome;

/* Event-loop-thread entry. Reserves capacity, submits the worker op, and parks
 * the continuation, per the FROZEN ordering above. Never blocks and never runs
 * the transport itself. Always fills *out; takes ownership of req->inputs. */
void hl_smtp_submit(const HlSmtpSubmitReq *req, HlSmtpSubmitOutcome *out);

/* Map a scheduling failure to its FROZEN audit tag (section 3), or NULL for
 * HL_SMTP_SCHED_NONE (a suspended op / internal failure emits no schedule tag).
 * The returned string is a static literal. */
const char *hl_smtp_schedule_str(HlSmtpSchedule schedule);

/* ── caller-owned ctx handling (resume path + shutdown sweep) ─────────── */

/* Read the worker op's published terminal (acquire). Returns 1 + fills *out iff
 * the op reached DONE, else 0. Safe until ctx_release. */
int  hl_smtp_submit_ctx_terminal(HlSmtpSubmitCtx *ctx, HlSmtpResult *out);

/* Whether the parked continuation may still be resumed (0 after a per-request
 * cancel or a suspension-setup failure). */
int  hl_smtp_submit_ctx_resumable(const HlSmtpSubmitCtx *ctx);

/* Per-request cancellation (teardown / deadline): marks non-resumable and
 * requests worker cancellation. Retains the runtime ref until terminal (release
 * happens later via ctx_release). */
void hl_smtp_submit_ctx_cancel(HlSmtpSubmitCtx *ctx);

/* Drop the retained runtime ref exactly once and free the ctx: from the resume
 * path after reading the terminal, or from the post-pool_free shutdown sweep.
 * When the worker ref is already gone this frees the worker op too. */
void hl_smtp_submit_ctx_release(HlSmtpSubmitCtx *ctx);

#endif /* HL_CAP_SMTP_SUBMIT_H */
