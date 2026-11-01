/*
 * cap/run_watchdog.h - a wall-clock bound for every script run (audit 12)
 *
 * The instruction budget (runtime/lua/budget.c, the QuickJS interrupt
 * handler) is a precise per-run COST, and it is only as complete as the
 * charging inside each engine: audits 9-12 kept finding work one Lua
 * instruction or one QuickJS step does without charging it (a prototype-
 * chain miss, Proxy ownKeys, a regexp compile, a rehash, a finalizer
 * check, ...). Charging each one never converges, so every run also has a
 * wall-clock deadline: one process-wide thread holds the armed deadlines
 * and, when one passes, raises its watch's stop flag. The engines poll that
 * flag at every instruction (Lua HULL PATCH 0005, in luaG_traceexec) and at
 * every call / backward jump (QuickJS HULL PATCH 0006, in
 * js_poll_interrupts), and the runtime turns it into the same sticky,
 * uncatchable trip the instruction budget raises ("run exceeded its time
 * limit"). So a loop of uncharged operations is bounded by time even where
 * it is not by cost.
 *
 * The watchdog thread never touches a VM: it only writes the stop flag of a
 * watch it holds, under its lock, and a watch is unlinked (under the same
 * lock) before its storage goes. A VM reads the flag with an atomic load.
 *
 * A watch is armed by every entry point of a run (with the instruction
 * budget: hl_lua_budget_arm / hl_js_budget_arm), so the deadline bounds an
 * UNINTERRUPTED run; a run that parks (http.fetch, db.async, a sleep) and is
 * resumed starts a new one. A watch is not disarmed when a run returns: its
 * flag may then be raised while the VM is idle, and the next entry's arm
 * clears it before any script runs - the contract the instruction budget's
 * own leftover state already has. It is disarmed when its VM is freed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_RUN_WATCHDOG_H
#define HL_CAP_RUN_WATCHDOG_H

#include <stdint.h>

/* Default deadline for an uninterrupted run started by the event loop or a
 * worker: a request handler, a middleware, a timer, a WebSocket / SSE
 * callback, an async resume, a task, a test case, a worker.dispatch job.
 * Such a run holds the (single-threaded) event loop for its whole length, and
 * the default instruction budget (100M) is a few seconds of script: a run
 * still going after a minute is stuck in uncharged work, not busy. */
#define HL_RUN_DEFAULT_MS        60000u
/* Default for a run of app.main itself (its first run and each resume): a
 * CLI tool blocks nothing else, and may legitimately spend longer in one
 * stretch (a long synchronous compute.call, a big local SQL statement). */
#define HL_RUN_MAIN_DEFAULT_MS  600000u
/* Default for a run of an HTTP handler (audit convergence §6.2): a request
 * handler or middleware, an SSE endpoint's handler, and each resume of one
 * (a run that parks on http.fetch / db.async starts a new run when it is
 * resumed). 10 s rather than 5 s: a handler doing a few synchronous
 * Postgres queries or a password hash on a loaded host stays well inside
 * it, while one request can no longer hold the event loop for a minute.
 * The app sets its own with manifest `limits.request_ms`. */
#define HL_RUN_HTTP_DEFAULT_MS   10000u
/* Default for a run of a timer, a WebSocket (server or client) callback or
 * an SSE stream callback, and each resume of one. The app sets its own with
 * manifest `limits.callback_ms`. */
#define HL_RUN_CALLBACK_DEFAULT_MS 5000u
/* Longest deadline accepted (a larger --max-run-ms is clamped): 24 hours. */
#define HL_RUN_MAX_MS          86400000u

/* Which default a run gets when none is configured. */
typedef enum {
    HL_RUN_ENTRY    = 0,   /* every other run: tasks, test cases, jobs, ... */
    HL_RUN_MAIN     = 1,   /* app.main's first run and its resumes */
    HL_RUN_HTTP     = 2,   /* an HTTP handler / middleware run, or its resume */
    HL_RUN_CALLBACK = 3,   /* a timer / WebSocket / SSE callback, or its resume */
} HlRunKind;

/* One armed deadline. Owned by the VM it bounds (zero-initialised before
 * first use); linked into the watchdog's list while pending. Every field but
 * `stop` is the watchdog's, under its lock. */
typedef struct HlRunWatch {
    struct HlRunWatch *next;
    struct HlRunWatch *prev;
    uint64_t           deadline_ms;   /* monotonic */
    int                armed;         /* deadline_ms is this run's */
    int                linked;        /* pending in the watchdog's list */
    int                stop;          /* raised at the deadline: atomic */
    uint64_t           seq;           /* bumped by every arm / restore /
                                       * disarm (hl_run_watch_peek) */
} HlRunWatch;

/* Process-wide configuration, from --max-run-ms / HULL_MAX_RUN_MS: < 0 the
 * defaults above, 0 no wall-clock bound at all, > 0 that many ms for every
 * run (clamped to HL_RUN_MAX_MS). Read at each arm. */
void     hl_run_watchdog_configure(int64_t ms);
/* The app's own defaults, from its manifest (`limits.request_ms` /
 * `limits.callback_ms`): 0 keeps Hull's default for that kind. An operator's
 * --max-run-ms / HULL_MAX_RUN_MS still overrides every kind. */
void     hl_run_watchdog_set_app_limits(uint32_t request_ms,
                                        uint32_t callback_ms);
/* The deadline a run of @p kind gets (0 = none). */
uint32_t hl_run_watchdog_limit_ms(HlRunKind kind);
/* Parse a --max-run-ms / HULL_MAX_RUN_MS value: a non-negative decimal
 * number of milliseconds. 0 on success with *out set, -1 otherwise. */
int      hl_run_watchdog_parse_ms(const char *s, int64_t *out);

/* Start a run: clear the stop flag and arm @p w with a deadline @p ms from
 * now (0 = no deadline for this run). If the watchdog thread cannot be
 * started the run has no wall-clock bound (logged once). */
void hl_run_watch_arm(HlRunWatch *w, uint32_t ms);

/* Unlink @p w and clear its flag. Once this returns the watchdog never
 * writes to @p w again, so its storage may go. Safe on a zeroed watch. */
void hl_run_watch_disarm(HlRunWatch *w);

/* A nested run (a test request inside a test case, a task run inside another
 * run) arms the watch for itself; the outer run takes its own deadline back
 * afterwards. save: the current deadline (0 = none); restore: put it back -
 * a deadline that has passed raises the flag at once. */
uint64_t hl_run_watch_save(HlRunWatch *w);
void     hl_run_watch_restore(HlRunWatch *w, uint64_t deadline_ms);

/* The current deadline (0 = none armed) and the arm sequence number, read
 * under the watchdog's lock from any thread: --vm-thread's loop thread
 * (shared/vm_thread.c) tells from the sequence whether the run it waits for
 * has armed the watch yet. */
void hl_run_watch_peek(HlRunWatch *w, uint64_t *deadline_ms, uint64_t *seq);

/* ── A blocking native call inside a run (audit convergence §6.2) ──
 *
 * The watchdog's flag is seen between instructions, so ONE blocking native
 * call (a synchronous http.get, a synchronous compute.call) ran to its own
 * timeout - up to 60 s / 1 h - past the run's deadline, holding the event
 * loop. Such a call clamps its timeout to what is left of the run it was made
 * from. The deadline is the one the CALLING THREAD's last arm / restore set
 * (hl_run_watch_arm / _restore record it per thread; the watch's disarm
 * clears it), so a pool thread (compute.async, db.async, SMTP workers) has
 * none and its ops keep their own timeouts.
 *
 * hl_run_deadline_left_ms: ms left, 0 = the deadline has passed, -1 = the
 * thread is in no run with a deadline.
 * hl_run_deadline_clamp_ms: min(@p ms, left) (@p ms 0 = unbounded); 0 =
 * nothing left (refuse the call as timed out); @p ms unchanged when there is
 * no deadline. */
int64_t  hl_run_deadline_left_ms(void);
uint64_t hl_run_deadline_clamp_ms(uint64_t ms);

/* ── Deferred start (audit 43) ─────────────────────────────────────
 *
 * A thread that exists when the phase-2 sandbox is applied keeps the
 * permissions it had before: on native Linux the vendored pledge reaches it
 * (TSYNC, docs/pledge_patches.md), but a cosmo APE's libc pledge / unveil
 * apply to the calling thread only. The runners therefore call
 * hl_run_watchdog_defer() before the app's top level is loaded and
 * hl_run_watchdog_release() once phase 2 is in place, so the watchdog THREAD
 * is born inside the sandbox. In between, an armed deadline is still kept:
 * hl_run_watch_stopped() compares it with the clock itself (every
 * HL_RUN_WATCH_POLL_EVERY calls - the budget hook, the QuickJS interrupt
 * handler and every work / SQL charge call it), so the top level's run stays
 * bounded with no thread. Entry points that never defer (hull test, hull
 * agent, workers) are unchanged. */
void hl_run_watchdog_defer(void);
void hl_run_watchdog_release(void);

extern int hl_run_watchdog_deferred_;   /* atomic; read by the inline below */
/* Deferred mode only: raise @p w's flag when its deadline has passed.
 * Returns the flag. */
int hl_run_watch_poll(HlRunWatch *w);

/* Whether @p w's deadline has passed. Any thread; lock-free except while
 * deferred (one uncontended lock every HL_RUN_WATCH_POLL_EVERY calls). */
static inline int hl_run_watch_stopped(HlRunWatch *w)
{
    if (!w) return 0;
    if (__atomic_load_n(&w->stop, __ATOMIC_RELAXED) != 0) return 1;
    if (__atomic_load_n(&hl_run_watchdog_deferred_, __ATOMIC_RELAXED))
        return hl_run_watch_poll(w);
    return 0;
}

/* The trip's message, shared by both runtimes. */
#define HL_RUN_TIME_LIMIT_MSG "run exceeded its time limit"

#endif /* HL_CAP_RUN_WATCHDOG_H */
