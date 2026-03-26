/*
 * commands/compute.c — hull compute subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.compute module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/compute.h"
#include "hull/tool.h"

int hl_cmd_compute(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.compute", argc, argv, env->hull_exe);
}
