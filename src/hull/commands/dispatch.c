/*
 * commands/dispatch.c — Table-driven subcommand dispatcher
 *
 * Central command table + dispatch function. Adding a new subcommand
 * means adding one line to the table and one .c/.h file.
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
#include "hull/commands/sign_platform.h"

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
    { NULL, NULL }  /* sentinel */
};

/* ── Public API ────────────────────────────────────────────────────── */

int hl_command_dispatch(int argc, char **argv)
{
    if (argc < 2)
        return -1;

    const char *name = argv[1];
    for (const HlCommand *cmd = commands; cmd->name; cmd++) {
        if (strcmp(name, cmd->name) == 0)
            return cmd->handler(argc - 1, argv + 1, argv[0]);
    }

    return -1;
}
