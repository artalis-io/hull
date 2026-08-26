/*
 * agent/deploy.c — `hull agent deploy` — deployment-readiness analysis.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/app_context.h"
#include "hull/cap/tool.h"
#include "hull/manifest.h"
#include "hull/runtime.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "lua.h"
#include "lauxlib.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#include "quickjs.h"
#endif

#include <sh_json.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Count files matching pattern in app_dir/subdir. */
static int count_files_in_dir(const char *app_dir, const char *subdir,
                              const char *pattern)
{
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", app_dir, subdir);

    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;

    char **files = hl_tool_find_files(dir_path, pattern, NULL);
    if (!files) return 0;

    int count = 0;
    for (char **fp = files; *fp; fp++) {
        count++;
        free(*fp);
    }
    free(files);
    return count;
}

int hl_agent_deploy(const char *app_dir, ShJsonBuf *out)
{
    /* Detect runtime */
    char entry_buf[PATH_MAX];
    const char *runtime = NULL;
    const char *entry_point = NULL;

#ifdef HL_ENABLE_LUA
    entry_point = hl_agent_detect_entry(app_dir, "lua", entry_buf, sizeof(entry_buf));
    if (entry_point) runtime = "lua";
#endif
#ifdef HL_ENABLE_JS
    if (!entry_point) {
        entry_point = hl_agent_detect_entry(app_dir, "js", entry_buf, sizeof(entry_buf));
        if (entry_point) runtime = "js";
    }
#endif

    if (!entry_point)
        return hl_agent_write_error(out, "no entry point found (app.lua or app.js)");

    /* Initialize app context to extract manifest. Register the hull.db module
     * (backed by an in-memory DB) rather than running in no_db mode: apps
     * commonly acquire the connection at module top level
     * (`local db = require("hull.db").default()`), which would otherwise fail
     * manifest extraction with "module not found: hull.db" and wrongly report
     * no manifest. no_migrate keeps this read-only introspection from running
     * the app's migrations; db_path stays NULL (-> :memory:), so there is no
     * file side effect and the connection is discarded when the context is
     * freed. */
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = {
        .app_dir = app_dir,
        .entry_point = entry_point,
        .no_db = 0,
        .no_migrate = 1,
        /* Read-only introspection: if the app errors LATE in its top-level code
         * (e.g. does DB work at module scope against the unmigrated :memory:
         * schema), still recover the manifest app.manifest() already stored. */
        .tolerate_load_error = 1,
    };

    HlManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    int has_manifest = 0;

    if (hl_app_context_init(&ctx, &opts) == 0) {
        /* Route manifest extraction through the runtime vtable so this
         * file doesn't peek at HlLua->L / HlJS->ctx internals. */
        HlRuntime *rt = hl_app_context_runtime(ctx);
        if (rt && rt->vt && rt->vt->extract_manifest &&
            rt->vt->extract_manifest(rt, &manifest) == 0)
            has_manifest = 1;
        hl_app_context_free(ctx);
    }

    /* Scan directories */
    int n_migrations = count_files_in_dir(app_dir, "migrations", "*.sql");
    int n_templates  = count_files_in_dir(app_dir, "templates", "*.html");
    int n_static     = count_files_in_dir(app_dir, "static", "*");
    int n_compute    = count_files_in_dir(app_dir, "compute", "*.wasm");
    int n_shaders    = count_files_in_dir(app_dir, "shaders", "*.wgsl");

    /* Check for existing configs */
    char path_buf[PATH_MAX];
    struct stat st;

    snprintf(path_buf, sizeof(path_buf), "%s/Dockerfile", app_dir);
    int has_dockerfile = (stat(path_buf, &st) == 0);

    snprintf(path_buf, sizeof(path_buf), "%s/fly.toml", app_dir);
    int has_fly_toml = (stat(path_buf, &st) == 0);

    snprintf(path_buf, sizeof(path_buf), "%s/deploy", app_dir);
    int has_systemd = 0;
    if (stat(path_buf, &st) == 0 && S_ISDIR(st.st_mode)) {
        char svc_pattern[PATH_MAX];
        snprintf(svc_pattern, sizeof(svc_pattern), "%s/deploy", app_dir);
        char **svc_files = hl_tool_find_files(svc_pattern, "*.service", NULL);
        if (svc_files) {
            has_systemd = (svc_files[0] != NULL);
            for (char **fp = svc_files; *fp; fp++) free(*fp);
            free(svc_files);
        }
    }

    snprintf(path_buf, sizeof(path_buf), "%s/package.sig", app_dir);
    int has_package_sig = (stat(path_buf, &st) == 0);

    /* Determine if app uses a database */
    int uses_database = (n_migrations > 0);
    if (!uses_database && has_manifest) {
        /* Check manifest for database-related indicators (fs_write for .db, etc.) */
        for (int i = 0; i < manifest.fs_write_count; i++) {
            if (strstr(manifest.fs_write[i], ".db"))
                uses_database = 1;
        }
    }

    /* Write JSON output */
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    sh_json_write_kv_string(&w, "app_dir", app_dir);
    sh_json_write_kv_string(&w, "runtime", runtime);

    const char *entry_basename = strrchr(entry_point, '/');
    entry_basename = entry_basename ? entry_basename + 1 : entry_point;
    sh_json_write_kv_string(&w, "entry_point", entry_basename);

    /* Manifest section */
    sh_json_write_key(&w, "manifest");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "present", has_manifest != 0);

    if (has_manifest) {
        sh_json_write_key(&w, "env");
        sh_json_write_array_start(&w);
        for (int i = 0; i < manifest.env_count; i++)
            sh_json_write_string(&w, manifest.env[i]);
        sh_json_write_array_end(&w);

        sh_json_write_key(&w, "hosts");
        sh_json_write_array_start(&w);
        for (int i = 0; i < manifest.hosts_count; i++)
            sh_json_write_string(&w, manifest.hosts[i]);
        sh_json_write_array_end(&w);

        sh_json_write_key(&w, "fs_write");
        sh_json_write_array_start(&w);
        for (int i = 0; i < manifest.fs_write_count; i++)
            sh_json_write_string(&w, manifest.fs_write[i]);
        sh_json_write_array_end(&w);

        sh_json_write_kv_bool(&w, "database", uses_database != 0);
        sh_json_write_kv_bool(&w, "gpu", manifest.gpu != 0);
        sh_json_write_kv_bool(&w, "compute", manifest.compute != 0);
    }
    sh_json_write_object_end(&w);

    /* Files section */
    sh_json_write_key(&w, "files");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "migrations", n_migrations);
    sh_json_write_kv_int(&w, "templates", n_templates);
    sh_json_write_kv_int(&w, "static", n_static);
    sh_json_write_kv_int(&w, "compute", n_compute);
    sh_json_write_kv_int(&w, "shaders", n_shaders);
    sh_json_write_object_end(&w);

    /* Compute modules — per-module detail.
     *
     * For every compute/<name>.wasm we report:
     *   - name             module name (filename without extension)
     *   - wasm_size        size of the .wasm artifact in bytes
     *   - has_aot          whether at least one compute/<name>.aot.<arch>
     *                      exists alongside the .wasm
     *   - has_source       whether compute/<name>/<name>.c exists
     *   - source_stale     true if has_source AND source mtime > .wasm mtime
     *                      (i.e. a `hull build` or `hull compute build` would
     *                      regenerate the artifact)
     *
     * Agents use this to surface compute coverage in deployment readiness
     * UIs and to decide whether to nudge developers to rebuild before
     * shipping.
     */
    sh_json_write_key(&w, "compute_modules");
    sh_json_write_array_start(&w);
    {
        char compute_dir[PATH_MAX];
        snprintf(compute_dir, sizeof(compute_dir), "%s/compute", app_dir);
        char **wasm_files = hl_tool_find_files(compute_dir, "*.wasm", NULL);
        if (wasm_files) {
            for (char **fp = wasm_files; *fp; fp++) {
                const char *full = *fp;
                /* Module name is the basename without the .wasm extension. */
                const char *slash = strrchr(full, '/');
                const char *base = slash ? slash + 1 : full;
                size_t base_len = strlen(base);
                if (base_len <= 5) continue;  /* must end with ".wasm" */
                char name[256];
                size_t copy_len = base_len - 5;
                if (copy_len >= sizeof(name)) copy_len = sizeof(name) - 1;
                memcpy(name, base, copy_len);
                name[copy_len] = '\0';

                /* Sizes + mtimes via stat. */
                struct stat wasm_st;
                long wasm_size = 0;
                time_t wasm_mtime = 0;
                if (stat(full, &wasm_st) == 0) {
                    wasm_size = (long)wasm_st.st_size;
                    wasm_mtime = wasm_st.st_mtime;
                }

                /* Look for compute/<name>.aot.* siblings. Buffer sized
                 * for sizeof(name) (256) + ".aot.*\0" (8) with a small
                 * safety margin. */
                int has_aot = 0;
                char aot_pattern[sizeof(name) + 16];
                snprintf(aot_pattern, sizeof(aot_pattern), "%s.aot.*", name);
                char **aot_files = hl_tool_find_files(compute_dir, aot_pattern, NULL);
                if (aot_files) {
                    has_aot = (aot_files[0] != NULL);
                    for (char **ap = aot_files; *ap; ap++) free(*ap);
                    free(aot_files);
                }

                /* Look for compute/<name>/<name>.c source. Path may exceed
                 * PATH_MAX with deeply nested compute_dir + long name; skip
                 * source-stale check in that pathological case. */
                char src_path[PATH_MAX];
                int sp_n = snprintf(src_path, sizeof(src_path), "%s/%s/%s.c",
                                    compute_dir, name, name);
                int has_source = 0, source_stale = 0;
                if (sp_n > 0 && (size_t)sp_n < sizeof(src_path)) {
                    struct stat src_st;
                    has_source = (stat(src_path, &src_st) == 0);
                    source_stale = has_source && (src_st.st_mtime > wasm_mtime);
                }

                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "name", name);
                sh_json_write_kv_int(&w, "wasm_size", (int)wasm_size);
                sh_json_write_kv_bool(&w, "has_aot", has_aot != 0);
                sh_json_write_kv_bool(&w, "has_source", has_source != 0);
                sh_json_write_kv_bool(&w, "source_stale", source_stale != 0);
                sh_json_write_object_end(&w);
            }
            for (char **fp = wasm_files; *fp; fp++) free(*fp);
            free(wasm_files);
        }
    }
    sh_json_write_array_end(&w);

    /* Signature section */
    sh_json_write_key(&w, "signature");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "package_sig", has_package_sig != 0);
    sh_json_write_object_end(&w);

    /* Existing configs */
    sh_json_write_key(&w, "configs");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "dockerfile", has_dockerfile != 0);
    sh_json_write_kv_bool(&w, "systemd", has_systemd != 0);
    sh_json_write_kv_bool(&w, "fly_toml", has_fly_toml != 0);
    sh_json_write_object_end(&w);

    /* Recommendations */
    sh_json_write_key(&w, "recommendations");
    sh_json_write_array_start(&w);
    if (!has_manifest)
        sh_json_write_string(&w, "No manifest found - add app.manifest({}) for env/host declarations");
    if (!has_package_sig)
        sh_json_write_string(&w, "No signature - run hull build --sign for verified deployments");
    if (has_manifest && manifest.env_count == 0 && manifest.hosts_count == 0)
        sh_json_write_string(&w, "Manifest is empty - declare env vars and hosts for better config generation");
    /* Stale-source advisory: re-scan briefly to flag any modules whose .c
     * is newer than the .wasm artifact. hull build auto-rebuilds when
     * clang is available; this advisory matters most for CI machines and
     * locked-down deploy pipelines without a wasm toolchain. */
    {
        char compute_dir[PATH_MAX];
        snprintf(compute_dir, sizeof(compute_dir), "%s/compute", app_dir);
        char **wasm_files = hl_tool_find_files(compute_dir, "*.wasm", NULL);
        if (wasm_files) {
            int any_stale = 0;
            for (char **fp = wasm_files; *fp && !any_stale; fp++) {
                const char *slash = strrchr(*fp, '/');
                const char *base = slash ? slash + 1 : *fp;
                size_t base_len = strlen(base);
                if (base_len <= 5) continue;
                char name[256];
                size_t copy_len = base_len - 5;
                if (copy_len >= sizeof(name)) copy_len = sizeof(name) - 1;
                memcpy(name, base, copy_len);
                name[copy_len] = '\0';
                char src_path[PATH_MAX];
                int sp_n = snprintf(src_path, sizeof(src_path), "%s/%s/%s.c",
                                    compute_dir, name, name);
                if (sp_n < 0 || (size_t)sp_n >= sizeof(src_path)) continue;
                struct stat src_st, wasm_st;
                if (stat(src_path, &src_st) == 0 &&
                    stat(*fp, &wasm_st) == 0 &&
                    src_st.st_mtime > wasm_st.st_mtime) {
                    any_stale = 1;
                }
            }
            if (any_stale) {
                sh_json_write_string(&w, "Compute source newer than .wasm - run `hull compute build` or rebuild (hull build auto-rebuilds when clang is available)");
            }
            for (char **fp = wasm_files; *fp; fp++) free(*fp);
            free(wasm_files);
        }
    }
    sh_json_write_array_end(&w);

    sh_json_write_object_end(&w);

    hl_manifest_free(&manifest);
    return 0;
}
