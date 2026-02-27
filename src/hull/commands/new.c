/*
 * commands/new.c — hull new subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.new module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/new.h"
#include "hull/tool.h"

int hl_cmd_new(int argc, char **argv, const char *hull_exe)
{
    return hull_tool("hull.new", argc, argv, hull_exe);
}
