/*
 * shared/cli_log.h - logging policy for `hull <subcommand>` (CLI mode).
 *
 * Hull has two logging consumers with different audiences:
 *
 *   1. The SERVER (serve.c) - an operator tailing stderr. It installs its own
 *      callback with a timestamp, and already omits file:line outside DEBUG
 *      builds.
 *   2. The CLI (`hull build`, `hull new`, `hull doctor`, ...) - a developer at
 *      a prompt. Nothing configured log.c for this path, so rxi/log.c's
 *      zero-initialized default applied: level LOG_TRACE, quiet false, and the
 *      built-in stderr callback that prints `src/hull/sandbox_tool.c:243:`.
 *      Internal C source coordinates in polished CLI output are noise.
 *
 * This module owns (2). Default policy:
 *
 *   - Hull-internal records (a C __FILE__) are shown only at WARN and above,
 *     with no file:line, prefixed `hull:` so they read as tool output.
 *   - Script records (an app's / stdlib's own log.info, whose source is a
 *     .lua/.js path) keep INFO, so `hull test` and friends still show what the
 *     app logged.
 *   - Records emitted while the BUILD-TIME APP-EVALUATION window is open (see
 *     hl_cli_log_set_app_phase) are suppressed entirely: `hull build` executes
 *     the app entry's top level to capture app.manifest(), and the app's own
 *     logging is an artifact of that mechanism, not something the user asked
 *     `hull build` to print.
 *
 * `--verbose` turns all three off: every level is emitted, file:line is
 * restored, and app-phase records are shown with an explicit `[build-eval]`
 * marker so it is obvious WHY app output appears during a build.
 *
 * Genuine failures are never hidden: WARN/ERROR/FATAL always print, and
 * nothing here touches fprintf(stderr, ...) diagnostics, which is how the
 * commands report real errors.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_CLI_LOG_H
#define HL_SHARED_CLI_LOG_H

/**
 * @brief Install the CLI logging policy. Idempotent.
 *
 * Call once, from the subcommand dispatcher, before the handler runs. NOT
 * called on the `hull <app>` serve path, which configures its own callback.
 *
 * @param verbose non-zero for `--verbose` (full diagnostics + file:line).
 */
void hl_cli_log_init(int verbose);

/** @brief Is the CLI policy in verbose mode? */
int hl_cli_log_verbose(void);

/**
 * @brief Hand logging ownership to the server policy. Irreversible.
 *
 * A subcommand may hand off to `hull_serve` IN-PROCESS - `hull jobs worker`
 * does exactly that (commands/jobs.c). The dispatcher has already installed
 * the CLI policy by then, and `hl_serve_init_logging` adds its OWN callback;
 * rxi/log.c invokes every registered callback and offers no way to remove one,
 * so both would fire and each surviving record would print TWICE, in two
 * different formats.
 *
 * `hl_serve_init_logging` therefore calls this first: the CLI callback goes
 * inert and the server's becomes the single emitter. There is exactly one
 * owner at a time, and the transition only ever runs CLI -> server (the server
 * never hands back), so this is deliberately one-way.
 */
void hl_cli_log_suspend(void);

/** @brief Has logging ownership been handed to the server policy? */
int hl_cli_log_suspended(void);

#ifdef HL_CLI_LOG_TEST_HOOKS
/**
 * @brief TEST ONLY: clear the suspend flag.
 *
 * hl_cli_log_suspend is deliberately one-way in production - the server never
 * hands logging back - which would otherwise let a single test that exercises
 * it silence every test that runs after it (utest does not fix ordering).
 * Compiled in only under -DHL_CLI_LOG_TEST_HOOKS, mirroring the existing
 * HL_RELEASE_IO_TEST_HOOKS convention, so the production build has no way to
 * resume and the invariant stays enforced where it matters.
 */
void hl_cli_log_reset_for_test(void);
#endif

/**
 * @brief Open/close the build-time app-evaluation window.
 *
 * Bracket any execution of USER application code by a build-side tool (today:
 * manifest extraction in `hull build` / `hull eject` / `hull manifest`).
 * Records emitted while open are attributed to the app and suppressed unless
 * --verbose. Safe to call when the policy was never installed (no-op).
 *
 * @param on non-zero to open the window, zero to close it.
 */
void hl_cli_log_set_app_phase(int on);

/** @brief Is the build-time app-evaluation window currently open? */
int hl_cli_log_app_phase(void);

/**
 * @brief Classify a log record's `file` field as Hull-internal C or not.
 *
 * rxi/log.c carries no source-domain field, and it is vendored (not to be
 * modified). C call sites pass `__FILE__`, which in this tree always ends in
 * ".c" or ".h"; script call sites pass a Lua/JS chunk name. Exposed for unit
 * testing.
 *
 * @returns 1 when @p file looks like a Hull C translation unit, else 0.
 */
int hl_cli_log_is_native_source(const char *file);

#endif /* HL_SHARED_CLI_LOG_H */
