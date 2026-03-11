/*
 * http_async.c — Non-blocking HTTP client (thin wrapper over Keel)
 *
 * Checks host allowlist and audits, then delegates to kl_client_start()
 * for the actual async I/O. On completion, bridges back to Hull's
 * HlAsyncCtx/HlAsyncCont layer for runtime-agnostic continuation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http_async.h"
#include "hull/cap/audit.h"
#include "hull/alloc.h"

#include <keel/allocator.h>
#include <keel/server.h>
#include <keel/url.h>

#include <string.h>

#include "log.h"

/* ── Static default allocator (outlives all async clients) ────────── */

static KlAllocator s_kl_alloc;
static int s_kl_alloc_inited;

static KlAllocator *get_kl_alloc(void)
{
    if (!s_kl_alloc_inited) {
        s_kl_alloc = kl_allocator_default();
        s_kl_alloc_inited = 1;
    }
    return &s_kl_alloc;
}

/* ── Driver cleanup ──────────────────────────────────────────────── */

static void free_keel_client(void *driver)
{
    if (!driver)
        return;
    KlClient *client = driver;
    kl_client_cancel(client);
    kl_client_free(client);
}

/* ── KlClient on_done callback ───────────────────────────────────── */

static void on_keel_client_done(KlClient *client, void *user_data)
{
    HlAsyncCtx *ctx = user_data;

    if (kl_client_error(client) == 0) {
        /* Success — set driver for resume */
        ctx->driver = client;

        /* Audit success */
        const KlClientResponse *resp = kl_client_response(client);
        ShJsonWriter w = hl_audit_begin("http.fetch");
        if (resp)
            sh_json_write_kv_int(&w, "status", resp->status);
        sh_json_write_kv_int(&w, "result", 0);
        hl_audit_end(&w);
    } else {
        /* Error — free client, set driver NULL */
        ctx->driver = NULL;
        kl_client_free(client);

        /* Audit failure */
        ShJsonWriter w = hl_audit_begin("http.fetch");
        sh_json_write_kv_int(&w, "result", -1);
        hl_audit_end(&w);
    }

    kl_async_complete(ctx->server, &ctx->op);
}

/* ── Deadline timeout ────────────────────────────────────────────── */

static void on_http_deadline(KlAsyncOp *op, void *user_data)
{
    HlAsyncCtx *ctx = user_data;
    (void)op;

    log_debug("[hull:c] http.fetch timeout");

    /* Cancel + free the KlClient */
    KlClient *client = ctx->driver;
    if (client) {
        kl_client_cancel(client);
        kl_client_free(client);
    }
    ctx->driver = NULL;

    /* Complete — on_resume fires with driver=NULL */
    kl_async_complete(ctx->server, &ctx->op);
}

/* ── Public API ──────────────────────────────────────────────────── */

HlAsyncCtx *hl_async_http_start(KlServer *server, KlConn *conn,
                                  HlAllocator *alloc,
                                  HlHttpConfig *http_cfg,
                                  const char *method, const char *url,
                                  const HlHttpHeader *headers, int num_headers,
                                  const char *body, size_t body_len)
{
    if (!server || !conn || !http_cfg || !method || !url)
        return NULL;

    /* Parse URL for allowlist check */
    KlUrl parsed;
    if (kl_url_parse(url, &parsed) != 0)
        return NULL;

    /* Host allowlist */
    if (hl_http_check_host(http_cfg, parsed.host, parsed.host_len) != 0) {
        ShJsonWriter w = hl_audit_begin("http.fetch");
        sh_json_write_kv_string(&w, "method", method);
        sh_json_write_kv_string(&w, "url", url);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return NULL;
    }

    int timeout_ms = http_cfg->timeout_ms > 0 ? http_cfg->timeout_ms
                                                : KL_CLIENT_DEFAULT_TIMEOUT_MS;

    /* Construct KlClientConfig */
    KlClientConfig kl_cfg = {
        .timeout_ms        = http_cfg->timeout_ms,
        .max_response_size = http_cfg->max_response_size,
        .tls               = http_cfg->tls,
    };

    /* HTTPS requires TLS; plain HTTP must not use TLS */
    if (parsed.is_https && !kl_cfg.tls)
        return NULL;
    if (!parsed.is_https)
        kl_cfg.tls = NULL;

    /* Create async context */
    HlAsyncCtx *ctx = hl_async_ctx_create(server, alloc);
    if (!ctx)
        return NULL;

    ctx->free_driver = free_keel_client;
    ctx->op.deadline_ms = kl_monotonic_ms() + (uint64_t)timeout_ms;
    ctx->op.on_deadline = on_http_deadline;

    /* Start Keel async client */
    KlClient *kl_client = kl_client_start(
        &server->ev, get_kl_alloc(), &kl_cfg,
        method, url,
        (const KlClientHeader *)headers, num_headers,
        body, body_len,
        on_keel_client_done, ctx);

    if (!kl_client) {
        hl_async_ctx_free(ctx);
        return NULL;
    }
    ctx->driver = kl_client;

    /* Suspend the inbound connection */
    if (kl_async_suspend(server, conn, &ctx->op) < 0) {
        kl_client_cancel(kl_client);
        kl_client_free(kl_client);
        ctx->driver = NULL;
        hl_async_ctx_free(ctx);
        return NULL;
    }

    return ctx;
}
