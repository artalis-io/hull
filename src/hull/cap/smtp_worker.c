/*
 * smtp_worker.c - SMTP worker-op ownership core (state machine + refcount +
 * cancellation). See include/hull/cap/smtp_worker.h and
 * docs/smtp_keel_slice2c_plan.md. References no async backend, no runtime, and
 * no borrowed configuration; the transport execute-phase is injected.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_worker.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Free seam: plain libc in production; under HL_SMTP_TEST_HOOKS a test can
 * observe op-shell frees (count + ordering vs terminal publication). */
#ifdef HL_SMTP_TEST_HOOKS
static void (*smtp_wop_test_freed)(void *) = 0;   /* fired just before free(w) */
static void wop_shell_free(void *w) { if (smtp_wop_test_freed) smtp_wop_test_freed(w); free(w); }
#else
#define wop_shell_free(w) free(w)
#endif

struct HlSmtpWorkerOp {
    _Atomic int   state;       /* HlSmtpState; the single linearizable state */
    _Atomic int   refcount;    /* 2 at create: worker ref + runtime ref */
    _Atomic int   resumable;   /* 1 until marked non-resumable */

    HlSmtpOp     *inputs;      /* owned; freed with the shell at refcount 0 */
    HlSmtpMessage msg_view;    /* borrowed view over inputs (built once) */

    HlSmtpExecFn     execute;
    void            *exec_user;
    HlSmtpTerminalFn on_terminal;   /* worker-side lease release; set before submit */
    void            *on_terminal_user;

    HlSmtpResult  result;      /* published under the DONE release store */
    int           cancelled;   /* set with result before the release store */
};

/* ── refcount ────────────────────────────────────────────────────────── */

static void wop_unref(HlSmtpWorkerOp *w)
{
    if (atomic_fetch_sub_explicit(&w->refcount, 1, memory_order_acq_rel) == 1) {
        /* Last ref: safe to free. The op shell + owned inputs are reclaimed only
         * here, which - because the worker holds its ref until AFTER it publishes
         * the terminal (wop_publish_and_done) - can never happen before terminal
         * publication. */
        hl_smtp_op_free(w->inputs);
        wop_shell_free(w);
    }
}

/* ── publish (worker-exclusive once past the linearization CAS) ───────── */

static void wop_publish_and_done(HlSmtpWorkerOp *w, int cancelled,
                                 const HlSmtpResult *r)
{
    w->result    = *r;
    w->cancelled = cancelled;
    /* Release: the result stores above happen-before any acquire-load that
     * observes DONE, so a consumer that sees DONE sees the full payload. */
    atomic_store_explicit(&w->state, HL_SMTP_ST_DONE, memory_order_release);

    /* Worker-side terminal hook (lease release), after terminal publication and
     * BEFORE the worker-ref drop. Runs on completion, cancel, and discard; this
     * is where the admission lease is released, because the pool's later done
     * callback is resume-only and may be dropped. */
    if (w->on_terminal)
        w->on_terminal(w, w->on_terminal_user);

    /* Drop the WORKER ref, last thing the worker touches on this op. The pool
     * independently enqueues/invokes its own done callback after work_fn
     * returns; the worker NEVER kicks the runtime resume itself. */
    wop_unref(w);
}

/* The worker lost the race out of RUNNING (or dequeued a pre-cancelled QUEUED
 * op): advance the exclusively-worker-owned CANCEL_REQUESTED -> COMPLETING and
 * publish a cancelled terminal. The public token stays connect_failed; the
 * cancel distinction is audit metadata added at the completion phase. */
static void wop_finish_cancelled(HlSmtpWorkerOp *w)
{
    int exp = HL_SMTP_ST_CANCEL_REQUESTED;
    (void)atomic_compare_exchange_strong_explicit(
        &w->state, &exp, HL_SMTP_ST_COMPLETING,
        memory_order_acq_rel, memory_order_acquire);
    HlSmtpResult r = { .rc = -1, .token = "connect_failed", .teardown_leaked = 0 };
    wop_publish_and_done(w, 1, &r);
}

/* ── cancel poll handed to the execute-phase ─────────────────────────── */

static int wop_poll_cancel(void *user)
{
    HlSmtpWorkerOp *w = (HlSmtpWorkerOp *)user;
    return atomic_load_explicit(&w->state, memory_order_acquire)
           == HL_SMTP_ST_CANCEL_REQUESTED;
}

/* ── public API ──────────────────────────────────────────────────────── */

HlSmtpWorkerOp *hl_smtp_wop_create(HlSmtpOp *inputs,
                                   HlSmtpExecFn execute, void *exec_user)
{
    if (!inputs || !execute)
        return NULL;
    HlSmtpWorkerOp *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    atomic_init(&w->state, HL_SMTP_ST_QUEUED);
    atomic_init(&w->refcount, 2);
    atomic_init(&w->resumable, 1);
    w->inputs    = inputs;
    w->execute   = execute;
    w->exec_user = exec_user;
    hl_smtp_op_message(inputs, &w->msg_view);
    return w;
}

void hl_smtp_wop_set_on_terminal(HlSmtpWorkerOp *w, HlSmtpTerminalFn fn, void *user)
{
    w->on_terminal      = fn;
    w->on_terminal_user = user;
}

void hl_smtp_wop_run(HlSmtpWorkerOp *w)
{
    /* Linearization point 1: dequeue. QUEUED -> RUNNING, or lose to a
     * cancel-before-run (state is then CANCEL_REQUESTED). */
    int exp = HL_SMTP_ST_QUEUED;
    if (!atomic_compare_exchange_strong_explicit(
            &w->state, &exp, HL_SMTP_ST_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) {
        wop_finish_cancelled(w);   /* cancel-before-run: no transport opened */
        return;
    }

    /* RUNNING: run the injected transport execute-phase, which may poll for a
     * cancel-during-run and abort early. */
    HlSmtpResult r;
    memset(&r, 0, sizeof r);
    w->execute(&w->msg_view, w->inputs->timeout_ms, &r, wop_poll_cancel, w,
               w->exec_user);

    /* Linearization point 2: RUNNING -> COMPLETING (worker wins, publish the
     * real terminal) vs a racing RUNNING -> CANCEL_REQUESTED (canceller wins,
     * honor the cancel). Exactly one wins. */
    int exp2 = HL_SMTP_ST_RUNNING;
    if (atomic_compare_exchange_strong_explicit(
            &w->state, &exp2, HL_SMTP_ST_COMPLETING,
            memory_order_acq_rel, memory_order_acquire)) {
        wop_publish_and_done(w, 0, &r);
    } else {
        wop_finish_cancelled(w);
    }
}

void hl_smtp_wop_discard(HlSmtpWorkerOp *w)
{
    /* Pool cancel_fn: the op never started (QUEUED). Force it to CANCEL_REQUESTED
     * (a racing request_cancel may have already done so), then publish a
     * cancelled terminal. Exclusive advancer out of CANCEL_REQUESTED. */
    int exp = HL_SMTP_ST_QUEUED;
    (void)atomic_compare_exchange_strong_explicit(
        &w->state, &exp, HL_SMTP_ST_CANCEL_REQUESTED,
        memory_order_acq_rel, memory_order_acquire);
    wop_finish_cancelled(w);
}

int hl_smtp_wop_request_cancel(HlSmtpWorkerOp *w)
{
    int exp = HL_SMTP_ST_QUEUED;
    if (atomic_compare_exchange_strong_explicit(
            &w->state, &exp, HL_SMTP_ST_CANCEL_REQUESTED,
            memory_order_acq_rel, memory_order_acquire))
        return 1;
    exp = HL_SMTP_ST_RUNNING;
    if (atomic_compare_exchange_strong_explicit(
            &w->state, &exp, HL_SMTP_ST_CANCEL_REQUESTED,
            memory_order_acq_rel, memory_order_acquire))
        return 1;
    /* Neither CAS won: either a prior cancel already registered (state stays
     * CANCEL_REQUESTED, in effect -> report registered) or the worker already
     * reached COMPLETING/DONE (report not registered). */
    return atomic_load_explicit(&w->state, memory_order_acquire)
           == HL_SMTP_ST_CANCEL_REQUESTED;
}

void hl_smtp_wop_mark_unresumable(HlSmtpWorkerOp *w)
{
    atomic_store_explicit(&w->resumable, 0, memory_order_relaxed);
}

int hl_smtp_wop_is_resumable(const HlSmtpWorkerOp *w)
{
    return atomic_load_explicit(&w->resumable, memory_order_relaxed);
}

void hl_smtp_wop_runtime_release(HlSmtpWorkerOp *w)
{
    wop_unref(w);   /* drop the RUNTIME ref */
}

int hl_smtp_wop_terminal(HlSmtpWorkerOp *w, HlSmtpResult *out)
{
    if (atomic_load_explicit(&w->state, memory_order_acquire) != HL_SMTP_ST_DONE)
        return 0;
    *out = w->result;
    return 1;
}

HlSmtpState hl_smtp_wop_state(const HlSmtpWorkerOp *w)
{
    return (HlSmtpState)atomic_load_explicit(&w->state, memory_order_acquire);
}
