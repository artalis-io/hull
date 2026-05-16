/**
 * @file commands/sign_platform.h
 * @brief `hull sign-platform` — sign the platform-library layer of package.sig.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_SIGN_PLATFORM_H
#define HL_COMMANDS_SIGN_PLATFORM_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_sign_platform(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_SIGN_PLATFORM_H */
