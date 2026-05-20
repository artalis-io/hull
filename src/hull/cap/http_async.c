/*
 * http_async.c — Non-blocking HTTP client (thin wrapper over Keel)
 *
 * Checks host allowlist and audits, then delegates to kl_redirect_start()
 * (or kl_client_start() if redirects disabled) for async I/O. On completion,
 * bridges back to Hull's HlAsyncCtx/HlAsyncCont layer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http_async.h"
#include "hull/cap/audit.h"
#include "hull/alloc.h"
#include "hull/async_backend.h"

#include <keel/allocator.h>
#include <keel/client_pool.h>
#include <keel/redirect.h>
#include <keel/server.h>
#include <keel/url.h>

#include <stdint.h>
#include <string.h>

#include "log.h"

/* ── Tagged pointer convention ───────────────────────────────────── *
 * LSB=1 marks a KlRedirectClient*; LSB=0 marks a KlClient*.
 * Same technique Keel uses for watcher vs connection dispatch.      */

#define TAG_REDIRECT(p) ((void *)((uintptr_t)(p) | 1))
#define UNTAG_REDIRECT(p) ((KlRedirectClient *)((uintptr_t)(p) & ~(uintptr_t)1))
#define IS_REDIRECT(p) ((uintptr_t)(p) & 1)

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

/* ── Driver cleanup (redirect client) ────────────────────────────── */

static void free_redirect_client(void *driver)
{
    if (!driver)
        return;
    KlRedirectClient *rc = UNTAG_REDIRECT(driver);
    kl_redirect_cancel(rc);
    kl_redirect_free(rc);
}

/* ── Driver cleanup (plain client, no redirects) ─────────────────── */

static void free_keel_client(void *driver)
{
    if (!driver)
        return;
    KlClient *client = driver;
    kl_client_cancel(client);
    kl_client_free(client);
}

/* ── KlRedirectClient on_done callback ───────────────────────────── */

static void on_redirect_done(KlRedirectClient *rc, void *user_data)
{
    HlAsyncCtx *ctx = user_data;

    if (kl_redirect_error(rc) == 0) {
        ctx->driver = TAG_REDIRECT(rc);

        const KlClientResponse *resp = kl_redirect_response(rc);
        ShJsonWriter w = hl_audit_begin("http.fetch");
        if (resp)
            sh_json_write_kv_int(&w, "status", resp->status);
        sh_json_write_kv_int(&w, "result", 0);
        hl_audit_end(&w);
    } else {
        KlError err = kl_redirect_last_error(rc);
        ctx->driver = NULL;
        kl_redirect_free(rc);

        ShJsonWriter w = hl_audit_begin("http.fetch");
        sh_json_write_kv_string(&w, "error", kl_strerror(err));
        sh_json_write_kv_int(&w, "result", -1);
        hl_audit_end(&w);
    }

    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        kl_async_complete(ctx->server, &ctx->op);
}

/* ── KlClient on_done callback (no-redirect path) ───────────────── */

static void on_keel_client_done(KlClient *client, void *user_data)
{
    HlAsyncCtx *ctx = user_data;

    if (kl_client_error(client) == 0) {
        ctx->driver = client;

        const KlClientResponse *resp = kl_client_response(client);
        ShJsonWriter w = hl_audit_begin("http.fetch");
        if (resp)
            sh_json_write_kv_int(&w, "status", resp->status);
        sh_json_write_kv_int(&w, "result", 0);
        hl_audit_end(&w);
    } else {
        KlError err = kl_client_last_error(client);
        ctx->driver = NULL;
        kl_client_free(client);

        ShJsonWriter w = hl_audit_begin("http.fetch");
        sh_json_write_kv_string(&w, "error", kl_strerror(err));
        sh_json_write_kv_int(&w, "result", -1);
        hl_audit_end(&w);
    }

    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        kl_async_complete(ctx->server, &ctx->op);
}

/* ── Deadline timeout ────────────────────────────────────────────── */

static void on_http_deadline(KlAsyncOp *op, void *user_data)
{
    HlAsyncCtx *ctx = user_data;
    (void)op;

    log_debug("[hull:c] http.fetch timeout");

    /* free_driver handles both redirect and plain client */
    if (ctx->free_driver && ctx->driver)
        ctx->free_driver(ctx->driver);
    ctx->driver = NULL;

    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
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
    if (!server || !http_cfg || !method || !url)
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
        .decompress        = http_cfg->decompress,
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

    ctx->op.deadline_ms = hl_async_backend()->monotonic_ms() + (uint64_t)timeout_ms;
    ctx->op.on_deadline = on_http_deadline;

    /* Start Keel async client — prefer redirect+pooled path */
    if (http_cfg->follow_redirects) {
        KlRedirectConfig redir = { .max_redirects = http_cfg->max_redirects };

        ctx->free_driver = free_redirect_client;

        KlRedirectClient *rc;
        if (http_cfg->pool)
            rc = kl_redirect_start_pooled(
                http_cfg->pool, &server->ev, get_kl_alloc(), &kl_cfg, &redir,
                method, url,
                (const KlClientHeader *)headers, num_headers,
                body, body_len, on_redirect_done, ctx);
        else
            rc = kl_redirect_start(
                &server->ev, get_kl_alloc(), &kl_cfg, &redir,
                method, url,
                (const KlClientHeader *)headers, num_headers,
                body, body_len, on_redirect_done, ctx);

        if (!rc) {
            hl_async_ctx_free(ctx);
            return NULL;
        }
        ctx->driver = TAG_REDIRECT(rc);
    } else {
        ctx->free_driver = free_keel_client;

        KlClient *kl_client;
        if (http_cfg->pool)
            kl_client = kl_client_start_pooled(
                http_cfg->pool, &server->ev, get_kl_alloc(), &kl_cfg,
                method, url,
                (const KlClientHeader *)headers, num_headers,
                body, body_len, on_keel_client_done, ctx);
        else
            kl_client = kl_client_start(
                &server->ev, get_kl_alloc(), &kl_cfg,
                method, url,
                (const KlClientHeader *)headers, num_headers,
                body, body_len, on_keel_client_done, ctx);

        if (!kl_client) {
            hl_async_ctx_free(ctx);
            return NULL;
        }
        ctx->driver = kl_client;
    }

    if (conn) {
        /* Attached mode: suspend the inbound connection */
        ctx->detached = 0;
        if (kl_async_suspend(server, conn, &ctx->op) < 0) {
            if (ctx->free_driver)
                ctx->free_driver(ctx->driver);
            ctx->driver = NULL;
            hl_async_ctx_free(ctx);
            return NULL;
        }
    } else {
        /* Detached mode (timer callback): no connection to suspend */
        ctx->detached = 1;
    }

    return ctx;
}

/* ── Response extraction (tagged pointer dispatch) ───────────────── */

const KlClientResponse *hl_http_async_response(void *driver)
{
    if (!driver)
        return NULL;
    if (IS_REDIRECT(driver))
        return kl_redirect_response(UNTAG_REDIRECT(driver));
    return kl_client_response((KlClient *)driver);
}
