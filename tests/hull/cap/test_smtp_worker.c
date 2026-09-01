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
 *   - queued cancel_fn (discard), dropped done_fn, non-resumable continuation;
 *   - no op-shell free before terminal publication.
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
static int          g_done_calls;
static int          g_done_state;     /* state observed inside the done_fn */
static int          g_free_before_done; /* 1 if a free was seen before done fired */
static int          g_exec_calls;
static HlSmtpResult g_terminal;       /* captured inside done_fn (before free) */
static int          g_terminal_valid;
static int          g_term_calls;     /* on_terminal hook invocations */
static int          g_term_state;     /* state observed inside on_terminal */
static int          g_term_before_free;   /* on_terminal fired before any free */
static int          g_term_before_done;   /* on_terminal fired before done_fn */

static void obs_reset(void)
{
    g_freed = 0; g_last_freed = NULL; g_done_calls = 0; g_done_state = -1;
    g_free_before_done = 0; g_exec_calls = 0;
    memset(&g_terminal, 0, sizeof g_terminal); g_terminal_valid = 0;
    g_term_calls = 0; g_term_state = -1; g_term_before_free = 0; g_term_before_done = 0;
}

/* Worker-side terminal hook (stands in for the admission-lease release). */
static void on_term(HlSmtpWorkerOp *w, void *user)
{
    (void)user;
    g_term_calls++;
    g_term_state = (int)hl_smtp_wop_state(w);   /* must be DONE */
    if (g_freed == 0)      g_term_before_free = 1;
    if (g_done_calls == 0) g_term_before_done = 1;
}

static void on_freed(void *w)
{
    g_freed++;
    g_last_freed = (HlSmtpWorkerOp *)w;
    if (g_done_calls == 0)
        g_free_before_done = 1;   /* freed before terminal handed off -> violation */
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

/* ── done_fn variants ───────────────────────────────────────────────── */
/* Resume path: terminal is visible, then drop the runtime ref. */
static void done_resume(HlSmtpWorkerOp *w, void *user)
{
    (void)user;
    g_done_calls++;
    /* release/acquire: seeing the hand-off means the terminal is published; the
     * acquire-load in terminal() must see the full payload. Captured here BEFORE
     * dropping the runtime ref (after which the op may be freed). */
    if (hl_smtp_wop_terminal(w, &g_terminal)) {
        g_terminal_valid = 1;
        g_done_state = HL_SMTP_ST_DONE;
    } else {
        g_done_state = (int)hl_smtp_wop_state(w);
    }
    hl_smtp_wop_runtime_release(w);
}

/* Dropped-done_fn / non-resumable: does NOT drop the runtime ref (the shutdown
 * sweep will), and must observe the continuation as non-resumable. */
static int g_saw_unresumable;
static void done_no_resume(HlSmtpWorkerOp *w, void *user)
{
    (void)user;
    g_done_calls++;
    g_done_state = (int)hl_smtp_wop_state(w);
    g_saw_unresumable = !hl_smtp_wop_is_resumable(w);
    if (hl_smtp_wop_terminal(w, &g_terminal))
        g_terminal_valid = 1;
    /* deliberately do NOT release here (the runtime-ref holder releases) */
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
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                           done_resume, NULL);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ((int)hl_smtp_wop_state(w), HL_SMTP_ST_QUEUED);

    hl_smtp_wop_run(w);   /* QUEUED -> RUNNING -> COMPLETING -> DONE */

    ASSERT_EQ(g_exec_calls, 1);
    ASSERT_EQ(g_done_calls, 1);
    ASSERT_EQ(g_done_state, HL_SMTP_ST_DONE);   /* terminal visible in done_fn */
    ASSERT_EQ(g_free_before_done, 0);           /* no free before terminal */
    ASSERT_EQ(g_freed, 1);                       /* worker + runtime ref -> freed once */
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, cancel_before_run_skips_exec)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                           done_resume, NULL);
    ASSERT_EQ(hl_smtp_wop_request_cancel(w), 1);                 /* QUEUED -> CANCEL_REQUESTED */
    ASSERT_EQ((int)hl_smtp_wop_state(w), HL_SMTP_ST_CANCEL_REQUESTED);

    hl_smtp_wop_run(w);   /* loses QUEUED->RUNNING, publishes cancelled */

    ASSERT_EQ(g_exec_calls, 0);                  /* transport never ran */
    ASSERT_TRUE(g_terminal_valid);
    ASSERT_EQ(g_terminal.rc, -1);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, cancel_during_run_wins_over_completion)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_cancels_mid_run, NULL,
                                           done_resume, NULL);
    /* exec_user must be the wop so the fake exec can request cancel on it. */
    w->exec_user = w;

    hl_smtp_wop_run(w);

    ASSERT_EQ(g_exec_calls, 1);
    ASSERT_TRUE(g_terminal_valid);
    ASSERT_EQ(g_terminal.rc, -1);                 /* cancel beat the completed exec */
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, completion_beats_late_cancel)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    /* done_no_resume keeps the runtime ref so the op stays alive after run for
     * the late-cancel + terminal inspection (no use-after-free). */
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                           done_no_resume, NULL);
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
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                           done_resume, NULL);
    hl_smtp_wop_discard(w);                        /* pool cancel_fn: never ran */
    ASSERT_EQ(g_exec_calls, 0);
    ASSERT_TRUE(g_terminal_valid);
    ASSERT_EQ(g_terminal.rc, -1);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, dropped_done_and_nonresumable_no_free_until_sweep)
{
    obs_reset();
    g_saw_unresumable = 0;
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                           done_no_resume, NULL);
    hl_smtp_wop_mark_unresumable(w);

    hl_smtp_wop_run(w);   /* worker drops its ref; runtime ref still held */

    ASSERT_EQ(g_done_calls, 1);
    ASSERT_EQ(g_saw_unresumable, 1);              /* done saw non-resumable */
    ASSERT_EQ(g_freed, 0);                         /* NOT freed: runtime ref pins the shell */
    ASSERT_EQ((int)hl_smtp_wop_state(w), HL_SMTP_ST_DONE);

    hl_smtp_wop_runtime_release(w);               /* the shutdown sweep */
    ASSERT_EQ(g_freed, 1);                          /* freed exactly once, after terminal */
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, early_runtime_release_shell_survives_until_publish)
{
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                           done_no_resume, NULL);
    /* Defensive: even if the runtime ref is released before terminal, the worker
     * ref keeps the shell alive through publication (no free-before-terminal). */
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 0);

    hl_smtp_wop_run(w);                            /* worker publishes then drops its ref */
    ASSERT_EQ(g_free_before_done, 0);
    ASSERT_EQ(g_freed, 1);
    smtp_wop_test_freed = 0;
}

UTEST(smtp_worker, on_terminal_fires_once_before_done_and_free)
{
    /* Completion path. */
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                           done_no_resume, NULL);
    hl_smtp_wop_set_on_terminal(w, on_term, NULL);
    hl_smtp_wop_run(w);
    ASSERT_EQ(g_term_calls, 1);
    ASSERT_EQ(g_term_state, HL_SMTP_ST_DONE);      /* fires after terminal publication */
    ASSERT_EQ(g_term_before_free, 1);               /* before any free */
    ASSERT_EQ(g_term_before_done, 1);               /* before done_fn */
    ASSERT_EQ(g_freed, 0);                           /* runtime ref still held */
    hl_smtp_wop_runtime_release(w);
    ASSERT_EQ(g_freed, 1);

    /* Discard (queued cancel_fn, never ran) also fires it exactly once. */
    obs_reset();
    smtp_wop_test_freed = on_freed;
    HlSmtpWorkerOp *w2 = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                            done_no_resume, NULL);
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
 * cancel. Each access is ref-protected, so whichever thread drops last frees. */
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
        HlSmtpWorkerOp *w = hl_smtp_wop_create(make_inputs(), exec_success, NULL,
                                               done_no_resume, NULL);
        ASSERT_TRUE(w != NULL);
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
        ASSERT_EQ(g_done_calls, 1);                 /* handed off exactly once */
        ASSERT_EQ(g_freed, 1);                       /* freed exactly once */
        ASSERT_EQ(g_free_before_done, 0);
        smtp_wop_test_freed = 0;
    }
}

UTEST_MAIN();
