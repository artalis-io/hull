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
/* Longest deadline accepted (a larger --max-run-ms is clamped): 24 hours. */
#define HL_RUN_MAX_MS          86400000u

/* Which default a run gets when none is configured. */
typedef enum {
    HL_RUN_ENTRY = 0,   /* every run but app.main's own */
    HL_RUN_MAIN  = 1,   /* app.main's first run and its resumes */
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
} HlRunWatch;

/* Process-wide configuration, from --max-run-ms / HULL_MAX_RUN_MS: < 0 the
 * defaults above, 0 no wall-clock bound at all, > 0 that many ms for every
 * run (clamped to HL_RUN_MAX_MS). Read at each arm. */
void     hl_run_watchdog_configure(int64_t ms);
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

/* Whether @p w's deadline has passed. Any thread; lock-free. */
static inline int hl_run_watch_stopped(const HlRunWatch *w)
{
    return w && __atomic_load_n(&w->stop, __ATOMIC_RELAXED) != 0;
}

/* The trip's message, shared by both runtimes. */
#define HL_RUN_TIME_LIMIT_MSG "run exceeded its time limit"

#endif /* HL_CAP_RUN_WATCHDOG_H */
