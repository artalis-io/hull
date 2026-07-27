/*
 * tool_orchestration.c — orchestration bindings for the `tool` global
 *
 * Split out of runtime/lua/mod_tool.c per architectural audit A-1.
 * runtime/lua/ stays a thin binding layer (spawn / mkdir / copy /
 * read_file / etc.); orchestration that crosses into commands/,
 * agent_lib, dev_state, migrate, module_registry/resolver lives here
 * at the same layer as its consumers.
 *
 * Wire-up: `runtime/lua/runtime.c` calls hl_lua_tool_register() first
 * (installs the `tool` global with the base surface), then
 * hl_lua_tool_register_orchestration() which splices the entries
 * below onto the existing table.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tool_orchestration.h"

#ifdef HL_ENABLE_LUA

#include "hull/agent_lib.h"
#include "hull/commands/doctor.h"
#include "hull/dev_state.h"
#include "hull/manifest.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"

#include "sh_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "lua.h"
#include "lauxlib.h"

/* ── tool.modules_resolve(manifest_table) → {ok, modules|error} ───── */

/* Take a manifest-shaped Lua table (modules + fs/env/hosts arrays),
 * walk it through the resolver, and return either {ok=true, modules=...}
 * or {ok=false, error=...}. Used by `hull build` to validate the
 * manifest before signing. The fs/env/hosts arrays here are converted
 * to counts only — the resolver doesn't need the actual values. */
static int l_tool_modules_resolve(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    HlManifest m = {0};

    lua_getfield(L, 1, "fs");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "read");
        if (lua_istable(L, -1)) {
            lua_Integer n = luaL_len(L, -1);
            m.fs_read_count = n > HL_MANIFEST_MAX_PATHS
                              ? HL_MANIFEST_MAX_PATHS : (int)n;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "write");
        if (lua_istable(L, -1)) {
            lua_Integer n = luaL_len(L, -1);
            m.fs_write_count = n > HL_MANIFEST_MAX_PATHS
                               ? HL_MANIFEST_MAX_PATHS : (int)n;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "env");
    if (lua_istable(L, -1)) {
        lua_Integer n = luaL_len(L, -1);
        m.env_count = n > HL_MANIFEST_MAX_ENVS ? HL_MANIFEST_MAX_ENVS : (int)n;
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "hosts");
    if (lua_istable(L, -1)) {
        lua_Integer n = luaL_len(L, -1);
        m.hosts_count = n > HL_MANIFEST_MAX_HOSTS ? HL_MANIFEST_MAX_HOSTS : (int)n;
    }
    lua_pop(L, 1);

    /* modules — copy names so the resolver sees stable pointers. */
    char *owned_names[HL_MANIFEST_MAX_MODULES] = {0};
    int owned_count = 0;
    lua_getfield(L, 1, "modules");
    if (lua_istable(L, -1)) {
        m.modules_declared = 1;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0 && m.modules_count < HL_MANIFEST_MAX_MODULES) {
            if (lua_isstring(L, -1)) {
                /* Canonical form: each VALUE is a spec "vendor/name@major"
                 * (array `{"hull/db@1"}` or keyed `{db="hull/db@1"}`; the
                 * value carries the spec either way). Same parse as
                 * manifest_declares_module() in mod_app.c: strip the "@N"
                 * major for the registry lookup. Falls back to the legacy
                 * key=name / value=version-number form for a bare number. */
                const char *val = lua_tostring(L, -1);
                int is_spec = strchr(val, '/') != NULL || strchr(val, '@') != NULL;
                const char *spec = NULL;
                size_t nlen = 0;
                long v = 0;
                int optional = 0;
                if (is_spec) {
                    spec = val;
                    /* Trailing '?' marks the module optional; it's after the
                     * major so the name (before '@') is unaffected, and strtol
                     * with a NULL end already stops at the '?'. */
                    size_t sl = strlen(spec);
                    optional = (sl > 0 && spec[sl - 1] == '?');
                    const char *at = strchr(spec, '@');
                    /* When there's no '@', strip a trailing '?' from the name so
                     * "hull/gpu?" resolves to "hull/gpu" (matches manifest_lua.c). */
                    nlen = at ? (size_t)(at - spec)
                              : (optional ? sl - 1 : sl);
                    v = at ? strtol(at + 1, NULL, 10) : 1;
                } else if (lua_type(L, -2) == LUA_TSTRING) {
                    spec = lua_tostring(L, -2);
                    nlen = strlen(spec);
                    v = strtol(val, NULL, 10);
                }
                if (spec && nlen > 0 && v >= 1 && v <= 255) {
                    char *copy = malloc(nlen + 1);
                    if (copy) {
                        memcpy(copy, spec, nlen);
                        copy[nlen] = '\0';
                        owned_names[owned_count] = copy;
                        m.modules[m.modules_count].name = copy;
                        m.modules[m.modules_count].api_major = (uint8_t)v;
                        m.modules[m.modules_count].optional = (uint8_t)optional;
                        m.modules_count++;
                        owned_count++;
                    }
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* Optional arg 2: a build flavor name. When set, validate against the
     * TARGET flavor's caps (e.g. `hull build --flavor=pure-compute` rejects
     * hull/web/ws-server) rather than this running hull's compiled-in caps. */
    uint32_t caps = hl_module_build_caps();
    if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
        const char *flavor = lua_tostring(L, 2);
        const HlBuildFlavor *f = hl_build_flavor_find(flavor);
        if (!f) {
            for (int i = 0; i < owned_count; i++) free(owned_names[i]);
            lua_newtable(L);
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "ok");
            lua_pushfstring(L, "unknown build flavor '%s'", flavor);
            lua_setfield(L, -2, "error");
            return 1;
        }
        caps = hl_build_flavor_caps(f, caps);
    }

    /* Optional arg 3: an array of composed feature names (hull build --with=X).
     * Admit the build caps those features supply, so the base build-tool's
     * resolver accepts modules the feature will fill into the produced app
     * binary (e.g. --with=gpu admits hull/gpu even though this hull, and the
     * base platform lib, were not compiled with HL_ENABLE_GPU). */
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            if (lua_isstring(L, -1))
                caps |= hl_module_feature_cap(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }

    HlResolvedModuleSet set;
    char err[HL_MODULE_RESOLVER_ERR_MAX] = {0};
    int rc = hl_module_resolver_resolve_caps(&m, &set, caps, err, sizeof(err));

    for (int i = 0; i < owned_count; i++) free(owned_names[i]);

    lua_newtable(L);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, err);
        lua_setfield(L, -2, "error");
        return 1;
    }

    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "ok");

    lua_newtable(L);  /* modules array */
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    int idx = 1;
    for (size_t i = 0; i < total; i++) {
        if (!hl_module_set_contains_index(&set, (int)i)) continue;
        lua_newtable(L);
        lua_pushstring(L, all[i].name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, all[i].api_major);
        lua_setfield(L, -2, "api_major");
        lua_pushboolean(L, all[i].intrinsic ? 1 : 0);
        lua_setfield(L, -2, "intrinsic");
        lua_rawseti(L, -2, idx++);
    }
    lua_setfield(L, -2, "modules");

    /* needs_http = does the resolved set require any HTTP subsystem (server or
     * client)? Drives `hull build`'s decision to compose the http core + the
     * per-runtime web bindings (issue #114, Phase C2). Reliable because the
     * route/ws/sse decorations (app.get/…) only exist when an HTTP module is
     * declared, so a serving app always trips an HTTP cap here. */
    uint32_t req_caps = hl_module_set_required_caps(&set);
    lua_pushboolean(L, (req_caps & HL_MOD_CAP_HTTP) ? 1 : 0);
    lua_setfield(L, -2, "needs_http");

    /* auto = the minimal build flavor that still satisfies this app's
     * declared modules (drives `hull build --flavor=auto`). Computed from
     * the resolved set's aggregate required-caps. */
    const HlBuildFlavor *autf = hl_build_flavor_auto(req_caps);
    if (autf) {
        lua_newtable(L);
        lua_pushstring(L, autf->name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, autf->asset);
        lua_setfield(L, -2, "asset");
        lua_setfield(L, -2, "auto");
    }

    return 1;
}

/* ── tool.build_flavor(name) → {ok, asset|error} ──────────────────── */

/* Validate a `hull build --flavor=<name>` value against the flavor registry
 * and return the platform-lib asset stem so build.lua can locate/link it.
 * Unknown name → {ok=false, error=...} listing the valid flavors. */
static int l_tool_build_flavor(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const HlBuildFlavor *f = hl_build_flavor_find(name);

    lua_newtable(L);
    if (!f) {
        const HlBuildFlavor *all = NULL;
        int n = hl_build_flavor_all(&all);
        char valid[256];
        size_t off = 0;
        for (int i = 0; i < n && off + 1 < sizeof(valid); i++) {
            int w = snprintf(valid + off, sizeof(valid) - off, "%s%s",
                             i ? ", " : "", all[i].name);
            if (w < 0) break;
            off += (size_t)w;
        }
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushfstring(L, "unknown build flavor '%s' (valid: %s)", name, valid);
        lua_setfield(L, -2, "error");
        return 1;
    }
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "ok");
    lua_pushstring(L, f->asset);
    lua_setfield(L, -2, "asset");
    return 1;
}

/* ── tool.doctor_json() — render the `hull doctor --json` payload ── */

/* Wrapping `hl_doctor_collect_json` via tmpfile() avoids
 * reimplementing the JSON payload in Lua. Consumed by
 * stdlib/lua/hull/doctor_tui.lua.
 *
 * tmpfile() (POSIX) is used in preference to open_memstream (glibc/BSD)
 * because Cosmopolitan's libc does not provide open_memstream. The
 * doctor payload is small (< 4 KiB), so the disk round-trip is
 * irrelevant. */
static int l_tool_doctor_json(lua_State *L)
{
    FILE *f = tmpfile();
    if (!f) return luaL_error(L, "doctor_json: tmpfile failed");
    hl_doctor_collect_json(f);
    fflush(f);

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return luaL_error(L, "doctor_json: fseek end failed"); }
    long end = ftell(f);
    if (end < 0)                    { fclose(f); return luaL_error(L, "doctor_json: ftell failed"); }
    rewind(f);

    char *buf = (char *)malloc((size_t)end);
    if (!buf)                       { fclose(f); return luaL_error(L, "doctor_json: malloc failed"); }
    size_t n = fread(buf, 1, (size_t)end, f);
    fclose(f);

    lua_pushlstring(L, buf, n);
    free(buf);
    return 1;
}

/* ── tool.agent_errors(app_dir) / tool.agent_context(task, level) ── */

#ifdef HL_ENABLE_HTTP_SERVER
/* hl_agent_errors reads the dev server's .hull/last_error.json sidecar, so
 * it ships only with the inbound HTTP server (which `hull dev` needs). */
static int l_tool_agent_errors(lua_State *L)
{
    const char *app_dir = luaL_optstring(L, 1, ".");
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_errors(app_dir, &out);
    if (out.buf) lua_pushlstring(L, out.buf, out.len);
    else         lua_pushnil(L);
    sh_json_buf_free(&out);
    lua_pushinteger(L, rc);
    return 2;
}
#endif /* HL_ENABLE_HTTP_SERVER */

static int l_tool_agent_context(lua_State *L)
{
    const char *task  = luaL_checkstring(L, 1);
    const char *level = luaL_optstring(L, 2, "compact");
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_context(task, level, &out);
    if (out.buf) lua_pushlstring(L, out.buf, out.len);
    else         lua_pushnil(L);
    sh_json_buf_free(&out);
    lua_pushinteger(L, rc);
    return 2;
}

#ifdef HL_ENABLE_DB

#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/migrate.h"
#include "hull/vfs.h"

extern const HlEntry hl_app_entries[];

/* ── tool.migrate_status(app_dir, db_path) — applied/pending list ── */

static int l_tool_migrate_status(lua_State *L)
{
    const char *app_dir = luaL_optstring(L, 1, ".");
    const char *db_path = luaL_optstring(L, 2, "data.db");

    const char *db_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(db_path, &db_err);
    if (!be) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s", db_err ? db_err : "no database backend");
        return 2;
    }
    HlDbHandle handle = { .backend = be, .ctx = NULL };
    if (be->open(&handle.ctx, db_path, NULL) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open database: %s", db_path);
        return 2;
    }

    HlVfs vfs;
    hl_vfs_init(&vfs, hl_app_entries, app_dir);

    HlMigrationStatus *entries = NULL;
    int count = 0;
    int rc = hl_migrate_status(&handle, &vfs, &entries, &count);
    be->close(&handle);

    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "migrate_status query failed");
        return 2;
    }

    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_newtable(L);
        lua_pushstring(L, entries[i].name);    lua_setfield(L, -2, "name");
        lua_pushboolean(L, entries[i].applied); lua_setfield(L, -2, "applied");
        if (entries[i].applied_at) {
            lua_pushstring(L, entries[i].applied_at);
            lua_setfield(L, -2, "applied_at");
        }
        lua_rawseti(L, -2, i + 1);
    }
    hl_migrate_status_free(entries, count);
    lua_pushinteger(L, count);
    return 2;
}
#endif

/* ── tool.modules_available — every first-party registry entry ──── */

static int l_tool_modules_available(lua_State *L)
{
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)total);
    lua_setfield(L, -2, "count");

    lua_newtable(L);  /* modules array */
    for (size_t i = 0; i < total; i++) {
        const HlModuleSpec *s = &all[i];
        lua_newtable(L);

        lua_pushstring(L, s->name);                 lua_setfield(L, -2, "name");
        lua_pushinteger(L, s->api_major);            lua_setfield(L, -2, "api_major");
        lua_pushboolean(L, s->intrinsic);            lua_setfield(L, -2, "intrinsic");
        lua_pushboolean(L, s->pure);                 lua_setfield(L, -2, "pure");

        lua_newtable(L);
        int di = 1;
        for (int j = 0; j < HL_MODULE_MAX_DEPS && s->deps[j]; j++) {
            lua_pushstring(L, s->deps[j]);
            lua_rawseti(L, -2, di++);
        }
        lua_setfield(L, -2, "deps");

        lua_newtable(L);
        int ci = 1;
        uint32_t need = s->required_caps;
        struct { uint32_t bit; const char *name; } map[] = {
            { HL_MOD_CAP_FS,          "fs"          },
            { HL_MOD_CAP_HOSTS,       "hosts"       },
            { HL_MOD_CAP_ENV,         "env"         },
            { HL_MOD_CAP_DB,          "db"          },
            { HL_MOD_CAP_WASM,        "wasm"        },
            { HL_MOD_CAP_GPU,         "gpu"         },
            { HL_MOD_CAP_HTTP_CLIENT, "http_client" },
            { HL_MOD_CAP_HTTP_SERVER, "http_server" },
            { HL_MOD_CAP_TUI,         "tui"         },
        };
        for (size_t k = 0; k < sizeof map / sizeof map[0]; k++) {
            if (need & map[k].bit) {
                lua_pushstring(L, map[k].name);
                lua_rawseti(L, -2, ci++);
            }
        }
        lua_setfield(L, -2, "caps");

        lua_rawseti(L, -2, (int)(i + 1));
    }
    lua_setfield(L, -2, "modules");
    return 1;
}

/* ── tool.dev_* — hull dev --tui bindings ─────────────────────────── */

/* All dev_* accessors consult hl_dev_state(), which is a no-op
 * (returns nil / safe defaults) when hull dev --tui isn't running.
 * That lets the stdlib's *_tui modules load cleanly in tests too.
 * The dev-state machinery (commands/dev.c) exists solely for `hull dev --tui`,
 * so it — and these bindings — need BOTH the inbound-HTTP server (hull dev) and
 * the TUI subsystem. On a TUI-off build (base composing TUI as a feature) they
 * drop out together with the dev-state singleton. */
#if defined(HL_ENABLE_HTTP_SERVER) && defined(HL_TUI_LINKED)

static int l_tool_dev_status(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    if (!s) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)s->child_pid);  lua_setfield(L, -2, "pid");
    lua_pushinteger(L, s->reload_count);            lua_setfield(L, -2, "reload_count");
    lua_pushinteger(L, (lua_Integer)s->last_reload_ms); lua_setfield(L, -2, "last_reload_ms");
    lua_pushinteger(L, s->log_count);               lua_setfield(L, -2, "log_count");
    lua_pushstring(L,  s->app_dir);                 lua_setfield(L, -2, "app_dir");

    int alive = 0;
    if (s->child_pid > 0) {
        int status;
        pid_t r = waitpid(s->child_pid, &status, WNOHANG);
        if (r == 0) alive = 1;
        else if (r == s->child_pid) {
            s->child_pid = 0;
        }
    }
    lua_pushboolean(L, alive); lua_setfield(L, -2, "alive");
    return 1;
}

static int l_tool_dev_drain(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    if (!s) { lua_pushinteger(L, 0); return 1; }
    int before = s->log_count;
    hl_dev_state_drain(s);
    lua_pushinteger(L, s->log_count - before);
    return 1;
}

static int l_tool_dev_recent_lines(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    int n = (int)luaL_optinteger(L, 1, 50);
    if (n < 1) n = 1;
    if (n > HL_DEV_LOG_LINES) n = HL_DEV_LOG_LINES;

    lua_newtable(L);
    if (!s || s->log_count == 0) return 1;

    int available = s->log_count < HL_DEV_LOG_LINES ? s->log_count : HL_DEV_LOG_LINES;
    if (n > available) n = available;

    for (int i = 0; i < n; i++) {
        int idx = (s->log_head - 1 - i + HL_DEV_LOG_LINES) % HL_DEV_LOG_LINES;
        lua_pushstring(L, s->log_lines[idx]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_tool_dev_check_file_change(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    lua_pushboolean(L, s ? hl_dev_state_check_file_change(s) : 0);
    return 1;
}

static int l_tool_dev_reload(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    if (!s) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, hl_dev_state_reload(s) == 0);
    return 1;
}

#endif /* HL_ENABLE_HTTP_SERVER && HL_TUI_LINKED */

/* ── tool.build_caps() → { db=true, wasm=true, tui=true, ... } ──────────
 * The base platform lib's COMPILE-TIME build caps (hl_module_build_caps),
 * as a name-keyed set. This is what an app inherits by linking the base;
 * it excludes the toolchain's private runtime force-loads (e.g. the tui
 * feature backing `hull ... --tui`). `hull build` uses it to decide whether
 * a declared feature must be composed (--with=X) or is already built in:
 * a stock native hull reports no `tui`, a monolithic HL_ENABLE_TUI or cosmo
 * build reports `tui`. */
static int l_tool_build_caps(lua_State *L)
{
    uint32_t caps = hl_module_build_caps();
    struct { uint32_t bit; const char *name; } map[] = {
        { HL_MOD_CAP_DB,          "db"          },
        { HL_MOD_CAP_WASM,        "wasm"        },
        { HL_MOD_CAP_GPU,         "gpu"         },
        { HL_MOD_CAP_HTTP_CLIENT, "http_client" },
        { HL_MOD_CAP_HTTP_SERVER, "http_server" },
        { HL_MOD_CAP_TUI,         "tui"         },
    };
    lua_newtable(L);
    for (size_t k = 0; k < sizeof map / sizeof map[0]; k++) {
        if (caps & map[k].bit) {
            lua_pushboolean(L, 1);
            lua_setfield(L, -2, map[k].name);
        }
    }
    return 1;
}

/* ── Registration ──────────────────────────────────────────────────── */

void hl_lua_tool_register_orchestration(lua_State *L)
{
    /* Splice onto the existing `tool` global. The base table was
     * installed by hl_lua_tool_register(); we look it up and add
     * each function as a field. */
    lua_getglobal(L, "tool");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    static const luaL_Reg orchestration_funcs[] = {
        { "modules_resolve",        l_tool_modules_resolve },
        { "build_caps",             l_tool_build_caps },
        { "build_flavor",           l_tool_build_flavor },
        { "doctor_json",            l_tool_doctor_json },
#ifdef HL_ENABLE_HTTP_SERVER
        { "agent_errors",           l_tool_agent_errors },
#endif
        { "agent_context",          l_tool_agent_context },
        { "modules_available",      l_tool_modules_available },
#if defined(HL_ENABLE_HTTP_SERVER) && defined(HL_TUI_LINKED)
        { "dev_status",             l_tool_dev_status },
        { "dev_drain",              l_tool_dev_drain },
        { "dev_recent_lines",       l_tool_dev_recent_lines },
        { "dev_check_file_change",  l_tool_dev_check_file_change },
        { "dev_reload",             l_tool_dev_reload },
#endif
#ifdef HL_ENABLE_DB
        { "migrate_status",         l_tool_migrate_status },
#endif
        { NULL, NULL }
    };
    luaL_setfuncs(L, orchestration_funcs, 0);

    lua_pop(L, 1);  /* drop tool */
}

#endif /* HL_ENABLE_LUA */
