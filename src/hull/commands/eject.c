/*
 * commands/eject.c — hull eject subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.eject module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/eject.h"
#include "hull/tool.h"

int hl_cmd_eject(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.eject", argc, argv, env->hull_exe);
}
