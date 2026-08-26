/*
 * commands/deploy.c - hull deploy subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.deploy module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/deploy.h"
#include "hull/tool.h"

int hl_cmd_deploy(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.deploy", argc, argv, env->hull_exe);
}
