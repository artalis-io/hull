/**
 * @file commands/verify_self.h
 * @brief `hull verify-self`. Verify the running binary against a release manifest.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_VERIFY_SELF_H
#define HL_COMMANDS_VERIFY_SELF_H

#include "hull/commands/dispatch.h"

/** @brief Entry point. Invoked by the command dispatcher. */
int hl_cmd_verify_self(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_VERIFY_SELF_H */
