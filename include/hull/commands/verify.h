/**
 * @file commands/verify.h
 * @brief `hull verify` — verify an app's package.sig signatures + file hashes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_VERIFY_H
#define HL_COMMANDS_VERIFY_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_verify(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_VERIFY_H */
