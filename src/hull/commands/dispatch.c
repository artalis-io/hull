/*
 * commands/dispatch.c — Table-driven subcommand dispatcher
 *
 * Central command table + dispatch function. Adding a new subcommand
 * means adding one line to the table and one .c/.h file.
 *
 * Global flags (--app-dir, --verbose, --json) are parsed before the
 * subcommand name and passed to handlers via HlCommandEnv.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/dispatch.h"
#include "hull/commands/keygen.h"
#include "hull/commands/build.h"
#include "hull/commands/verify.h"
#include "hull/commands/inspect.h"
#include "hull/commands/manifest.h"
#include "hull/commands/test.h"
#include "hull/commands/new.h"
#include "hull/commands/dev.h"
#include "hull/commands/eject.h"
#include "hull/commands/migrate.h"
#include "hull/commands/sign_platform.h"
#include "hull/commands/agent.h"
#include "hull/commands/mcp.h"
#include "hull/commands/check.h"
#include "hull/commands/compute.h"

#include <string.h>

/* ── Command table ─────────────────────────────────────────────────── */

static const HlCommand commands[] = {
    { "keygen",   hl_cmd_keygen },
    { "build",    hl_cmd_build },
    { "verify",   hl_cmd_verify },
    { "inspect",  hl_cmd_inspect },
    { "manifest", hl_cmd_manifest },
    { "test",     hl_cmd_test },
    { "new",      hl_cmd_new },
    { "dev",      hl_cmd_dev },
    { "eject",         hl_cmd_eject },
    { "sign-platform", hl_cmd_sign_platform },
    { "migrate",       hl_cmd_migrate },
    { "agent",         hl_cmd_agent },
    { "mcp",           hl_cmd_mcp },
    { "check",         hl_cmd_check },
    { "compute",       hl_cmd_compute },
    { NULL, NULL }  /* sentinel */
};

/* ── Public API ────────────────────────────────────────────────────── */

int hl_command_dispatch(int argc, char **argv)
{
    if (argc < 2)
        return -1;

    HlCommandEnv env = { .hull_exe = argv[0], .app_dir = "." };

    /* Scan for global flags before the subcommand */
    int cmd_idx = 1;
    while (cmd_idx < argc && argv[cmd_idx][0] == '-') {
        if (strcmp(argv[cmd_idx], "--app-dir") == 0 && cmd_idx + 1 < argc) {
            env.app_dir = argv[++cmd_idx];
        } else if (strncmp(argv[cmd_idx], "--app-dir=", 10) == 0) {
            env.app_dir = argv[cmd_idx] + 10;
        } else if (strcmp(argv[cmd_idx], "--verbose") == 0) {
            env.verbose = 1;
        } else if (strcmp(argv[cmd_idx], "--json") == 0) {
            env.json_output = 1;
        } else {
            break;  /* unknown flag — stop scanning */
        }
        cmd_idx++;
    }

    if (cmd_idx >= argc)
        return -1;

    const char *name = argv[cmd_idx];
    for (const HlCommand *cmd = commands; cmd->name; cmd++) {
        if (strcmp(name, cmd->name) == 0)
            return cmd->handler(argc - cmd_idx, argv + cmd_idx, &env);
    }

    return -1;
}
