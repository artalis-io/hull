/**
 * @file commands/agent.h
 * @brief `hull agent` - AI-native introspection subcommand.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_AGENT_H
#define HL_COMMANDS_AGENT_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_agent(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_AGENT_H */
