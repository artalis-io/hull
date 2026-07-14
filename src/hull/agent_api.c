/*
 * agent_api.c — Diagnostic HTTP endpoints for AI agents
 *
 * Registers /_hull/agent/ pre-body middleware when --agent-api is enabled.
 * Each endpoint delegates to the shared agent library (agent_lib.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/agent_api.h"
#include "hull/agent_lib.h"

#include <keel/server.h>
#include <keel/request.h>
#include <keel/response.h>

#include <string.h>

/* ── Middleware handler ─────────────────────────────────────────────── */

static int agent_api_middleware(KlRequest *req, KlResponse *res, void *user_data)
{
    HlAgentApiCtx *ctx = (HlAgentApiCtx *)user_data;
    const char *path = kl_request_path(req);

    /* Strip /_hull/agent/ prefix */
    static const char prefix[] = "/_hull/agent/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return 0; /* continue */

    const char *endpoint = path + sizeof(prefix) - 1;

    ShJsonBuf out;
    sh_json_buf_init(&out);

    if (strcmp(endpoint, "routes") == 0) {
        hl_agent_routes(ctx->app_dir, &out);
#ifdef HL_ENABLE_SQLITE
    } else if (strcmp(endpoint, "schema") == 0) {
        hl_agent_db_schema(ctx->app_dir, ctx->db_path, &out);
#endif
    } else if (strcmp(endpoint, "status") == 0) {
        hl_agent_status(ctx->app_dir, 0, &out);
    } else if (strcmp(endpoint, "errors") == 0) {
        hl_agent_errors(ctx->app_dir, &out);
#ifdef HL_ENABLE_SQLITE
    } else if (strcmp(endpoint, "migrate") == 0) {
        hl_agent_migrate_status(ctx->app_dir, ctx->db_path, &out);
#endif
    } else {
        sh_json_buf_free(&out);
        return 0; /* not our endpoint, continue */
    }

    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/json");
    if (out.buf)
        kl_response_body_copy(res, out.buf, out.len);
    sh_json_buf_free(&out);
    return 1; /* short-circuit */
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_agent_api_register(KlServer *server, HlAgentApiCtx *ctx)
{
    kl_server_use(server, "GET", "/_hull/agent/*",
                  agent_api_middleware, ctx);
    return 0;
}
