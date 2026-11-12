/**
 * @file commands/types.h
 * @brief `hull types` - install the TypeScript declarations into an app.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_TYPES_H
#define HL_COMMANDS_TYPES_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_types(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_TYPES_H */
