/**
 * @file cap/smtp_worker.h
 * @brief SMTP worker-op ownership core: state machine + refcount + cancellation.
 *
 * The runtime scheduling model runs the SMTP conversation on a bounded worker
 * thread while the runtime suspends (docs/smtp_keel_slice2c_plan.md). This unit
 * is the ownership CORE: a single linearizable atomic state machine, a two-owner
 * refcount, and the two-phase cancel-request / confirmed-completion contract. It
 * references no async backend, no runtime, and no borrowed configuration, and
 * the transport execute-phase is injected, so the ownership is fully unit
 * testable without a live pool, socket, or binding. Submission to the async pool
 * and runtime suspension live in separate layers.
 *
 * State machine (the CAS out of QUEUED and out of RUNNING are the linearization
 * points; exactly one of complete/cancel wins each):
 *
 *   QUEUED ---> RUNNING ---> COMPLETING ---> DONE
 *      \                                    /
 *       \--------> CANCEL_REQUESTED --------/
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_WORKER_H
#define HL_CAP_SMTP_WORKER_H

#include "hull/cap/smtp.h"      /* HlSmtpMessage, HlSmtpResult */
#include "hull/cap/smtp_op.h"   /* HlSmtpOp */

typedef enum {
    HL_SMTP_ST_QUEUED = 0,
    HL_SMTP_ST_RUNNING,
    HL_SMTP_ST_CANCEL_REQUESTED,
    HL_SMTP_ST_COMPLETING,
    HL_SMTP_ST_DONE,
} HlSmtpState;

typedef struct HlSmtpWorkerOp HlSmtpWorkerOp;

/* Poll callback the execute-phase calls at safe points to observe a cancel
 * request (returns 1 once cancellation has been requested). Supplied by the
 * worker core so the execute-phase can abort a long conversation. */
typedef int (*HlSmtpCancelPollFn)(void *poll_user);

/* The injectable transport execute-phase. Runs the SMTP conversation to terminal
 * and fills *out; may poll @p poll to abort early on a cancel request. Returns
 * out->rc. In production this wraps hl_smtp_execute and resolves its own
 * per-worker TLS context (no TLS-context pointer is kept in the op); tests
 * inject a fake. */
typedef int (*HlSmtpExecFn)(const HlSmtpMessage *msg, int timeout_ms,
                            HlSmtpResult *out,
                            HlSmtpCancelPollFn poll, void *poll_user,
                            void *exec_user);

/* Note on the resume hop: the worker NEVER kicks the runtime resume itself.
 * work_fn runs to terminal and returns; the pool then independently invokes the
 * pool-level done callback (registered by the submit layer at pool_submit),
 * which calls hl_net_op_complete() and nothing else. That backend-dispatch
 * boundary is what makes fast-completion-before-suspend safe, so no done
 * callback lives in this unit. */

/* Worker-side terminal hook, fired once on the worker thread right after
 * terminal publication (and, on the run path, confirmed teardown) and BEFORE the
 * worker-ref drop. This is where the admission lease is released: the pool's
 * later done callback is resume-only and may be dropped, so lease ownership lives
 * on the worker side. Keeps this unit admission-agnostic (a plain callback). */
typedef void (*HlSmtpTerminalFn)(HlSmtpWorkerOp *wop, void *user);

/* Create a worker op that takes ownership of @p inputs. refcount = 2 (one worker
 * ref, one runtime ref), state = QUEUED. Returns NULL on OOM (inputs untouched,
 * the caller still owns them to free). */
HlSmtpWorkerOp *hl_smtp_wop_create(HlSmtpOp *inputs,
                                   HlSmtpExecFn execute, void *exec_user);

/* Set the worker-side terminal hook (see HlSmtpTerminalFn). Must be called
 * before the op is submitted (single-threaded window; no race with the worker). */
void hl_smtp_wop_set_on_terminal(HlSmtpWorkerOp *w,
                                 HlSmtpTerminalFn fn, void *user);

/* Worker thread entry (the pool work_fn): dequeue (QUEUED -> RUNNING), run the
 * execute-phase, publish the terminal (release), fire the terminal hook, drop
 * the WORKER ref, and return. Honors a cancel requested before or during the run
 * (publishes a cancelled terminal instead). Does NOT invoke any resume: the pool
 * dispatches its own done callback after this returns. Exactly one of
 * run/discard is called per op. */
void hl_smtp_wop_run(HlSmtpWorkerOp *wop);

/* Pool cancel_fn for an op that never started (QUEUED at pool shutdown): publish
 * a cancelled terminal, fire the terminal hook, and drop the WORKER ref. No
 * transport is opened and no done callback is assumed. Exactly one of
 * run/discard is called per op. */
void hl_smtp_wop_discard(HlSmtpWorkerOp *wop);

/* Request cancellation (runtime/request teardown or a deadline). CAS
 * QUEUED/RUNNING -> CANCEL_REQUESTED; a no-op if the worker already reached
 * COMPLETING/DONE. Drops NO ref. Returns 1 if the request was registered, 0 if
 * the worker had already won the race. Safe from any thread. */
int hl_smtp_wop_request_cancel(HlSmtpWorkerOp *wop);

/* Mark the continuation non-resumable (a per-request cancel, or a suspension
 * setup that failed after submit). The runtime ref is RETAINED until terminal;
 * the pool's later done callback must then not resume a released runtime. */
void hl_smtp_wop_mark_unresumable(HlSmtpWorkerOp *wop);

/* Query whether the continuation was marked non-resumable (for the done_fn). */
int hl_smtp_wop_is_resumable(const HlSmtpWorkerOp *wop);

/* Drop the RUNTIME ref exactly once: from the resume path after a terminal, or
 * from the post-pool_free shutdown sweep if the done_fn was dropped. Must not be
 * called before the worker publishes a terminal (the caller ensures this). When
 * the last ref drops, the op shell + owned inputs are freed. */
void hl_smtp_wop_runtime_release(HlSmtpWorkerOp *wop);

/* Read the published terminal with acquire semantics. Returns 1 and fills *out
 * iff the op has reached DONE (payload fully visible), else 0. */
int hl_smtp_wop_terminal(HlSmtpWorkerOp *wop, HlSmtpResult *out);

/* Current state (acquire), for callers/tests. */
HlSmtpState hl_smtp_wop_state(const HlSmtpWorkerOp *wop);

#endif /* HL_CAP_SMTP_WORKER_H */
