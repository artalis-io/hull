/*
 * commands/version.h — hull version subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_VERSION_H
#define HL_COMMANDS_VERSION_H

#include "hull/commands/dispatch.h"

int hl_cmd_version(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_VERSION_H */
