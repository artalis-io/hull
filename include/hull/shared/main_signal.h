/**
 * @file main_signal.h
 * @brief app.main observes SIGINT / SIGTERM (platform gap 7).
 *
 * Installed by both runners (serve.c and serve_cli.c) around the window in
 * which app.main runs, and removed once main has settled. Before it, no
 * handler was installed while main ran: Ctrl-C killed the process at once,
 * with no teardown and no stdout flush, so a tool could not stop what it had
 * started (hsctl's `pair start` could not stop both ranks).
 *
 * How it works:
 *
 *   1. The handler is async-signal-safe: it records the signal and writes one
 *      byte to a self-pipe. It chains to a previously installed sa_handler
 *      (the TUI's, which restores the terminal) rather than replacing it. A
 *      signal the process ignores (an inherited SIG_IGN) is left ignored.
 *      The self-pipe is made once and kept open for the process lifetime, so
 *      a handler on another thread never writes to a reused descriptor.
 *   2. The pipe's read end is a watcher on the event loop, so the signal is
 *      DELIVERED on the loop thread, as a new run, whichever thread the kernel
 *      handed it to: the runtime's main_interrupt hook runs the app's handler
 *      (JS ctx.signal, Lua ctx.on_interrupt) with the budget, deadline and
 *      stale-transaction guard every entry gets.
 *   3. A GRACE starts at the first delivery (default HL_DEFAULT_DRAIN_TIMEOUT_MS,
 *      the serve loop's drain; --interrupt-grace-ms N, a downgrade flag). If
 *      main settles within it, main's return value is the exit code. If not,
 *      the loop is stopped: the runner tears down in the normal order and the
 *      exit status is 128 + the signal.
 *   4. With no handler registered (the runtime's hook says so), main is not
 *      continued: the loop stops at once and the exit status is 128 + the
 *      signal - differing from before only by the teardown and the flush.
 *   5. A second signal during the grace stops the loop at once (128 + it).
 *      A second signal BEFORE the first was delivered (main in a synchronous
 *      stretch the loop never gets to) resets the default action and
 *      re-raises, so a stuck tool still dies on the second Ctrl-C.
 *
 *   6. main-then-serve (serve.c, main AND handlers): a signal that arrived
 *      while main ran is a stop request, so the runner does not enter the
 *      serve loop even when main returned 0 - it tears down and exits
 *      128 + the signal (main's non-zero code still wins). Before audit 44
 *      the request was lost and the process served until SIGKILL.
 *
 * The main window only: once a full build enters the serve loop, Keel's own
 * handler and drain apply as before.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_MAIN_SIGNAL_H
#define HL_SHARED_MAIN_SIGNAL_H

typedef struct HlAsyncBackendCtx HlAsyncBackendCtx;
typedef struct HlRuntime         HlRuntime;

/* Install the SIGINT / SIGTERM handlers and the loop watcher. @p grace_ms is
 * how long main may go on after the first delivered signal (0: none - the
 * loop stops right after the handler ran). One installation per process at a
 * time. 0 on success; -1 (nothing installed, logged) when the pipe or the
 * watcher could not be made - main then runs as before this module. */
int  hl_main_signal_install(HlAsyncBackendCtx *loop, HlRuntime *rt, int grace_ms);

/* Restore the previous handlers, drop the watcher and the grace timer. Safe
 * when nothing is installed. Call right after run_main returns, before the
 * loop is used for anything else (serve.c's main-then-serve). A handler is
 * restored only while ours is still the current one: one installed after it
 * (the TUI's, still acquired) is left in place, and ours, still in its chain,
 * then only passes signals on to what it replaced (audit 44). */
void hl_main_signal_remove(void);

/* The signal (SIGINT / SIGTERM) that arrived while the last installation was
 * active - delivered to the app or not - else 0. Read after remove: serve.c
 * does not enter the serve loop after main when it is set. */
int  hl_main_signal_received(void);

/* After the loop stopped: the signal that ENDED main - the grace expired, a
 * second signal arrived, or the app had no handler - else 0. The runner's
 * exit status is then 128 + it; main's own result is not read. Only the loop
 * thread sets it. */
int  hl_main_signal_ended(void);

/* "SIGINT" / "SIGTERM" / "SIGNAL". */
const char *hl_main_signal_name(int sig);

/* Parse an --interrupt-grace-ms value: a non-negative decimal number of
 * milliseconds, clamped to 24 h. 0 on success, -1 otherwise. */
int  hl_main_signal_parse_ms(const char *s, int *out);

#endif /* HL_SHARED_MAIN_SIGNAL_H */
