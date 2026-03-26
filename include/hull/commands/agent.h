/*
 * commands/agent.h — hull agent subcommand (AI-native development tooling)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_AGENT_H
#define HL_COMMANDS_AGENT_H

#include "hull/commands/dispatch.h"

int hl_cmd_agent(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_AGENT_H */
