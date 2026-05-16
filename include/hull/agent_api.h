/**
 * @file agent_api.h
 * @brief HTTP transport for the agent introspection library.
 *
 * Mounts `/_hull/agent/*` endpoints on a Keel server when the
 * `--agent-api` flag is enabled. Each endpoint delegates to the shared
 * functions in `agent_lib.h`, so the CLI, MCP, and HTTP surfaces all
 * return identical JSON.
 *
 * Only intended for dev mode: by default the endpoints are
 * disabled — production builds should not expose `/_hull/agent/*`.
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
