/*
 * commands/init.h — hull init subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_INIT_H
#define HL_COMMANDS_INIT_H

#include "hull/commands/dispatch.h"

int hl_cmd_init(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_INIT_H */
