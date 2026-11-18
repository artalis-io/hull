/**
 * @file commands/plugins.h
 * @brief `hull plugins list|run` - an app's build plugins (docs/buildcontext_design.md §9.2).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_PLUGINS_H
#define HL_COMMANDS_PLUGINS_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_plugins(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_PLUGINS_H */
