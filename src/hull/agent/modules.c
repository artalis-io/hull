/*
 * agent/modules.c - `hull agent modules`: emit declared + intrinsic
 * module set and the build's capability matrix as JSON.
 *
 * Sibling to `hull agent manifest` and `hull agent capabilities`, but
 * focused on the first-party module-declaration surface. Agents use
 * this to discover what an app imports vs. what the build can supply.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/manifest.h"
#include "hull/module_registry.h"
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

static int emit_modules_json(const HlManifest *m, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    /* declared - verbatim from the manifest's modules table */
    sh_json_write_key(&w, "declared");
    sh_json_write_object_start(&w);
    if (m && m->modules_declared) {
        for (int i = 0; i < m->modules_count; i++) {
            char vstr[8];
            snprintf(vstr, sizeof(vstr), "%d", (int)m->modules[i].api_major);
            sh_json_write_kv_string(&w, m->modules[i].name, vstr);
        }
    }
    sh_json_write_object_end(&w);

    /* intrinsic - registry entries always admitted regardless of manifest */
    sh_json_write_key(&w, "intrinsic");
    sh_json_write_array_start(&w);
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    for (size_t i = 0; i < total; i++)
        if (all[i].intrinsic)
            sh_json_write_string(&w, all[i].name);
    sh_json_write_array_end(&w);

    /* build_caps - which compile-time subsystems are linked in */
    sh_json_write_key(&w, "build_caps");
    sh_json_write_object_start(&w);
#ifdef HL_ENABLE_DB
    sh_json_write_kv_bool(&w, "db", true);
#else
    sh_json_write_kv_bool(&w, "db", false);
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
    sh_json_write_object_end(&w);

    sh_json_write_kv_int(&w, "registry_count", (int)total);

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_modules_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt)
        return hl_agent_write_error(out, "no runtime");

    HlManifest m = {0};
#ifdef HL_ENABLE_LUA
    if (rt->vt->kind == HL_RT_LUA)
        rt->vt->extract_manifest(rt, &m);
#endif
#ifdef HL_ENABLE_JS
    if (rt->vt->kind == HL_RT_JS)
        rt->vt->extract_manifest(rt, &m);
#endif

    int rc = emit_modules_json(&m, out);
    hl_manifest_free(&m);
    return rc;
}

int hl_agent_modules(const char *app_dir, ShJsonBuf *out)
{
    if (!app_dir)
        return hl_agent_write_error(out, "app_dir required");

    HlAppContextOpts opts = { .app_dir = app_dir };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "failed to load app");

    int rc = hl_agent_modules_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}
