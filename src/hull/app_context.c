/*
 * app_context.c — Reusable application context for commands
 *
 * Consolidates the VFS + DB + runtime init sequence that was
 * previously duplicated in agent_lib.c and commands/test.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/cap/db.h"
#include "hull/entry.h"
#include "hull/migrate.h"
#include "hull/vfs.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"
#endif

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Opaque struct ─────────────────────────────────────────────────── */

struct HlAppContext {
    sqlite3       *db;
    HlVfs          app_vfs;
    HlVfs          platform_vfs;
    HlStmtCache    stmt_cache;
    HlRuntime     *rt;

    /* Storage — large enough for either runtime */
    // cppcheck-suppress unusedStructMember
    union {
#ifdef HL_ENABLE_LUA
        HlLua lua;
#endif
#ifdef HL_ENABLE_JS
        HlJS  js;
#endif
    } rt_storage;

    int            is_lua;    /* 1 = Lua, 0 = JS */
    int            db_open;   /* 1 = db was opened */
    int            rt_init;   /* 1 = runtime was initialized */

#ifdef HL_ENABLE_WASM
    HlWasmCache    wasm_cache;
    int            wasm_ok;   /* 1 = wasm_cache was initialized */
#endif
};

/* ── Entry point detection ─────────────────────────────────────────── */

static const char *detect_entry(const char *app_dir, const char *ext,
                                char *buf, size_t buf_size)
{
    size_t dir_len = strlen(app_dir);
    while (dir_len > 1 && app_dir[dir_len - 1] == '/')
        dir_len--;

    snprintf(buf, buf_size, "%.*s/app.%s", (int)dir_len, app_dir, ext);
    if (access(buf, F_OK) == 0) return buf;
    return NULL;
}

/* ── Init ──────────────────────────────────────────────────────────── */

int hl_app_context_init(HlAppContext **out, const HlAppContextOpts *opts)
{
    if (!out || !opts || !opts->app_dir) return -1;

    HlAppContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    /* Detect entry point */
    char entry_buf[4096];
    const char *entry = opts->entry_point;

    if (!entry) {
#ifdef HL_ENABLE_LUA
        entry = detect_entry(opts->app_dir, "lua", entry_buf, sizeof(entry_buf));
        if (entry) ctx->is_lua = 1;
#endif
#ifdef HL_ENABLE_JS
        if (!entry) { /* cppcheck-suppress identicalInnerCondition ; JS fallback after Lua */
            entry = detect_entry(opts->app_dir, "js", entry_buf, sizeof(entry_buf));
            if (entry) ctx->is_lua = 0;
        }
#endif
    } else {
        /* Caller provided entry point — determine runtime from extension */
        size_t len = strlen(entry);
        if (len >= 4 && strcmp(entry + len - 4, ".lua") == 0)
            ctx->is_lua = 1;
        else
            ctx->is_lua = 0;
    }

    // cppcheck-suppress knownConditionTrueFalse
    if (!entry) {
        free(ctx);
        return -1;
    }

    /* Open database */
    const char *db_path = opts->db_path ? opts->db_path : ":memory:";
    if (sqlite3_open(db_path, &ctx->db) != SQLITE_OK) {
        free(ctx);
        return -1;
    }
    ctx->db_open = 1;
    hl_cap_db_init(ctx->db);

    /* Init VFS instances */
    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    hl_vfs_init(&ctx->app_vfs, hl_app_entries, opts->app_dir);
    hl_vfs_init(&ctx->platform_vfs, hl_stdlib_entries, NULL);

    /* Run migrations */
    if (!opts->no_migrate)
        hl_migrate_run(ctx->db, &ctx->app_vfs);

    /* Init statement cache */
    hl_stmt_cache_init(&ctx->stmt_cache, ctx->db, opts->alloc);

#ifdef HL_ENABLE_WASM
    if (hl_cap_wasm_init(&ctx->wasm_cache) == 0)
        ctx->wasm_ok = 1;
#endif

    /* Init runtime */
#ifdef HL_ENABLE_LUA
    if (ctx->is_lua) {
        HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
        cfg.sandbox = opts->sandbox ? opts->sandbox : 1;
        HlLua *lua = &ctx->rt_storage.lua;
        memset(lua, 0, sizeof(*lua));
        lua->base.db = ctx->db;
        lua->base.stmt_cache = &ctx->stmt_cache;
        lua->base.app_vfs = &ctx->app_vfs;
        lua->base.platform_vfs = &ctx->platform_vfs;
#ifdef HL_ENABLE_WASM
        if (ctx->wasm_ok)
            lua->base.wasm_cache = &ctx->wasm_cache;
#endif
        ctx->rt = &lua->base;

        if (hl_lua_init(lua, &cfg) != 0) {
            hl_app_context_free(ctx);
            return -1;
        }
        ctx->rt_init = 1;

        if (hl_lua_load_app(lua, entry) != 0) {
            hl_app_context_free(ctx);
            return -1;
        }

        *out = ctx;
        return 0;
    }
#endif

#ifdef HL_ENABLE_JS
    if (!ctx->is_lua) {
        HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
        HlJS *js = &ctx->rt_storage.js;
        memset(js, 0, sizeof(*js));
        js->base.db = ctx->db;
        js->base.stmt_cache = &ctx->stmt_cache;
        js->base.app_vfs = &ctx->app_vfs;
        js->base.platform_vfs = &ctx->platform_vfs;
#ifdef HL_ENABLE_WASM
        if (ctx->wasm_ok)
            js->base.wasm_cache = &ctx->wasm_cache;
#endif
        ctx->rt = &js->base;

        if (hl_js_init(js, &cfg) != 0) {
            hl_app_context_free(ctx);
            return -1;
        }
        ctx->rt_init = 1;

        if (hl_js_load_app(js, entry) != 0) {
            hl_app_context_free(ctx);
            return -1;
        }

        *out = ctx;
        return 0;
    }
#endif

    hl_app_context_free(ctx);
    return -1;
}

/* ── Free ──────────────────────────────────────────────────────────── */

void hl_app_context_free(HlAppContext *ctx)
{
    if (!ctx) return;

    if (ctx->rt_init) {
#ifdef HL_ENABLE_LUA
        if (ctx->is_lua)
            hl_lua_free(&ctx->rt_storage.lua);
#endif
#ifdef HL_ENABLE_JS
        if (!ctx->is_lua)
            hl_js_free(&ctx->rt_storage.js);
#endif
    }

#ifdef HL_ENABLE_WASM
    if (ctx->wasm_ok)
        hl_cap_wasm_destroy(&ctx->wasm_cache);
#endif

    hl_stmt_cache_destroy(&ctx->stmt_cache);

    if (ctx->db_open) {
        hl_cap_db_shutdown(ctx->db);
        sqlite3_close(ctx->db);
    }

    ctx->rt = NULL;
    ctx->db = NULL;
    ctx->rt_init = 0;
    ctx->db_open = 0;
    free(ctx);
}

/* ── Accessors ─────────────────────────────────────────────────────── */

sqlite3 *hl_app_context_db(HlAppContext *ctx)
{
    return ctx ? ctx->db : NULL;
}

HlRuntime *hl_app_context_runtime(HlAppContext *ctx)
{
    return ctx ? ctx->rt : NULL;
}

const HlVfs *hl_app_context_app_vfs(HlAppContext *ctx)
{
    return ctx ? &ctx->app_vfs : NULL;
}

const HlVfs *hl_app_context_platform_vfs(HlAppContext *ctx)
{
    return ctx ? &ctx->platform_vfs : NULL;
}

HlStmtCache *hl_app_context_stmt_cache(HlAppContext *ctx)
{
    return ctx ? &ctx->stmt_cache : NULL;
}

int hl_app_context_is_lua(HlAppContext *ctx)
{
    return ctx ? ctx->is_lua : 0;
}

#ifdef HL_ENABLE_LUA
HlLua *hl_app_context_lua(HlAppContext *ctx)
{
    if (!ctx || !ctx->is_lua) return NULL;
    return &ctx->rt_storage.lua;
}
#endif

#ifdef HL_ENABLE_JS
HlJS *hl_app_context_js(HlAppContext *ctx)
{
    if (!ctx || ctx->is_lua) return NULL;
    return &ctx->rt_storage.js;
}
#endif
