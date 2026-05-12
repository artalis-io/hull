/*
 * commands/init.c — hull init subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.init module.
 * Unlike `hull new`, init works in-place and is idempotent.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/init.h"
#include "hull/tool.h"

int hl_cmd_init(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.init", argc, argv, env->hull_exe);
}
