/**
 * @file commands/keygen.h
 * @brief `hull keygen` - generate an Ed25519 keypair for app signing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_KEYGEN_H
#define HL_COMMANDS_KEYGEN_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_keygen(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_KEYGEN_H */
