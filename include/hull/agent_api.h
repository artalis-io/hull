/*
 * agent_api.h — Diagnostic HTTP endpoints for AI agents
 *
 * Registers /_hull/agent/ middleware when --agent-api is enabled.
 * Each endpoint delegates to the shared agent library (agent_lib.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_AGENT_API_H
#define HL_AGENT_API_H

#include <keel/server.h>

/* Context passed to the middleware */
typedef struct {
    const char *app_dir;
    const char *db_path;
} HlAgentApiCtx;

/*
 * Register /_hull/agent/ routes on the server.
 * Call from main.c when --agent-api flag is set.
 */
int hl_agent_api_register(KlServer *server, HlAgentApiCtx *ctx);

#endif /* HL_AGENT_API_H */
