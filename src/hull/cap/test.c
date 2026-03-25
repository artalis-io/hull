/*
 * cap/test.c — Shared test dispatch logic
 *
 * In-process HTTP dispatch (no TCP) for testing Hull apps.
 * Routes are matched via kl_router_match, handlers called in-process,
 * and KlResponse fields inspected directly.
 *
 * Runtime-specific bindings live in test_lua.c and test_js.c.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared C dispatch logic ───────────────────────────────────────── */

int hl_cap_test_dispatch(KlRouter *router, const char *method,
                         const char *path, const char *body_data,
                         size_t body_len, const char **header_names,
                         const char **header_values, int num_headers,
                         const char *ctx_json, HlAllocator *hl_alloc,
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

    /* Match route */
    KlRoute *matched = NULL;
    KlParam params[KL_MAX_PARAMS];
    int num_params = 0;

    int match_status = kl_router_match(router, method, strlen(method),
                                        path, path_len,
                                        &matched, params, &num_params);

    if (match_status != 200 || !matched) {
        result->status = match_status;
        return 0;
    }

    /* Build request */
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

    /* Copy matched params */
    req.num_params = num_params;
    for (int i = 0; i < num_params && i < KL_MAX_PARAMS; i++)
        req.params[i] = params[i];

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
     * The runtime dispatcher frees req.ctx using a tagged-allocation layout:
     *   [size_t: alloc_sz][char[alloc_sz]: json\0]
     * with req->ctx pointing past the size prefix. Allocate through the
     * same tracked allocator the runtime uses for balanced accounting. */
    if (ctx_json) {
        size_t json_len = strlen(ctx_json);
        size_t alloc_sz = json_len + 1;
        size_t *block = hl_alloc_malloc(hl_alloc, sizeof(size_t) + alloc_sz);
        if (block) {
            block[0] = alloc_sz;
            memcpy(block + 1, ctx_json, alloc_sz);
            req.ctx = (char *)(block + 1);
        }
    }

    /* Build response */
    KlAllocator alloc = kl_allocator_default();
    KlResponse res;
    if (kl_response_init(&res, &alloc) != 0) return -1;
    res.conn_fd = -1; /* no actual connection */

    /* Dispatch handler */
    matched->handler(&req, &res, matched->user_data);

    /* Extract results */
    result->status = res.status;
    result->body = res.body;
    result->body_len = res.body_len;
    result->hdr_buf = res.hdr_buf;
    result->hdr_len = res.hdr_len;

    /* Copy body and headers before freeing response (kl_response_free
     * frees hdr_buf). Body points into runtime-managed memory. */
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
