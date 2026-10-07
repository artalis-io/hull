/*
 * agent_api.c - Diagnostic HTTP endpoints for AI agents
 *
 * Registers /_hull/agent/ pre-body middleware when --agent-api is enabled.
 * Each endpoint delegates to the shared agent library (agent_lib.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/agent_api.h"
#include "hull/agent_lib.h"

#include <keel/http_server.h>
#include <keel/http_request.h>
#include <keel/http_response.h>

#include <string.h>
#include <strings.h>  /* strncasecmp */

/* ── Request origin ─────────────────────────────────────────────────── */

/* The listener is loopback-only, but a web page the developer opens can
 * still reach it through DNS rebinding: a name the attacker controls that
 * re-resolves to 127.0.0.1 makes the browser send the request - with the
 * attacker's name in Host - and let the page read the JSON (app paths,
 * schema, errors). So the Host must name loopback itself, and a request a
 * browser marks as coming from a page (Origin) is refused (audit 9). */
int hl_agent_api_host_ok(const char *host, size_t len)
{
    if (!host) return 0;
    static const char *const names[] = { "localhost", "127.0.0.1", "[::1]" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t n = strlen(names[i]);
        if (len < n || strncasecmp(host, names[i], n) != 0) continue;
        if (len == n) return 1;
        if (host[n] != ':' || len == n + 1 || len > n + 6) continue;
        size_t k = n + 1;
        while (k < len && host[k] >= '0' && host[k] <= '9') k++;
        if (k == len) return 1;
    }
    return 0;
}

static int agent_api_refuse(KlHttpResponse *res)
{
    static const char body[] =
        "{\"error\":\"agent API requests must come from a local client "
        "(Host localhost / 127.0.0.1 / [::1], no Origin)\"}";
    kl_http_response_status(res, 403);
    kl_http_response_header(res, "Content-Type", "application/json");
    kl_http_response_body_copy(res, body, sizeof body - 1);
    return 1;
}

/* ── Middleware handler ─────────────────────────────────────────────── */

static int agent_api_middleware(KlHttpRequest *req, KlHttpResponse *res, void *user_data)
{
    HlAgentApiCtx *ctx = (HlAgentApiCtx *)user_data;
    const char *path = req->path;

    /* Strip /_hull/agent/ prefix */
    static const char prefix[] = "/_hull/agent/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return 0; /* continue */

    size_t host_len = 0;
    const char *host = kl_http_request_header_len(req, "Host", &host_len);
    if (!hl_agent_api_host_ok(host, host_len) ||
        kl_http_request_header(req, "Origin") != NULL)
        return agent_api_refuse(res);

    const char *endpoint = path + sizeof(prefix) - 1;

    ShJsonBuf out;
    sh_json_buf_init(&out);

    if (strcmp(endpoint, "routes") == 0) {
        hl_agent_routes(ctx->app_dir, &out);
#ifdef HL_ENABLE_DB
    } else if (strcmp(endpoint, "schema") == 0) {
        hl_agent_db_schema(ctx->app_dir, ctx->db_path, &out);
#endif
    } else if (strcmp(endpoint, "status") == 0) {
        hl_agent_status(ctx->app_dir, 0, &out);
    } else if (strcmp(endpoint, "errors") == 0) {
        hl_agent_errors(ctx->app_dir, &out);
#ifdef HL_ENABLE_DB
    } else if (strcmp(endpoint, "migrate") == 0) {
        hl_agent_migrate_status(ctx->app_dir, ctx->db_path, &out);
#endif
    } else {
        sh_json_buf_free(&out);
        return 0; /* not our endpoint, continue */
    }

    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    if (out.buf)
        kl_http_response_body_copy(res, out.buf, out.len);
    sh_json_buf_free(&out);
    return 1; /* short-circuit */
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_agent_api_register(KlHttpServer *server, HlAgentApiCtx *ctx)
{
    kl_http_server_use(server, "GET", "/_hull/agent/*",
                  agent_api_middleware, ctx);
    return 0;
}
