/*
 * commands/plugins.c - hull plugins subcommand
 *
 * Thin wrapper: launches the Lua tool VM with the hull.plugins module, which
 * lists the build plugins an app registers (hull.build.json) and runs them
 * over their source selections (docs/buildcontext_design.md §9.2, §13.5).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/plugins.h"
#include "hull/tool.h"

int hl_cmd_plugins(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.plugins", argc, argv, env->hull_exe);
}
