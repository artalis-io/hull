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
#ifdef HL_ENABLE_HTTP_SERVER
#include "hull/commands/test.h"
#endif
#include "hull/commands/new.h"
#ifdef HL_ENABLE_HTTP_SERVER
#include "hull/commands/dev.h"
#endif
#include "hull/commands/eject.h"
#ifdef HL_ENABLE_DB
#include "hull/commands/migrate.h"
#endif
#include "hull/commands/sign_platform.h"
#ifdef HL_ENABLE_HTTP_SERVER
#include "hull/commands/agent.h"
#include "hull/commands/mcp.h"
#endif
#include "hull/commands/check.h"
#include "hull/commands/compute.h"
#include "hull/commands/deploy.h"
#include "hull/commands/version.h"
#include "hull/commands/doctor.h"
#include "hull/commands/init.h"
#include "hull/commands/modules.h"
#ifdef HL_ENABLE_HTTP_CLIENT
#include "hull/commands/update.h"
#include "hull/commands/tools.h"
#endif
#include "hull/commands/sign_release.h"
#include "hull/commands/verify_release.h"
#include "hull/commands/help.h"

#include <string.h>

/* ── Command table ─────────────────────────────────────────────────── */

static const HlCommand commands[] = {
    { "keygen",   hl_cmd_keygen },
    { "build",    hl_cmd_build },
    { "verify",   hl_cmd_verify },
    { "inspect",  hl_cmd_inspect },
    { "manifest", hl_cmd_manifest },
#ifdef HL_ENABLE_HTTP_SERVER
    { "test",     hl_cmd_test },
#endif
    { "new",      hl_cmd_new },
#ifdef HL_ENABLE_HTTP_SERVER
    { "dev",      hl_cmd_dev },
#endif
    { "eject",         hl_cmd_eject },
    { "sign-platform", hl_cmd_sign_platform },
#ifdef HL_ENABLE_DB
    { "migrate",       hl_cmd_migrate },
#endif
#ifdef HL_ENABLE_HTTP_SERVER
    { "agent",         hl_cmd_agent },
    { "mcp",           hl_cmd_mcp },
#endif
    { "check",         hl_cmd_check },
    { "compute",       hl_cmd_compute },
    { "deploy",        hl_cmd_deploy },
    { "version",       hl_cmd_version },
    { "doctor",        hl_cmd_doctor },
    { "init",          hl_cmd_init },
    { "modules",       hl_cmd_modules },
#ifdef HL_ENABLE_HTTP_CLIENT
    { "update",         hl_cmd_update },
    { "tools",          hl_cmd_tools },
#endif
    { "sign-release",   hl_cmd_sign_release },
    { "verify-release", hl_cmd_verify_release },
    { "help",           hl_cmd_help },
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
