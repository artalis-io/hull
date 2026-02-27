/*
 * commands/inspect.c — hull inspect subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.inspect module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/inspect.h"
#include "hull/tool.h"

int hl_cmd_inspect(int argc, char **argv, const char *hull_exe)
{
    return hull_tool("hull.inspect", argc, argv, hull_exe);
}
