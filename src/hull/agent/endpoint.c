/*
 * agent/endpoint.c — `hull agent endpoint METHOD PATH` and
 * `hull agent middleware METHOD PATH`.
 *
 * Both walk the runtime's enumerate_routes / enumerate_middleware
 * vtable hooks, filter by a Keel-style pattern match, and emit JSON.
 * Together they let an agent preview what would happen for a request
 * without actually issuing it.
 *
 * Pattern matching mirrors Keel's router (see vendor/keel/src/router.c):
 *   "*"            — match any method
 *   "/foo"         — exact path
 *   "/foo/*"       — prefix match (everything under /foo/)
 *   "/users/:id"   — parameter capture (matches /users/anything-no-slash)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/runtime.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif

#include <sh_json.h>
#include <string.h>

/* Returns 1 if method matches the pattern's method spec. */
static int method_matches(const char *pattern_method, const char *req_method)
{
    if (!pattern_method || !req_method) return 0;
    if (pattern_method[0] == '*' && pattern_method[1] == '\0') return 1;
    return strcasecmp(pattern_method, req_method) == 0;
}

/* Returns 1 if path matches the pattern. Handles `*`, `/*`, `:param`. */
static int path_matches(const char *pattern, const char *path)
{
    if (!pattern || !path) return 0;

    const char *p = pattern;
    const char *r = path;

    while (*p && *r) {
        if (p[0] == '*' && p[1] == '\0') {
            return 1;  /* trailing wildcard matches the rest */
        }
        if (*p == ':') {
            /* Skip the parameter name in the pattern */
            while (*p && *p != '/') p++;
            /* Skip the captured segment in the path. L-4 fix: require at
             * least one byte — empty segments (e.g. /users/) should NOT
             * match /users/:id, matching Keel router semantics. */
            const char *r_seg_start = r;
            while (*r && *r != '/') r++;
            if (r == r_seg_start) return 0;
            continue;
        }
        if (*p != *r) return 0;
        p++;
        r++;
    }

    /* Both consumed exactly */
    if (*p == '\0' && *r == '\0') return 1;
    /* Pattern ends in trailing slash + wildcard */
    if (p[0] == '*' && p[1] == '\0') return 1;
    return 0;
}

/* ── enumerate-callback state ──────────────────────────────────────── */

typedef struct {
    ShJsonWriter *w;
    const char   *method;
    const char   *path;
    int           matched_count;
    int           include_routes;
    int           include_middleware;
} EndpointState;

static void route_cb(void *user, const char *m, const char *p)
{
    EndpointState *s = user;
    if (!s->include_routes) return;
    if (!method_matches(m, s->method)) return;
    if (!path_matches(p, s->path)) return;
    sh_json_write_object_start(s->w);
    sh_json_write_kv_string(s->w, "method",  m ? m : "");
    sh_json_write_kv_string(s->w, "pattern", p ? p : "");
    sh_json_write_kv_string(s->w, "kind",    "route");
    sh_json_write_object_end(s->w);
    s->matched_count++;
}

static void middleware_cb(void *user, const char *m, const char *p,
                          const char *phase)
{
    EndpointState *s = user;
    if (!s->include_middleware) return;
    if (!method_matches(m, s->method)) return;
    if (!path_matches(p, s->path)) return;
    sh_json_write_object_start(s->w);
    sh_json_write_kv_string(s->w, "method",  m ? m : "");
    sh_json_write_kv_string(s->w, "pattern", p ? p : "");
    sh_json_write_kv_string(s->w, "phase",   phase ? phase : "");
    sh_json_write_kv_string(s->w, "kind",    "middleware");
    sh_json_write_object_end(s->w);
    s->matched_count++;
}

/* ── public API ────────────────────────────────────────────────────── */

int hl_agent_endpoint_ctx(HlAppContext *ctx, const char *method,
                          const char *path, ShJsonBuf *out)
{
    if (!method || !path)
        return hl_agent_write_error(out, "method and path required");

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) return hl_agent_write_error(out, "no runtime");

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "method", method);
    sh_json_write_kv_string(&w, "path",   path);

    /* Middleware first — they fire before the handler. */
    EndpointState ms = { .w = &w, .method = method, .path = path,
                         .include_middleware = 1 };
    sh_json_write_key(&w, "middleware");
    sh_json_write_array_start(&w);
    if (rt->vt->enumerate_middleware)
        rt->vt->enumerate_middleware(rt, middleware_cb, &ms);
    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "middleware_count", ms.matched_count);

    /* Then the matched routes. */
    EndpointState rs = { .w = &w, .method = method, .path = path,
                         .include_routes = 1 };
    sh_json_write_key(&w, "routes");
    sh_json_write_array_start(&w);
    if (rt->vt->enumerate_routes)
        rt->vt->enumerate_routes(rt, route_cb, &rs);
    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "route_count", rs.matched_count);

    sh_json_write_kv_bool(&w, "would_match", rs.matched_count > 0);

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_endpoint(const char *app_dir, const char *method,
                      const char *path, ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";
    HlAppContextOpts opts = { .app_dir = app_dir };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "failed to load app");
    int rc = hl_agent_endpoint_ctx(ctx, method, path, out);
    hl_app_context_free(ctx);
    return rc;
}

int hl_agent_middleware_ctx(HlAppContext *ctx, const char *method,
                            const char *path, ShJsonBuf *out)
{
    if (!method || !path)
        return hl_agent_write_error(out, "method and path required");

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) return hl_agent_write_error(out, "no runtime");

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "method", method);
    sh_json_write_kv_string(&w, "path",   path);

    EndpointState ms = { .w = &w, .method = method, .path = path,
                         .include_middleware = 1 };
    sh_json_write_key(&w, "middleware");
    sh_json_write_array_start(&w);
    if (rt->vt->enumerate_middleware)
        rt->vt->enumerate_middleware(rt, middleware_cb, &ms);
    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "count", ms.matched_count);

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_middleware(const char *app_dir, const char *method,
                        const char *path, ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";
    HlAppContextOpts opts = { .app_dir = app_dir };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "failed to load app");
    int rc = hl_agent_middleware_ctx(ctx, method, path, out);
    hl_app_context_free(ctx);
    return rc;
}
