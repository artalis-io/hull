/*
 * commands/verify.h — hull verify subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_VERIFY_H
#define HL_COMMANDS_VERIFY_H

#include "hull/commands/dispatch.h"

int hl_cmd_verify(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_VERIFY_H */
