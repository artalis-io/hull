/*
 * agent/overview.c - `hull agent overview` - single-shot project summary.
 *
 * Composite summary an agent dropped into an unfamiliar app dir can
 * read in one call: runtime, manifest presence, route stats, compute
 * modules + AOT readiness + wamrc state, GPU shaders, migrations,
 * declared modules, tests, build readiness.
 *
 * Intentionally cheap - no manifest parse beyond what `hl_app_context_init`
 * already does, no DB connection (skip "pending migrations" - agents
 * can call `hull agent migrate` for that), no shape inference (leave
 * the agent to draw conclusions from the data).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/manifest.h"
#include "hull/runtime.h"
#include "hull/tools_install.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif

#include <sh_json.h>

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Counters wired to the runtime vtable's enumerate_* callbacks ── */

typedef struct {
    int total;
    int has_get, has_post, has_put, has_delete, has_patch, has_other;
    int has_ws, has_sse;
} RouteStats;

static void route_count_cb(void *user, const char *method, const char *pattern)
{
    RouteStats *s = user;
    (void)pattern;
    s->total++;
    if      (method && strcmp(method, "GET") == 0)    s->has_get = 1;
    else if (method && strcmp(method, "POST") == 0)   s->has_post = 1;
    else if (method && strcmp(method, "PUT") == 0)    s->has_put = 1;
    else if (method && strcmp(method, "DELETE") == 0) s->has_delete = 1;
    else if (method && strcmp(method, "PATCH") == 0)  s->has_patch = 1;
    else if (method && strcmp(method, "WS") == 0)     s->has_ws = 1;
    else if (method && strcmp(method, "SSE") == 0)    s->has_sse = 1;
    else                                              s->has_other = 1;
}

/* ── Filesystem counters (compute, gpu, migrations, tests) ────── */

/* Count entries in `dir` whose name passes `match`. Returns 0 if the
 * directory doesn't exist (treated as empty). */
typedef int (*name_match_fn)(const char *name);

static int count_matching(const char *dir, name_match_fn match)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (match(e->d_name)) n++;
    }
    closedir(d);
    return n;
}

static int has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    return nl > el && strcmp(name + nl - el, ext) == 0;
}

static int is_wasm(const char *n)     { return has_ext(n, ".wasm"); }
static int is_aot(const char *n)      { return strstr(n, ".aot.") != NULL; }
static int is_wgsl(const char *n)     { return has_ext(n, ".wgsl"); }
static int is_sql(const char *n)      { return has_ext(n, ".sql"); }
static int is_test_file(const char *n)
{
    /* tests/test_*.{c,lua,js} or *_test.{lua,js} */
    if (strncmp(n, "test_", 5) == 0 &&
        (has_ext(n, ".lua") || has_ext(n, ".js") || has_ext(n, ".c")))
        return 1;
    if (has_ext(n, "_test.lua") || has_ext(n, "_test.js")) return 1;
    return 0;
}

/* Count .wasm modules AND check whether at least one has a matching
 * .aot.<arch> sibling. Encoded together so we walk the dir once. */
static void scan_compute(const char *app_dir, int *out_count, int *out_any_aot)
{
    *out_count = 0;
    *out_any_aot = 0;
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/compute", app_dir);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return;

    DIR *d = opendir(dir);
    if (!d) return;

    /* Two passes: enumerate names into a small array, then cross-check. */
    char *names[256];
    int nnames = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nnames < 256) {
        if (e->d_name[0] == '.') continue;
        names[nnames] = strdup(e->d_name);
        if (!names[nnames]) break;
        nnames++;
    }
    closedir(d);

    int wasm_count = 0;
    int any_aot = 0;
    for (int i = 0; i < nnames; i++) {
        const char *ni = names[i];
        if (!is_wasm(ni)) continue;
        wasm_count++;
        /* Skip the ".wasm" extension to derive base; look for any
         * matching base.aot.* sibling. */
        size_t base_len = strlen(ni) - 5;
        for (int j = 0; j < nnames; j++) {
            if (i == j) continue;
            const char *nj = names[j];
            size_t nj_len = strlen(nj);
            if (nj_len <= base_len + 5) continue;
            if (strncmp(nj, ni, base_len) != 0) continue;
            if (strncmp(nj + base_len, ".aot.", 5) != 0) continue;
            any_aot = 1;
            break;
        }
    }
    for (int i = 0; i < nnames; i++) free(names[i]);

    *out_count = wasm_count;
    *out_any_aot = any_aot;
    (void)is_aot;  /* silence unused-static via the typedef-only path */
}

/* ── Manifest module list helpers ──────────────────────────────── */

static void emit_modules_list(ShJsonWriter *w, const HlManifest *m)
{
    sh_json_write_array_start(w);
    if (m && m->modules_declared) {
        for (int i = 0; i < m->modules_count; i++) {
            char spec[64];
            snprintf(spec, sizeof(spec), "%s@%d",
                     m->modules[i].name,
                     (int)m->modules[i].api_major);
            sh_json_write_string(w, spec);
        }
    }
    sh_json_write_array_end(w);
}

/* ── Main entry point ─────────────────────────────────────────── */

int hl_agent_overview_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    const char *app_dir = hl_app_context_app_dir(ctx);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    sh_json_write_kv_string(&w, "app_dir", app_dir ? app_dir : ".");

    /* Runtime + manifest presence (cheap, from filesystem). */
    char app_lua[PATH_MAX], app_js[PATH_MAX];
    snprintf(app_lua, sizeof(app_lua), "%s/app.lua", app_dir);
    snprintf(app_js,  sizeof(app_js),  "%s/app.js",  app_dir);
    struct stat st;
    int has_lua = (stat(app_lua, &st) == 0);
    int has_js  = (stat(app_js,  &st) == 0);
    sh_json_write_kv_string(&w, "runtime",
        hl_app_context_is_lua(ctx) ? "lua" :
        has_js ? "js" :
        has_lua ? "lua" : "unknown");

    sh_json_write_kv_bool(&w, "entry_present", has_lua || has_js);

    /* Routes - walk via vtable callbacks. */
    RouteStats rs = {0};
    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (rt && rt->vt && rt->vt->enumerate_routes)
        rt->vt->enumerate_routes(rt, route_count_cb, &rs);

    sh_json_write_key(&w, "routes");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "count", rs.total);
    sh_json_write_key(&w, "methods");
    sh_json_write_array_start(&w);
    if (rs.has_get)    sh_json_write_string(&w, "GET");
    if (rs.has_post)   sh_json_write_string(&w, "POST");
    if (rs.has_put)    sh_json_write_string(&w, "PUT");
    if (rs.has_delete) sh_json_write_string(&w, "DELETE");
    if (rs.has_patch)  sh_json_write_string(&w, "PATCH");
    if (rs.has_other)  sh_json_write_string(&w, "OTHER");
    sh_json_write_array_end(&w);
    sh_json_write_kv_bool(&w, "has_ws",  rs.has_ws != 0);
    sh_json_write_kv_bool(&w, "has_sse", rs.has_sse != 0);
    sh_json_write_object_end(&w);

    /* Compute modules + AOT readiness + wamrc state. */
    int compute_count = 0, compute_any_aot = 0;
    scan_compute(app_dir, &compute_count, &compute_any_aot);
    char wamrc_path[PATH_MAX];
    int wamrc_found = (hl_tools_lookup_path("wamrc", NULL,
                                            wamrc_path, sizeof(wamrc_path)) == 0);
    sh_json_write_key(&w, "compute_modules");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "count", compute_count);
    sh_json_write_kv_bool(&w, "any_aot", compute_any_aot != 0);
    sh_json_write_kv_bool(&w, "wamrc_installed", wamrc_found != 0);
    sh_json_write_object_end(&w);

    /* GPU shaders. */
    char shaders_dir[PATH_MAX];
    snprintf(shaders_dir, sizeof(shaders_dir), "%s/shaders", app_dir);
    int gpu_count = count_matching(shaders_dir, is_wgsl);
    sh_json_write_key(&w, "gpu_shaders");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "count", gpu_count);
    sh_json_write_object_end(&w);

    /* Migrations - count files only. Pending count requires DB
     * connection - agents needing that call `hull agent migrate`. */
    char migrations_dir[PATH_MAX];
    snprintf(migrations_dir, sizeof(migrations_dir), "%s/migrations", app_dir);
    int migrations_count = count_matching(migrations_dir, is_sql);
    sh_json_write_key(&w, "migrations");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "count", migrations_count);
    sh_json_write_object_end(&w);

    /* Modules declared in manifest. */
    HlManifest m = {0};
    int have_manifest = 0;
#ifdef HL_ENABLE_LUA
    if (rt && rt->vt && rt->vt->kind == HL_RT_LUA && rt->vt->extract_manifest) {
        rt->vt->extract_manifest(rt, &m);
        have_manifest = 1;
    }
#endif
#ifdef HL_ENABLE_JS
    if (rt && rt->vt && rt->vt->kind == HL_RT_JS && rt->vt->extract_manifest) {
        rt->vt->extract_manifest(rt, &m);
        have_manifest = 1;
    }
#endif
    sh_json_write_kv_bool(&w, "manifest_present", have_manifest && m.modules_declared);
    sh_json_write_key(&w, "modules_declared");
    emit_modules_list(&w, have_manifest ? &m : NULL);
    if (have_manifest) hl_manifest_free(&m);

    /* Tests. */
    char tests_dir[PATH_MAX];
    snprintf(tests_dir, sizeof(tests_dir), "%s/tests", app_dir);
    int tests_count = count_matching(tests_dir, is_test_file);
    sh_json_write_key(&w, "tests");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "files", tests_count);
    sh_json_write_object_end(&w);

    /* Build readiness heuristic: entry + manifest + nothing else
     * blocking. Compute modules without wamrc count as "ready" since
     * hull build falls back to interpreter; missing wamrc surfaces as
     * a hint in the compute_modules block. */
    int build_ready = (has_lua || has_js) && have_manifest;
    sh_json_write_kv_bool(&w, "build_ready", build_ready != 0);

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_overview(const char *app_dir, ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";

    HlAppContextOpts opts = { .app_dir = app_dir };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "failed to load app");

    int rc = hl_agent_overview_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}
