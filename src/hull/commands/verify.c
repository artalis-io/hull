/*
 * commands/verify.c — hull verify subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.verify module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/verify.h"
#include "hull/tool.h"

int hl_cmd_verify(int argc, char **argv, const char *hull_exe)
{
    return hull_tool("hull.verify", argc, argv, hull_exe);
}
