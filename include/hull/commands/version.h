/**
 * @file commands/version.h
 * @brief `hull version` - print hull version, runtime, platform.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_VERSION_H
#define HL_COMMANDS_VERSION_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_version(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_VERSION_H */
