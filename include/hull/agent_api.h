/**
 * @file agent_api.h
 * @brief HTTP transport for the agent introspection library.
 *
 * Mounts `/_hull/agent/...` endpoints on a Keel server when the
 * `--agent-api` flag is enabled. Each endpoint delegates to the shared
 * functions in `agent_lib.h`, so the CLI, MCP, and HTTP surfaces all
 * return identical JSON.
 *
 * Only intended for dev mode: by default the endpoints are
 * disabled - production builds should not expose `/_hull/agent/...`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_AGENT_API_H
#define HL_AGENT_API_H

#include <keel/http_server.h>

/* Context passed to the middleware */
typedef struct {
    const char *app_dir;
    const char *db_path;
} HlAgentApiCtx;

/*
 * Register /_hull/agent/ routes on the server.
 * Call from main.c when --agent-api flag is set.
 */
int hl_agent_api_register(KlHttpServer *server, HlAgentApiCtx *ctx);

/*
 * 1 when the Host header value (@p len bytes, not NUL-terminated) names
 * loopback: localhost, 127.0.0.1 or [::1], each with an optional port.
 * Every agent API request must carry one, and none may carry an Origin
 * (DNS rebinding: a page on an attacker's name resolved to 127.0.0.1).
 */
int hl_agent_api_host_ok(const char *host, size_t len);

#endif /* HL_AGENT_API_H */
