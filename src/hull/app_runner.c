/*
 * app_runner.c - the slim produced-app entry point.
 *
 * A binary from `hull build` links hl_app_run() (via templates/app_main.c)
 * instead of hull_main(). hl_app_run runs the app through hull_serve and
 * references none of the hull dev CLI: no command table, no subcommand
 * handlers, no agent introspection. Because agent/eval.c pulls both runtimes'
 * eval (it must, so `hull agent eval` works on either), keeping the CLI out of
 * a produced app is what lets a single-runtime app stay slim - it references
 * only hull_serve -> app_context -> the composed runtime.
 *
 * The hull toolchain itself keeps the full hull_main dispatcher; only produced
 * apps use this slim trampoline.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/serve.h"

int hl_app_run(int argc, char **argv)
{
    return hull_serve(argc, argv);
}
