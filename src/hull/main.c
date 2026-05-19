/*
 * main.c — Hull's `hull_main` entry-point dispatcher
 *
 * This file is intentionally small: it owns the top-level subcommand
 * dispatch (--version, `run` alias, registered subcommands, fall-through
 * to `hull_serve`). The full server/CLI lifecycle lives in serve.c so
 * future HL_ENABLE_HTTP=0 builds can swap in a Keel-free counterpart
 * without touching this file.
 *
 * Adding a new subcommand:
 *   1. New .c/.h under src/hull/commands/
 *   2. One line in src/hull/commands/dispatch.c's command table
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/dispatch.h"
#include "hull/commands/version.h"
#include "hull/serve.h"

#include <string.h>

/* Fall-through entry. With HL_ENABLE_HTTP=1 this drives serve.c
 * (which branches on app.main vs routes). With HL_ENABLE_HTTP=0 the
 * same hull_serve symbol is exported by serve_cli.c — load app,
 * invoke app.main, exit. Apps that register HTTP routes on an
 * HL_ENABLE_HTTP=0 build fail at module-resolve time (the modules
 * they need require HL_ENABLE_HTTP build cap). */

int hull_main(int argc, char **argv)
{
    /* Handle -v / --version before subcommand dispatch so that
     * `hull --version` and `hull -v` work as conventional global flags. */
    if (argc >= 2 &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        char *ver_argv[] = { "version" };
        HlCommandEnv env = { .hull_exe = argv[0], .app_dir = "." };
        return hl_cmd_version(1, ver_argv, &env);
    }

    /* `hull run [args]` is a discoverable alias for `hull [args]` —
     * both go through hull_serve, which branches on whether the loaded
     * app registered app.main (CLI mode) or routes (server mode). */
    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        for (int i = 1; i < argc - 1; i++) argv[i] = argv[i + 1];
        argc--;
        argv[argc] = NULL;
        return hull_serve(argc, argv);
    }

    int rc = hl_command_dispatch(argc, argv);
    if (rc != -1)
        return rc;

    return hull_serve(argc, argv);
}
