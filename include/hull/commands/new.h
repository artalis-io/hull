/**
 * @file commands/new.h
 * @brief `hull new` — scaffold a new Hull project from a template.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_NEW_H
#define HL_COMMANDS_NEW_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_new(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_NEW_H */
