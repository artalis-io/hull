/**
 * @file commands/manifest.h
 * @brief `hull manifest` — print or hash the app's effective manifest.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_MANIFEST_H
#define HL_COMMANDS_MANIFEST_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_manifest(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_MANIFEST_H */
