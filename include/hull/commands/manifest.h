/*
 * commands/manifest.h — hull manifest subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_MANIFEST_H
#define HL_COMMANDS_MANIFEST_H

#include "hull/commands/dispatch.h"

int hl_cmd_manifest(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_MANIFEST_H */
