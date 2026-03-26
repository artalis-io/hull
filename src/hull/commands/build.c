/*
 * commands/build.c — hull build subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.build module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/build.h"
#include "hull/tool.h"

int hl_cmd_build(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.build", argc, argv, env->hull_exe);
}
