/**
 * @file commands/eject.h
 * @brief `hull eject` - emit a standalone build artifact bundle.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_EJECT_H
#define HL_COMMANDS_EJECT_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_eject(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_EJECT_H */
