/**
 * @file commands/mcp.h
 * @brief `hull mcp` — Model Context Protocol server for AI agents.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_MCP_H
#define HL_COMMANDS_MCP_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_mcp(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_MCP_H */
