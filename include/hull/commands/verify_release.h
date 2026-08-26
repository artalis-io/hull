/**
 * @file commands/verify_release.h
 * @brief `hull verify-release` - verify an Ed25519 release-manifest signature.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_VERIFY_RELEASE_H
#define HL_COMMANDS_VERIFY_RELEASE_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_verify_release(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_VERIFY_RELEASE_H */
