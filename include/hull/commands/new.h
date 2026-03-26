/*
 * commands/new.h — hull new subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_NEW_H
#define HL_COMMANDS_NEW_H

#include "hull/commands/dispatch.h"

int hl_cmd_new(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_NEW_H */
