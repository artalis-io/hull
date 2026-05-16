/**
 * @file commands/check.h
 * @brief `hull check` — run unit tests + verify the app signature.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_CHECK_H
#define HL_COMMANDS_CHECK_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_check(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_CHECK_H */
