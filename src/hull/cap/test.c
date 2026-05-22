/*
 * cap/test.c — in-process synthetic-request harness for hull test
 *
 * Builds a fully-formed KlRequest from the runtime-side test bindings
 * (method, path, headers, body, opaque JSON ctx), hands it to Keel's
 * router pipeline via `kl_router_dispatch_synthetic`, and copies the
 * resulting status / body / headers into an HlTestResult that the
 * Lua/JS runtime sides can inspect.
 *
 * Routing semantics (match → pre-body middleware → post-body
 * middleware → handler) live in Keel — this file deliberately does
 * not duplicate that sequence so it cannot drift away from the
 * network-driven dispatch in vendor/keel/src/connection.c and h2.c.
 *
 * Runtime-specific bindings live in runtime/{lua,js}/mod_test.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/test.h"

#include <keel/router.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/allocator.h>
#include <keel/body_reader.h>

#include "hull/alloc.h"
#include "hull/reqctx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hl_cap_test_dispatch(KlRouter *router, const char *method,
                         const char *path, const char *body_data,
                         size_t body_len, const char **header_names,
                         const char **header_values, int num_headers,
                         const char *ctx_json, HlAllocator *hl_alloc,
                         int run_middleware,
                         HlTestResult *result)
{
    if (!router || !method || !path || !result) return -1;

    memset(result, 0, sizeof(*result));

    /* Split path at '?' for query string */
    const char *query = NULL;
    size_t path_len = strlen(path);
    size_t query_len = 0;
    const char *qmark = strchr(path, '?');
    if (qmark) {
        path_len = (size_t)(qmark - path);
        query = qmark + 1;
        query_len = strlen(query);
    }

    /* Build request — params are filled in by dispatch_synthetic. */
    KlRequest req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.method_len = strlen(method);
    req.path = path;
    req.path_len = path_len;
    req.query = query;
    req.query_len = query_len;
    req.version_major = 1;
    req.version_minor = 1;
    req.keep_alive = 0;

    /* Set headers — lowercase names to match llhttp parser behavior */
    char lowered_names[KL_MAX_HEADERS][64];
    for (int i = 0; i < num_headers && i < KL_MAX_HEADERS; i++) {
        size_t nlen = strlen(header_names[i]);
        if (nlen >= sizeof(lowered_names[0])) continue; /* skip oversized */
        for (size_t j = 0; j < nlen; j++) {
            char c = header_names[i][j];
            lowered_names[i][j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        lowered_names[i][nlen] = '\0';
        req.headers[i].name = lowered_names[i];
        req.headers[i].name_len = nlen;
        req.headers[i].value = header_values[i];
        req.headers[i].value_len = strlen(header_values[i]);
    }
    req.num_headers = num_headers;

    /* Fake body reader if body provided — must be KlBufReader so that
     * hl_cap_body_data() can cast and read .data/.len fields. */
    KlBufReader fake_buf;
    memset(&fake_buf, 0, sizeof(fake_buf));
    if (body_data && body_len > 0) {
        fake_buf.data = (char *)body_data;
        fake_buf.len = body_len;
        fake_buf.cap = body_len;
        req.body_reader = &fake_buf.base;
        req.content_length = body_len;
    }

    /* Inject context if provided (parsed as JSON by Lua/JS bindings).
     * Allocate an HlReqCtx with kind=JSON so the runtime can parse it.
     * The runtime dispatcher frees req.ctx via HlReqCtx kind dispatch. */
    if (ctx_json) {
        size_t json_len = strlen(ctx_json);
        HlReqCtx *rctx = hl_alloc_malloc(hl_alloc, sizeof(HlReqCtx));
        if (rctx) {
            rctx->kind = HL_REQCTX_JSON;
            rctx->json.data = hl_alloc_malloc(hl_alloc, json_len + 1);
            if (rctx->json.data) {
                memcpy(rctx->json.data, ctx_json, json_len + 1);
                rctx->json.len = json_len;
                req.ctx = rctx;
            } else {
                hl_alloc_free(hl_alloc, rctx, sizeof(HlReqCtx));
            }
        }
    }

    /* Build response, then hand the (req, res) pair to Keel's router
     * pipeline. dispatch_synthetic encapsulates the match → pre-body
     * mw → post-body mw → handler sequence so this file no longer
     * needs to mirror Keel internals. */
    KlAllocator alloc = kl_allocator_default();
    KlResponse res;
    if (kl_response_init(&res, &alloc) != 0) return -1;
    res.conn_fd = -1; /* no actual connection */

    (void)kl_router_dispatch_synthetic(router, &req, &res, run_middleware);

    /* Extract results — copy body and headers into hl_alloc-owned
     * storage before freeing the response (kl_response_free releases
     * hdr_buf, and body may live in runtime-managed memory). */
    result->status = res.status;
    result->body = res.body;
    result->body_len = res.body_len;
    result->hdr_buf = res.hdr_buf;
    result->hdr_len = res.hdr_len;

    if (res.body && res.body_len > 0) {
        char *body_copy = hl_alloc_malloc(hl_alloc, res.body_len + 1);
        if (body_copy) {
            memcpy(body_copy, res.body, res.body_len);
            body_copy[res.body_len] = '\0';
            result->body = body_copy;
        }
    }
    if (res.hdr_buf && res.hdr_len > 0) {
        char *hdr_copy = hl_alloc_malloc(hl_alloc, res.hdr_len + 1);
        if (hdr_copy) {
            memcpy(hdr_copy, res.hdr_buf, res.hdr_len);
            hdr_copy[res.hdr_len] = '\0';
            result->hdr_buf = hdr_copy;
            result->hdr_len = res.hdr_len;
        }
    }

    kl_response_free(&res);
    return 0;
}
