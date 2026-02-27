/*
 * commands/sign_platform.c — hull sign-platform subcommand
 *
 * Thin wrapper: launches the Lua tool VM with hull.sign_platform module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/sign_platform.h"
#include "hull/tool.h"

int hl_cmd_sign_platform(int argc, char **argv, const char *hull_exe)
{
    return hull_tool("hull.sign_platform", argc, argv, hull_exe);
}
