/*
 * commands/analyze.c — hull analyze subcommand
 *
 * Thin wrapper: launches the Lua tool VM with the hull.source.analyze module, which
 * parses the app's Lua source via hull.source.lua and reports diagnostics (see
 * docs/hull_analyze_design.md). No HTTP/DB/WASM -- present in every build flavor.
 *
 * Note: the module is hull.source.analyze, NOT hull.analyze -- the latter already
 * backs `hull modules analyze` (import-vs-manifest declaration analysis), a distinct
 * check. This command is source SYNTAX analysis.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/analyze.h"
#include "hull/tool.h"

int hl_cmd_analyze(int argc, char **argv, const HlCommandEnv *env)
{
    return hull_tool("hull.source.analyze", argc, argv, env->hull_exe);
}
