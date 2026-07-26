/**
 * @file serve.h
 * @brief Entry point for the server/CLI lifecycle.
 *
 * `hull_serve` orchestrates the full app load → manifest extract →
 * sandbox → module-resolve → migrations → dispatch sequence. Whether
 * dispatch ends in `kl_server_run` (server mode) or `run_main` (CLI
 * mode) is decided after manifest extraction based on whether the
 * loaded app registered `app.main` or HTTP routes.
 *
 * Split out of `main.c` so future `HL_ENABLE_HTTP=0` builds can swap
 * in a CLI-only counterpart without touching the dispatcher entry.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SERVE_H
#define HL_SERVE_H

/**
 * @brief Run the full serve lifecycle. Returns the process exit code.
 *
 *   argv: typically the program's argv (with subcommand stripped)
 */
int hull_serve(int argc, char **argv);

/**
 * @brief Slim produced-app entry point.
 *
 * A built app links this instead of `hull_main`: it runs the app via
 * `hull_serve` and pulls none of the `hull` dev CLI (dispatch, subcommands,
 * agent introspection). That keeps a produced single-runtime app from dragging
 * in the command table -> agent -> both runtimes, which is what lets it stay
 * slim (one runtime, zero of the other interpreter). The `hull` toolchain still
 * uses `hull_main` (full dispatch).
 */
int hl_app_run(int argc, char **argv);

#endif /* HL_SERVE_H */
