/**
 * @file commands/sign_release.h
 * @brief `hull sign-release` — sign a release manifest (hull.sha256).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_SIGN_RELEASE_H
#define HL_COMMANDS_SIGN_RELEASE_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_sign_release(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_SIGN_RELEASE_H */
