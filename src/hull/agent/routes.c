/*
 * agent/routes.c — `hull agent routes` introspection.
 *
 * Walks the runtime's registered routes + middleware via the
 * HlRuntimeVtable enumerate_routes / enumerate_middleware methods
 * and emits JSON.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/app_context.h"
#include "hull/runtime.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif

#include <sh_json.h>


/* ── hl_agent_routes ───────────────────────────────────────────────── */


/* Single-path implementation via HlRuntimeVtable::enumerate_routes /
 * enumerate_middleware. Folds the previous parallel agent_routes_lua +
 * agent_routes_js (~150 lines × 2) into ~30 lines of JSON emission. */

static void emit_route_json(void *user, const char *method, const char *pattern)
{
    ShJsonWriter *w = user;
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "method",  method  ? method  : "");
    sh_json_write_kv_string(w, "pattern", pattern ? pattern : "");
    sh_json_write_object_end(w);
}

static void emit_middleware_json(void *user,
                                 const char *method, const char *pattern,
                                 const char *phase)
{
    ShJsonWriter *w = user;
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "method",  method  ? method  : "");
    sh_json_write_kv_string(w, "pattern", pattern ? pattern : "");
    sh_json_write_kv_string(w, "phase",   phase   ? phase   : "");
    sh_json_write_object_end(w);
}

int hl_agent_routes_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) {
        sh_json_write_object_end(&w);
        return -1;
    }

    sh_json_write_kv_string(&w, "runtime",
        rt->vt == &hl_lua_vtable ? "lua" :
        rt->vt == &hl_js_vtable  ? "js"  : rt->vt->name);

    sh_json_write_key(&w, "routes");
    sh_json_write_array_start(&w);
    if (rt->vt->enumerate_routes)
        rt->vt->enumerate_routes(rt, emit_route_json, &w);
    sh_json_write_array_end(&w);

    sh_json_write_key(&w, "middleware");
    sh_json_write_array_start(&w);
    if (rt->vt->enumerate_middleware)
        rt->vt->enumerate_middleware(rt, emit_middleware_json, &w);
    sh_json_write_array_end(&w);

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_routes(const char *app_dir, ShJsonBuf *out)
{
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = { .app_dir = app_dir };
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "no entry point found");

    int rc = hl_agent_routes_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}

