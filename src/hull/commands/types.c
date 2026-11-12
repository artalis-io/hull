/*
 * commands/types.c - hull types subcommand
 *
 * Thin wrapper: launches the Lua tool VM with the hull.types module, which
 * installs the embedded TypeScript declarations (tool.types_files()).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/types.h"
#include "hull/tool.h"

int hl_cmd_types(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.types", argc, argv, env->hull_exe);
}
