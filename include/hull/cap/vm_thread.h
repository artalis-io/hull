/*
 * vm_thread.h - run an app VM's entries on a thread of their own
 * (audit convergence phase 5, design §6.3 phase 1; --vm-thread)
 *
 * Without it, a script run executes on the event-loop thread, and one native
 * call that ignores the run's stop flag (an uncharged engine builtin, a long
 * synchronous capability call) holds every connection of the server until it
 * returns. With it, the entries the runtime offloads (HTTP handlers and
 * middleware, timers, async resumes, WebSocket / SSE callbacks; see each
 * runtime's vmt wrappers) run on one dedicated VM thread, and the loop thread
 * waits for each of them to finish.
 *
 * The model is a BATON, not a queue of concurrent work: at any moment either
 * the loop thread or the VM thread runs Hull code, never both, and every
 * hand-off goes through one mutex, so whatever the VM thread wrote is visible
 * to the loop thread afterwards and vice versa. Every existing data structure
 * (the Keel server, connections, responses, the async / park machinery, the
 * runtime, the database connections) is therefore used exactly as before -
 * single-threaded, only not always from the same thread. Requests stay
 * serialized: one VM, one run at a time.
 *
 * What the thread adds is ABANDONMENT. The loop thread waits for a run only
 * until the run's own deadline (its run watch, cap/run_watchdog.h, armed by
 * the run itself) plus a grace period. A run still going then is stuck in a
 * native call that does not look at the stop flag (the watchdog raised it at
 * the deadline: the run trips at its next poll once the call returns). The
 * loop thread then goes into STANDBY without touching anything the VM thread
 * may be using:
 *
 *   - the request that started the run (an HTTP entry on a plaintext
 *     HTTP/1.x connection) is answered 503 at once, written straight to its
 *     socket (the descriptor was captured before the hand-off; nothing in a
 *     handler or middleware entry writes that socket - only an SSE entry
 *     streams, and it is never answered early), and the socket's write side
 *     is shut, so whatever the run later leaves in the response is never seen;
 *   - every NEW connection on the plaintext listener is accepted and answered
 *     503 (Retry-After: 1) and closed - the listener is the only Keel object
 *     standby touches, and only with accept(2), which Keel cannot be doing
 *     since the loop thread is the one that runs Keel;
 *   - connections Keel already holds, timers and async completions wait: the
 *     Keel loop is not run in standby (its callbacks would run Hull code
 *     concurrently with the stuck run).
 *
 * When the native call returns, the VM thread trips at its next poll, the
 * run unwinds and its entry finishes on the VM thread as any failed run does.
 * The loop thread sees the job done, finishes the entry (an HTTP entry
 * answers 503 and closes the connection - its response was already sent, or
 * is sent now on a TLS / HTTP/2 connection) and resumes the Keel loop.
 *
 * If the run stays stuck for the HARD CEILING (counted from abandonment), or
 * the server was asked to stop and the drain timeout has passed, the process
 * exits at once (_exit, status 75 = EX_TEMPFAIL for the ceiling) with one
 * log line: no teardown can run while a thread is inside the VM, and a
 * supervisor restarts the process.
 *
 * Thread-local state: everything a run binds per thread (the db budget
 * binding, the fs charge scope, the SQLite namespace trust, the audit buffer,
 * the stb caps) is bound by the run itself on the thread it runs on, at its
 * arm. The one per-thread value that persists BETWEEN runs - the transaction
 * generation (hl_db_txn_gen) - is handed across with the baton. QuickJS
 * records the C stack it may use, so the runtime's thread_switch hook
 * (JS_UpdateStackTop) runs whenever the VM changes threads.
 *
 * Off (the default) nothing here is created and every entry runs inline,
 * exactly as before.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_VM_THREAD_H
#define HL_CAP_VM_THREAD_H

#include <stddef.h>
#include <stdint.h>

#include "hull/cap/run_watchdog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults (manifest limits.vm_thread_grace_ms / _ceiling_ms, CLI
 * --vm-thread-grace-ms / --vm-thread-ceiling-ms). */
#define HL_VMT_DEFAULT_GRACE_MS    1000u
#define HL_VMT_DEFAULT_CEILING_MS 60000u
#define HL_VMT_MAX_MS             HL_RUN_MAX_MS
/* The exit status when the ceiling ends the process. */
#define HL_VMT_EXIT_STUCK          75
/* The VM thread's stack (virtual; pages are touched as used). QuickJS's own
 * limit (max_stack_bytes) and Lua's C-call limit sit well inside it. */
#define HL_VMT_STACK_BYTES        (64u * 1024u * 1024u)

typedef struct HlVmThread HlVmThread;

typedef struct HlVmThreadConfig {
    HlRunWatch *watch;          /* the VM's run watch: its deadline is the
                                 * abandonment clock (required) */
    uint32_t    grace_ms;       /* 0 = HL_VMT_DEFAULT_GRACE_MS */
    uint32_t    ceiling_ms;     /* 0 = never exit (not recommended) */
    const intptr_t *listen_fd;  /* the plaintext listener answered 503 in
                                 * standby, read when standby starts (Keel
                                 * binds it when the loop starts); NULL =
                                 * none (a TLS listener) */
    const int  *server_running; /* Keel's running / draining flags (atomic),
                                 * read in standby to honour a stop; */
    const int  *server_draining;/* either may be NULL */
    uint32_t    drain_ms;       /* after a stop, how long standby waits */
    /* Called on the thread about to run VM code, whenever the VM changes
     * threads (QuickJS: JS_UpdateStackTop). May be NULL. */
    void      (*thread_switch)(void *ud);
    void       *switch_ud;
} HlVmThreadConfig;

/* Start the VM thread. Call after the kernel sandbox is applied (a thread
 * inherits the seccomp filter and Landlock domain only from its creator).
 * 0 / -1 (logged). */
int  hl_vmt_create(HlVmThread **out, const HlVmThreadConfig *cfg);

/* Stop and join the VM thread. Must be called on the loop thread with no run
 * in progress (always true between Keel callbacks). Hands the transaction
 * generation back to the caller and runs thread_switch for it, so whatever
 * runs VM code afterwards (teardown) does so on the calling thread exactly
 * as without the thread. NULL-safe. */
void hl_vmt_destroy(HlVmThread *t);

/* 1 when the caller is the VM thread. */
int  hl_vmt_on_vm_thread(const HlVmThread *t);

/* What an HTTP entry tells hl_vmt_run, captured on the loop thread BEFORE the
 * hand-off (the response is the VM thread's from then on). */
typedef struct HlVmtHttp {
    int fd;        /* the request's socket, -1 if none */
    int early_ok;  /* 1: plaintext HTTP/1.x, nothing sent yet - a raw 503
                    * may be written to fd when the run is abandoned */
} HlVmtHttp;

/* Outcome of hl_vmt_run. */
#define HL_VMT_DONE       0   /* the run finished normally */
#define HL_VMT_ABANDONED  1   /* the run outlived deadline + grace; it has
                               * finished by the time hl_vmt_run returns */

/* Run fn(ud) as one entry of the VM. On the VM thread itself, or with no VM
 * thread (t NULL), fn runs inline and HL_VMT_DONE is returned. Otherwise the
 * loop thread hands fn to the VM thread and waits (standby once the run is
 * abandoned) until it finishes. @p http may be NULL (not an HTTP entry);
 * *early_sent (may be NULL) is set to 1 when a raw 503 went out on its fd. */
int  hl_vmt_run(HlVmThread *t, void (*fn)(void *), void *ud,
                const HlVmtHttp *http, int *early_sent);

/* Process-wide counters, for tests and diagnostics. */
typedef struct HlVmtStats {
    uint64_t jobs;          /* entries run on the VM thread */
    uint64_t abandoned;     /* runs abandoned */
    uint64_t standby_503;   /* connections answered 503 in standby */
} HlVmtStats;
void hl_vmt_stats(const HlVmThread *t, HlVmtStats *out);

/* Test hook (tests/e2e_vm_thread.sh): a native call that ignores the stop
 * flag. hull._test_native_sleep(ms) (Lua) / hull._testNativeSleep(ms) (JS)
 * exist only when HULL_TEST_NATIVE_SLEEP=1 is in the environment at start;
 * they sleep in C for ms (1..120000) without looking at the run's budget or
 * deadline - the shape of an uncharged engine builtin. Never set it in
 * production: it hands the app a way to hold its run past any deadline. */
int  hl_vmt_test_hook_enabled(void);
void hl_vmt_test_native_sleep(uint32_t ms);
#define HL_VMT_TEST_SLEEP_MAX_MS 120000u

/* Pure helpers, exposed for unit tests. */

/* The deadline (monotonic ms) the loop thread measures a run against: the
 * run watch's deadline when the run has armed the watch since it was handed
 * over (@p armed_this_job: the watch's sequence moved), else
 * @p job_start_ms + @p fallback_ms, the deadline the run's arm will set.
 * 0 = no deadline (abandonment off: --max-run-ms 0). */
uint64_t hl_vmt_effective_deadline(int armed_this_job,
                                   uint64_t watch_deadline_ms,
                                   uint64_t job_start_ms,
                                   uint32_t fallback_ms);

#ifdef __cplusplus
}
#endif

#endif /* HL_CAP_VM_THREAD_H */
