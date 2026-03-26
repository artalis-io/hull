/*
 * commands/mcp.h — hull mcp serve (MCP stdio server)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_MCP_H
#define HL_COMMANDS_MCP_H

#include "hull/commands/dispatch.h"

int hl_cmd_mcp(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_MCP_H */
