/*
 * agent/perf.c — `hull agent perf`: runtime statistics snapshot.
 *
 * Emits configuration limits + a brief feature-flag summary. For live
 * stats while a server is running, prefer the dev-server's /ready
 * endpoint (see hull.web.middleware.health) which exposes connection
 * counts and uptime.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/app_context.h"
#include "hull/runtime.h"
#include "hull/utils/limits.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif

#include <sh_json.h>
#include <stdlib.h>

int hl_agent_perf_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    HlRuntime *rt = hl_app_context_runtime(ctx);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    /* Runtime identity */
    if (rt && rt->vt) {
        const char *name = rt->vt->name ? rt->vt->name : "unknown";
#ifdef HL_ENABLE_LUA
        if (rt->vt->kind == HL_RT_LUA) name = "lua";
#endif
#ifdef HL_ENABLE_JS
        if (rt->vt->kind == HL_RT_JS) name = "js";
#endif
        sh_json_write_kv_string(&w, "runtime", name);
    }

    /* Feature flags compiled in */
    sh_json_write_key(&w, "features");
    sh_json_write_object_start(&w);
#ifdef HL_ENABLE_LUA
    sh_json_write_kv_bool(&w, "lua", true);
#else
    sh_json_write_kv_bool(&w, "lua", false);
#endif
#ifdef HL_ENABLE_JS
    sh_json_write_kv_bool(&w, "js", true);
#else
    sh_json_write_kv_bool(&w, "js", false);
#endif
#ifdef HL_ENABLE_WASM
    sh_json_write_kv_bool(&w, "wasm", true);
#else
    sh_json_write_kv_bool(&w, "wasm", false);
#endif
#ifdef HL_ENABLE_GPU
    sh_json_write_kv_bool(&w, "gpu", true);
#else
    sh_json_write_kv_bool(&w, "gpu", false);
#endif
#ifdef HL_ENABLE_DB
    sh_json_write_kv_bool(&w, "db", true);
#else
    sh_json_write_kv_bool(&w, "db", false);
#endif
#ifdef HL_ENABLE_TCC
    sh_json_write_kv_bool(&w, "tcc", true);
#else
    sh_json_write_kv_bool(&w, "tcc", false);
#endif
    sh_json_write_object_end(&w);

    /* Compile-time defaults — agent can compare against runtime override
     * flags reported in dev.json. */
    sh_json_write_key(&w, "limits");
    sh_json_write_object_start(&w);
#ifdef HL_ENABLE_LUA
    sh_json_write_kv_int(&w, "lua_default_heap_bytes",   HL_LUA_DEFAULT_HEAP);
#endif
#ifdef HL_ENABLE_JS
    sh_json_write_kv_int(&w, "js_default_heap_bytes",    HL_JS_DEFAULT_HEAP);
    sh_json_write_kv_int(&w, "js_default_stack_bytes",   HL_JS_DEFAULT_STACK);
#endif
    sh_json_write_kv_int(&w, "default_instructions_per_request",
                              HL_DEFAULT_INSTRUCTIONS);
    sh_json_write_kv_int(&w, "default_port",            HL_DEFAULT_PORT);
    sh_json_write_kv_int(&w, "default_max_connections", HL_DEFAULT_MAX_CONN);
    sh_json_write_kv_int(&w, "default_read_timeout_ms", HL_DEFAULT_READ_TIMEOUT_MS);
    sh_json_write_object_end(&w);

    /* Tip pointing the agent at /ready if it needs live stats */
    sh_json_write_kv_string(&w, "live_stats_hint",
        "for live connection counts and uptime, GET /ready against the dev "
        "server (requires hull.web.middleware.health to be installed).");

    sh_json_write_object_end(&w);
    return 0;
}
