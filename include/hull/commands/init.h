/**
 * @file commands/init.h
 * @brief `hull init` — initialize a hull project in-place.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_INIT_H
#define HL_COMMANDS_INIT_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_init(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_INIT_H */
