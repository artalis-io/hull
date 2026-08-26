/**
 * @file commands/tools.h
 * @brief `hull tools <verb>` subcommand dispatcher.
 *
 * Verbs:
 *   - `hull tools list [--json]`     - print registry + install state
 *   - `hull tools install <name>`    - download + verify + install
 *   - `hull tools install --all`     - install every published tool
 *   - `hull tools uninstall <name>`  - remove an installed tool
 *
 * Trust chain is identical to `hull update`: every tool is listed in
 * the same `hull.sha256` manifest, signed by the same Ed25519 release
 * key. See docs/tools_install.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_TOOLS_H
#define HL_COMMANDS_TOOLS_H

#include "hull/commands/dispatch.h"

int hl_cmd_tools(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_TOOLS_H */
