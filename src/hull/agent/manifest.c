/*
 * agent/manifest.c — `hull agent manifest`: emit the effective manifest
 * as JSON after extraction + normalisation.
 *
 * Different from `hull manifest` which prints a content hash. This is the
 * structured form that an agent can read to know exactly which capabilities
 * the sandbox will enforce.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/manifest.h"
#include "hull/runtime.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif

#include <sh_json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit_string_array(ShJsonWriter *w, const char *key,
                              const char *const *items, int count)
{
    sh_json_write_key(w, key);
    sh_json_write_array_start(w);
    for (int i = 0; i < count; i++)
        sh_json_write_string(w, items[i] ? items[i] : "");
    sh_json_write_array_end(w);
}

/* Counter callback for enumerate_routes / enumerate_middleware. */
static void count_route_cb(void *user, const char *method, const char *pattern)
{
    (void)method; (void)pattern;
    (*(int *)user)++;
}
static void count_mw_cb(void *user, const char *method, const char *pattern,
                        const char *phase)
{
    (void)method; (void)pattern; (void)phase;
    (*(int *)user)++;
}

/* Detect dispatch mode from what the app registered. Mutually
 * exclusive at registration time (mod_app.c gates), so seeing both
 * `main` and any HTTP-side registration is impossible — the loader
 * would have errored. The "unknown" branch fires for apps that
 * register nothing (e.g. just declared a manifest). */
static const char *detect_mode(HlRuntime *rt,
                               int *out_routes, int *out_middleware)
{
    int routes = 0, middleware = 0;
    if (rt->vt->enumerate_routes)
        rt->vt->enumerate_routes(rt, count_route_cb, &routes);
    if (rt->vt->enumerate_middleware)
        rt->vt->enumerate_middleware(rt, count_mw_cb, &middleware);

    if (out_routes)     *out_routes = routes;
    if (out_middleware) *out_middleware = middleware;

    if (rt->vt->has_main && rt->vt->has_main(rt)) return "cli";
    if (routes > 0 || middleware > 0)             return "server";
    return "unknown";
}

static int emit_manifest_json(const HlManifest *m, ShJsonBuf *out,
                              const char *runtime_name,
                              const char *mode,
                              int routes_count, int middleware_count,
                              int main_registered)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    sh_json_write_kv_bool(&w, "declared", m->present ? true : false);
    if (runtime_name)
        sh_json_write_kv_string(&w, "runtime", runtime_name);

    /* Dispatch mode — derived from what the app registered. Useful
     * for tooling that wants to know whether to introspect routes or
     * run main without parsing source. */
    if (mode)
        sh_json_write_kv_string(&w, "mode", mode);
    sh_json_write_kv_bool(&w, "main_registered",
                          main_registered ? true : false);
    sh_json_write_kv_int(&w, "routes_registered", routes_count);
    sh_json_write_kv_int(&w, "middleware_registered", middleware_count);

    /* fs */
    sh_json_write_key(&w, "fs");
    sh_json_write_object_start(&w);
    emit_string_array(&w, "read",  m->fs_read,  m->fs_read_count);
    emit_string_array(&w, "write", m->fs_write, m->fs_write_count);
    sh_json_write_object_end(&w);

    /* env, hosts */
    emit_string_array(&w, "env",   m->env,   m->env_count);
    emit_string_array(&w, "hosts", m->hosts, m->hosts_count);

    /* csp */
    if (m->csp_set && m->csp)
        sh_json_write_kv_string(&w, "csp", m->csp);

    /* cors */
    if (m->cors_set) {
        sh_json_write_key(&w, "cors");
        sh_json_write_object_start(&w);
        emit_string_array(&w, "origins", m->cors_origins, m->cors_origin_count);
        if (m->cors_methods) sh_json_write_kv_string(&w, "methods", m->cors_methods);
        if (m->cors_headers) sh_json_write_kv_string(&w, "headers", m->cors_headers);
        sh_json_write_kv_bool(&w, "credentials", m->cors_credentials ? true : false);
        if (m->cors_max_age > 0)
            sh_json_write_kv_int(&w, "max_age", m->cors_max_age);
        sh_json_write_object_end(&w);
    }

    /* wasm caps (only if any are set) */
    if (m->wasm_heap || m->wasm_stack || m->wasm_gas ||
        m->wasm_max_input || m->wasm_max_output) {
        sh_json_write_key(&w, "wasm");
        sh_json_write_object_start(&w);
        if (m->wasm_heap)        sh_json_write_kv_int(&w, "heap", m->wasm_heap);
        if (m->wasm_stack)       sh_json_write_kv_int(&w, "stack", m->wasm_stack);
        if (m->wasm_gas)         sh_json_write_kv_int(&w, "gas", m->wasm_gas);
        if (m->wasm_max_input)   sh_json_write_kv_int(&w, "max_input", m->wasm_max_input);
        if (m->wasm_max_output)  sh_json_write_kv_int(&w, "max_output", m->wasm_max_output);
        sh_json_write_object_end(&w);
    }

    /* feature flags */
    if (m->gpu) {
        sh_json_write_key(&w, "gpu");
        if (m->gpu_device_count > 0) {
            sh_json_write_object_start(&w);
            sh_json_write_key(&w, "devices");
            sh_json_write_array_start(&w);
            for (int i = 0; i < m->gpu_device_count; i++)
                sh_json_write_int(&w, m->gpu_devices[i]);
            sh_json_write_array_end(&w);
            sh_json_write_object_end(&w);
        } else {
            sh_json_write_bool(&w, true);
        }
    }
    if (m->compute)
        sh_json_write_kv_bool(&w, "compute", true);

    /* modules — first-party stdlib declarations */
    if (m->modules_declared) {
        sh_json_write_key(&w, "modules");
        sh_json_write_object_start(&w);
        for (int i = 0; i < m->modules_count; i++) {
            char vstr[8];
            snprintf(vstr, sizeof(vstr), "%d", (int)m->modules[i].api_major);
            sh_json_write_kv_string(&w, m->modules[i].name, vstr);
        }
        sh_json_write_object_end(&w);
    }

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_manifest_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt)
        return hl_agent_write_error(out, "no runtime");

    HlManifest m = {0};
    int rc = -1;
    const char *runtime_name = NULL;

#ifdef HL_ENABLE_LUA
    if (rt->vt == &hl_lua_vtable) {
        runtime_name = "lua";
        rc = rt->vt->extract_manifest(rt, &m);
    }
#endif
#ifdef HL_ENABLE_JS
    if (rt->vt == &hl_js_vtable) {
        runtime_name = "js";
        rc = rt->vt->extract_manifest(rt, &m);
    }
#endif

    if (rc != 0) {
        /* No manifest declared — still emit an empty manifest object so
         * the agent gets predictable JSON shape. */
    }

    int routes = 0, middleware = 0;
    const char *mode = detect_mode(rt, &routes, &middleware);
    int main_registered = (rt->vt->has_main && rt->vt->has_main(rt)) ? 1 : 0;

    int ret = emit_manifest_json(&m, out, runtime_name,
                                  mode, routes, middleware, main_registered);
    hl_manifest_free(&m);
    return ret;
}

int hl_agent_manifest(const char *app_dir, ShJsonBuf *out)
{
    if (!app_dir)
        return hl_agent_write_error(out, "app_dir required");

    HlAppContextOpts opts = { .app_dir = app_dir };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "failed to load app");

    int rc = hl_agent_manifest_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}
