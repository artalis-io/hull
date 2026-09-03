/*
 * test_smtp_worker.c - SMTP worker-op ownership core.
 *
 * Independently pins the state machine, refcount, and cancellation contract
 * with an injected fake execute-phase (no live SMTP, pool, or runtime binding):
 *   - legal + illegal state transitions;
 *   - cancel-before-run and cancel-during-run;
 *   - cancellation vs completion CAS races under repetition (+ TSan via make tsan);
 *   - release/acquire visibility of the terminal payload;
 *   - exactly-once worker + runtime ref drops;
 *   - queued cancel_fn (discard), dropped-done shutdown sweep, non-resumable
 *     continuation;
 *   - no op-shell free before terminal publication.
 *
 * The worker NEVER kicks a runtime resume itself: work_fn publishes the terminal,
 * fires the terminal hook (lease release), drops the worker ref, and returns; the
 * pool independently invokes its own done callback afterwards. These tests model
 * the runtime side directly (capture the published terminal, then drop the
 * runtime ref) rather than through a done_fn.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include (compiled with -DHL_SMTP_TEST_HOOKS) for the free
 * observer; cap_smtp_worker.o is excluded from this test's link. */
#include "../../../src/hull/cap/smtp_worker.c"

#include "hull/cap/smtp_op.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── shared observation state ────────────────────────────────────────── */
static int          g_freed;          /* op-shell frees observed */
static HlSmtpWorkerOp *g_last_freed;
static int          g_free_state;     /* state observed at free time (must be DONE) */
static int          g_exec_calls;
static HlSmtpResult g_terminal;       /* captured terminal payload */
static int          g_terminal_valid;
static int          g_term_calls;     /* on_terminal hook invocations */
static int          g_term_state;     /* state observed inside on_terminal */
static int          g_term_before_free;   /* on_terminal fired before any free */

static void obs_reset(void)
{
    g_freed = 0; g_last_freed = NULL; g_free_state = -1; g_exec_calls = 0;
    memset(&g_terminal, 0, sizeof g_terminal); g_terminal_valid = 0;
    g_term_calls = 0; g_term_state = -1; g_term_before_free = 0;
}

/* Worker-side terminal hook (stands in for the admission-lease release). Fires
 * after terminal publication and before the worker-ref drop, so it can safely
 * capture the published terminal on the worker's own thread. */
static void on_term(HlSmtpWorkerOp *w, void *user)
{
    (void)user;
    g_term_calls++;
    g_term_state = (int)hl_smtp_wop_state(w);   /* must be DONE */
    if (g_freed == 0) g_term_before_free = 1;
    if (hl_smtp_wop_terminal(w, &g_terminal)) g_terminal_valid = 1;
}

static void on_freed(void *w)
{
    g_freed++;
    g_last_freed = (HlSmtpWorkerOp *)w;
    /* The shell is still valid here (fired just before free): a free must never
     * be observed before the terminal is published. */
    g_free_state = (int)hl_smtp_wop_state((HlSmtpWorkerOp *)w);
}

/* Runtime-side capture: the runtime ref is retained after run(), so the terminal
 * is readable until the caller drops that ref via hl_smtp_wop_runtime_release. */
static void rt_capture(HlSmtpWorkerOp *w)
{
    if (hl_smtp_wop_terminal(w, &g_terminal)) g_terminal_valid = 1;
}

/* ── injectable execute-phase variants ──────────────────────────────── */
static int exec_success(const HlSmtpMessage *msg, int timeout_ms,
                        HlSmtpResult *out, HlSmtpCancelPollFn poll,
                        void *poll_user, void *exec_user)
{
    (void)msg; (void)timeout_ms; (void)poll; (void)poll_user; (void)exec_user;
    g_exec_calls++;
    out->rc = 0; out->token = NULL; out->teardown_leaked = 0;
    return 0;
}

/* Simulates a cancel arriving DURING the run: requests cancel on itself, then
 * returns a completed result. The worker's finish-CAS must then lose and publish
 * a cancelled terminal instead of this rc=0. exec_user is the wop. */
static int exec_cancels_mid_run(const HlSmtpMessage *msg, int timeout_ms,
                                HlSmtpResult *out, HlSmtpCancelPollFn poll,
                                void *poll_user, void *exec_user)
{
    (void)msg; (void)timeout_ms;
    g_exec_calls++;
    HlSmtpWorkerOp *w = (HlSmtpWorkerOp *)exec_user;
    hl_smtp_wop_request_cancel(w);
    /* the poll must now report the cancel */
    (void)poll; (void)poll_user;
    out->rc = 0; out->token = NULL; out->teardown_leaked = 0;
    return 0;
}

/* Build a minimal owned op for the worker to carry. */
static HlSmtpOp *make_inputs(void)
{
    HlSmtpMessage m; memset(&m, 0, sizeof m);
    m.host = "h"; m.port = 25; m.from = "f"; m.to = "t";
    m.subject = "s"; m.body = "b";
    return hl_smtp_op_create(&m, 1000);
}

/* ── tests ──────────────────────────────────────────────────────────── */

UTEST(smtp_worker, happy_path_queued_running_completing_done)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ((int)hl_smtp_wop_state(w), HL_SMTP_ST_QUEUED);

    hl_smtp_wop_run(w);   /* QUEUED -> RUNNING -> COMPLETING -> DONE */

    ASSERT_EQ(g_exec_calls, 1);
    ASSERT_EQ((int)hl_smtp_wop_state(w), HL_SMTP_ST_DONE); /* terminal published */
    ASSERT_EQ(g_freed, 0);                       /* runtime ref still held */
    rt_capture(w);                               /* runtime reads the terminal */
    ASSERT_TRUE(g_terminal_valid);
    ASSERT_EQ(g_terminal.rc, 0);
    hl_smtp_wop_runtime_release(w);              /* drop the runtime ref */
    ASSERT_EQ(g_freed, 1);                       /* worker + runtime ref -> freed once */
    ASSERT_EQ(g_free_state, HL_SMTP_ST_DONE);    /* no free before terminal */
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, cancel_before_run_skips_exec)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    ASSERT_EQ(hl_smtp_wop_request_cancel(w), 1);                 /* QUEUED -> CANCEL_REQUESTED */
    ASSERT_EQ((int)hl_smtp_wop_state(w), HL_SMTP_ST_CANCEL_REQUESTED);

    hl_smtp_wop_run(w);   /* loses QUEUED->RUNNING, publishes cancelled */

    ASSERT_EQ(g_exec_calls, 0);                  /* transport never ran */
    rt_capture(w);
    ASSERT_TRUE(g_terminal_valid);
    ASSERT_EQ(g_terminal.rc, -1);
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, cancel_during_run_wins_over_completion)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_cancels_mid_run, NULL);
    /* exec_user must be the wop so the fake exec can request cancel on it. */
    w->exec_user = w;

    hl_smtp_wop_run(w);

    ASSERT_EQ(g_exec_calls, 1);
    rt_capture(w);
    ASSERT_TRUE(g_terminal_valid);
    ASSERT_EQ(g_terminal.rc, -1);                 /* cancel beat the completed exec */
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, completion_beats_late_cancel)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    hl_smtp_wop_run(w);                           /* completes -> DONE */
    ASSERT_EQ(hl_smtp_wop_request_cancel(w), 0);  /* worker already won */
    HlSmtpResult t; ASSERT_TRUE(hl_smtp_wop_terminal(w, &t));
    ASSERT_EQ(t.rc, 0);
    ASSERT_EQ(g_freed, 0);                          /* runtime ref still held */
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, discard_never_ran_publishes_cancelled)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    hl_smtp_wop_discard(w);                        /* pool cancel_fn: never ran */
    ASSERT_EQ(g_exec_calls, 0);
    rt_capture(w);
    ASSERT_TRUE(g_terminal_valid);
    ASSERT_EQ(g_terminal.rc, -1);
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, dropped_done_and_nonresumable_no_free_until_sweep)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    hl_smtp_wop_mark_unresumable(w);

    hl_smtp_wop_run(w);   /* worker drops its ref; runtime ref still held */

    /* The pool's done callback was dropped (never invoked), so nothing resumes
     * or releases the runtime ref here. The continuation must read non-resumable
     * and the shell must survive on the retained runtime ref. */
    ASSERT_EQ(hl_smtp_wop_is_resumable(w), 0);    /* marked non-resumable */
    ASSERT_EQ(g_freed, 0);                         /* NOT freed: runtime ref pins the shell */
    ASSERT_EQ((int)hl_smtp_wop_state(w), HL_SMTP_ST_DONE);

    hl_smtp_wop_runtime_release(w);               /* the shutdown sweep */
    ASSERT_EQ(g_freed, 1);                          /* freed exactly once, after terminal */
    ASSERT_EQ(g_free_state, HL_SMTP_ST_DONE);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, early_runtime_release_shell_survives_until_publish)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    /* Defensive: even if the runtime ref is released before terminal, the worker
     * ref keeps the shell alive through publication (no free-before-terminal). */
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 0);

    hl_smtp_wop_run(w);                            /* worker publishes then drops its ref */
    ASSERT_EQ(g_freed, 1);
    ASSERT_EQ(g_free_state, HL_SMTP_ST_DONE);      /* freed only after terminal */
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, on_terminal_fires_once_before_free)
{
    /* Completion path. */
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    hl_smtp_wop_set_on_terminal(w, on_term, NULL);
    hl_smtp_wop_run(w);
    ASSERT_EQ(g_term_calls, 1);
    ASSERT_EQ(g_term_state, HL_SMTP_ST_DONE);      /* fires after terminal publication */
    ASSERT_EQ(g_term_before_free, 1);               /* before any free */
    ASSERT_TRUE(g_terminal_valid);                  /* captured on the worker side */
    ASSERT_EQ(g_freed, 0);                           /* runtime ref still held */
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 1);

    /* Discard (queued cancel_fn, never ran) also fires it exactly once. */
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w2 = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
    hl_smtp_wop_set_on_terminal(w2, on_term, NULL);
    hl_smtp_wop_discard(w2);
    ASSERT_EQ(g_term_calls, 1);
    ASSERT_EQ(g_term_state, HL_SMTP_ST_DONE);
    ASSERT_EQ(g_term_before_free, 1);
    hl_smtp_wop_runtime_release(w2);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

/* ── cancel-vs-completion CAS race under repetition (+ TSan) ─────────── */
/* The worker thread owns the WORKER ref (dropped in run); the canceller thread
 * owns the RUNTIME ref (it is the runtime side) and drops it after requesting
 * cancel. Each access is ref-protected, so whichever thread drops last frees.
 * The terminal is captured on the worker's own thread via the on_terminal hook,
 * before the worker-ref drop, so no thread reads the op after it is freed. */
typedef struct { HlSmtpWorkerOp *w; } RaceArg;
static void *race_worker(void *a) { hl_smtp_wop_run(((RaceArg *)a)->w); return NULL; }
static void *race_cancel(void *a)
{
    HlSmtpWorkerOp *w = ((RaceArg *)a)->w;
    hl_smtp_wop_request_cancel(w);
    hl_smtp_wop_runtime_release(w);   /* the runtime ref this side owns */
    return NULL;
}

UTEST(smtp_worker, race_cancel_vs_completion_exactly_once)
{
    for (int i = 0; i < 2000; i++) {
        obs_reset();
        smtp_wop_test_freed = on_freed;
        HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL);
        ASSERT_TRUE(w != NULL);
        hl_smtp_wop_set_on_terminal(w, on_term, NULL);   /* worker-side capture */
        RaceArg arg = { w };
        pthread_t t1, t2;
        pthread_create(&t1, NULL, race_worker, &arg);
        pthread_create(&t2, NULL, race_cancel, &arg);
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
        /* The op is freed by now (worker + runtime refs dropped); inspect only
         * the captured terminal, never the freed op. */
        ASSERT_TRUE(g_terminal_valid);              /* reached DONE */
        ASSERT_TRUE(g_terminal.rc == 0 || g_terminal.rc == -1);  /* exactly one terminal */
        ASSERT_EQ(g_term_calls, 1);                 /* published exactly once */
        ASSERT_EQ(g_freed, 1);                       /* freed exactly once */
        ASSERT_EQ(g_free_state, HL_SMTP_ST_DONE);
        smtp_wop_test_freed = 0;
    }
}

UTEST_MAIN();
