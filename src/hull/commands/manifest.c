/*
 * commands/manifest.c — hull manifest subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.manifest module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/manifest.h"
#include "hull/tool.h"

int hl_cmd_manifest(int argc, char **argv, const char *hull_exe)
{
    return hull_tool("hull.manifest", argc, argv, hull_exe);
}
