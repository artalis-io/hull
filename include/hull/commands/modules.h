/**
 * @file commands/modules.h
 * @brief `hull modules` - inspect the first-party module registry and
 *        the modules an app declares.
 *
 * Subcommands:
 *   - `hull modules list [APP_DIR]`       - what the app declares
 *   - `hull modules available`            - full registry
 *   - `hull modules explain <NAME>`       - spec for one module
 *
 * Add `--json` (a global flag) to any of them for machine-readable
 * output. With no subcommand, prints usage.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_MODULES_H
#define HL_COMMANDS_MODULES_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_modules(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_MODULES_H */
