/*
 * commands/build.h — hull build subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_BUILD_H
#define HL_COMMANDS_BUILD_H

#include "hull/commands/dispatch.h"

int hl_cmd_build(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_BUILD_H */
