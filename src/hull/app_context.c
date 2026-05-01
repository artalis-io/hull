/*
 * app_context.c — Reusable application context for commands
 *
 * Consolidates the VFS + DB + runtime init sequence that was
 * previously duplicated in agent_lib.c, commands/test.c, and main.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/entry.h"
#include "hull/migrate.h"
#include "hull/vfs.h"
#include "hull/worker_db.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"
#endif
#ifdef HL_ENABLE_GPU
#include "hull/cap/gpu.h"
#endif

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Opaque struct ─────────────────────────────────────────────────── */

struct HlAppContext {
    HlDbHandle     db_handle;     /* vtable-based DB handle */
    sqlite3       *db;            /* raw sqlite3* for migrate/agent (from handle) */
    HlVfs          app_vfs;
    HlVfs          platform_vfs;
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
    int            app_loaded; /* 1 = app code was loaded */

    /* Tracks whether context owns the WASM cache (internal init) vs
     * borrowing an external one (server passes its own static cache). */
#ifdef HL_ENABLE_WASM
    HlWasmCache    wasm_cache;
    int            wasm_ok;       /* 1 = wasm_cache was initialized */
    int            wasm_external; /* 1 = external cache, don't destroy */
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

/* ── Internal: determine runtime type from entry point ─────────────── */

static int resolve_entry_and_runtime(HlAppContext *ctx,
                                     const HlAppContextOpts *opts,
                                     const char **out_entry,
                                     char *entry_buf, size_t buf_size)
{
    const char *entry = opts->entry_point;

    if (!entry) {
#ifdef HL_ENABLE_LUA
        entry = detect_entry(opts->app_dir, "lua", entry_buf, buf_size);
        if (entry) ctx->is_lua = 1;
#endif
#ifdef HL_ENABLE_JS
        if (!entry) { /* cppcheck-suppress identicalInnerCondition ; JS fallback after Lua */
            entry = detect_entry(opts->app_dir, "js", entry_buf, buf_size);
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
    if (!entry)
        return -1;

    *out_entry = entry;
    return 0;
}

/* ── Internal: load app code into an initialized runtime ───────────── */

static int load_app_code(HlAppContext *ctx, const char *entry)
{
#ifdef HL_ENABLE_LUA
    if (ctx->is_lua) {
        if (hl_lua_load_app(&ctx->rt_storage.lua, entry) != 0)
            return -1;
        ctx->app_loaded = 1;
        return 0;
    }
#endif

#ifdef HL_ENABLE_JS
    if (!ctx->is_lua) {
        if (hl_js_load_app(&ctx->rt_storage.js, entry) != 0)
            return -1;
        ctx->app_loaded = 1;
        return 0;
    }
#endif

    (void)entry;
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────────── */

int hl_app_context_init(HlAppContext **out, const HlAppContextOpts *opts)
{
    if (!out || !opts || !opts->app_dir) return -1;

    HlAppContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    /* Detect entry point and runtime type */
    char entry_buf[4096];
    const char *entry = NULL;
    if (resolve_entry_and_runtime(ctx, opts, &entry, entry_buf, sizeof(entry_buf)) != 0) {
        free(ctx);
        return -1;
    }

    /* Open database via vtable */
    const char *db_path = opts->db_path ? opts->db_path : ":memory:";
    ctx->db_handle.backend = &hl_db_backend_sqlite;
    if (hl_db_backend_sqlite.open(&ctx->db_handle.ctx, db_path,
                                   opts->alloc) != 0) {
        free(ctx);
        return -1;
    }
    ctx->db_open = 1;
    ctx->db = hl_db_sqlite_raw(&ctx->db_handle);

    /* Init VFS instances */
    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    hl_vfs_init(&ctx->app_vfs, hl_app_entries, opts->app_dir);
    hl_vfs_init(&ctx->platform_vfs, hl_stdlib_entries, NULL);

    /* Run migrations (fail on error to prevent starting with broken schema) */
    if (!opts->no_migrate) {
        int migrated = hl_migrate_run(ctx->db, &ctx->app_vfs);
        if (migrated == HL_MIGRATE_ERR) {
            hl_app_context_free(ctx);
            return -1;
        }
    }

    /* WASM cache: use external if provided, else init internal */
#ifdef HL_ENABLE_WASM
    if (opts->wasm_cache) {
        ctx->wasm_external = 1;
        ctx->wasm_ok = 1;
    } else {
        if (hl_cap_wasm_init(&ctx->wasm_cache) == 0)
            ctx->wasm_ok = 1;
    }
#endif

    /* Init runtime */
#ifdef HL_ENABLE_LUA
    if (ctx->is_lua) {
        HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
        cfg.sandbox = opts->sandbox ? opts->sandbox : 1;
        if (opts->heap_limit > 0)        cfg.max_heap_bytes   = (size_t)opts->heap_limit;
        if (opts->instruction_limit > 0) cfg.max_instructions = opts->instruction_limit;
        HlLua *lua = &ctx->rt_storage.lua;
        memset(lua, 0, sizeof(*lua));
        lua->base.db_handle = &ctx->db_handle;
        lua->base.hull_db_handle = &ctx->db_handle;
        lua->base.app_vfs = &ctx->app_vfs;
        lua->base.platform_vfs = &ctx->platform_vfs;
        if (opts->alloc) lua->base.alloc = opts->alloc;
        if (opts->thread_pool) lua->base.thread_pool = opts->thread_pool;
        if (opts->worker_db_path) lua->base.db_path = opts->worker_db_path;
        if (opts->compress) lua->base.compress = opts->compress;
#ifdef HL_ENABLE_WASM
        if (ctx->wasm_ok)
            lua->base.wasm_cache = opts->wasm_cache ? opts->wasm_cache : &ctx->wasm_cache;
#endif
#ifdef HL_ENABLE_GPU
        if (opts->gpu_ctx) lua->base.gpu_ctx = opts->gpu_ctx;
#endif
        ctx->rt = &lua->base;
        ctx->rt->vt = &hl_lua_vtable;

        if (hl_lua_init(lua, &cfg) != 0) {
            hl_app_context_free(ctx);
            return -1;
        }
        ctx->rt_init = 1;

        if (!opts->no_load) {
            if (load_app_code(ctx, entry) != 0) {
                hl_app_context_free(ctx);
                return -1;
            }
        }

        /* Init worker DB after runtime init (needs db_path) */
        if (opts->worker_db_path)
            hl_worker_db_init(opts->worker_db_path);

        *out = ctx;
        return 0;
    }
#endif

#ifdef HL_ENABLE_JS
    if (!ctx->is_lua) {
        HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
        if (opts->heap_limit > 0)        cfg.max_heap_bytes   = (size_t)opts->heap_limit;
        if (opts->stack_limit > 0)       cfg.max_stack_bytes  = (size_t)opts->stack_limit;
        if (opts->instruction_limit > 0) cfg.max_instructions = opts->instruction_limit;
        HlJS *js = &ctx->rt_storage.js;
        memset(js, 0, sizeof(*js));
        js->base.db_handle = &ctx->db_handle;
        js->base.hull_db_handle = &ctx->db_handle;
        js->base.app_vfs = &ctx->app_vfs;
        js->base.platform_vfs = &ctx->platform_vfs;
        if (opts->alloc) js->base.alloc = opts->alloc;
        if (opts->thread_pool) js->base.thread_pool = opts->thread_pool;
        if (opts->worker_db_path) js->base.db_path = opts->worker_db_path;
        if (opts->compress) js->base.compress = opts->compress;
#ifdef HL_ENABLE_WASM
        if (ctx->wasm_ok)
            js->base.wasm_cache = opts->wasm_cache ? opts->wasm_cache : &ctx->wasm_cache;
#endif
#ifdef HL_ENABLE_GPU
        if (opts->gpu_ctx) js->base.gpu_ctx = opts->gpu_ctx;
#endif
        ctx->rt = &js->base;
        ctx->rt->vt = &hl_js_vtable;

        if (hl_js_init(js, &cfg) != 0) {
            hl_app_context_free(ctx);
            return -1;
        }
        ctx->rt_init = 1;

        if (!opts->no_load) {
            if (load_app_code(ctx, entry) != 0) {
                hl_app_context_free(ctx);
                return -1;
            }
        }

        /* Init worker DB after runtime init */
        if (opts->worker_db_path)
            hl_worker_db_init(opts->worker_db_path);

        *out = ctx;
        return 0;
    }
#endif

    hl_app_context_free(ctx);
    return -1;
}

/* ── Load (deferred) ──────────────────────────────────────────────── */

int hl_app_context_load(HlAppContext *ctx, const char *entry_point)
{
    if (!ctx || !entry_point) return -1;
    if (ctx->app_loaded) return 0; /* already loaded */
    if (!ctx->rt_init) return -1;  /* runtime not initialized */
    return load_app_code(ctx, entry_point);
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
    if (ctx->wasm_ok && !ctx->wasm_external)
        hl_cap_wasm_destroy(&ctx->wasm_cache);
#endif

    if (ctx->db_open) {
        ctx->db_handle.backend->close(ctx->db_handle.ctx);
        ctx->db_handle.ctx = NULL;
        ctx->db = NULL;
    }

    ctx->rt = NULL;
    ctx->db = NULL;
    ctx->rt_init = 0;
    ctx->db_open = 0;
    ctx->app_loaded = 0;
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
    return ctx ? hl_db_sqlite_cache(&ctx->db_handle) : NULL;
}

int hl_app_context_is_lua(HlAppContext *ctx)
{
    return ctx ? ctx->is_lua : 0;
}

const char *hl_app_context_app_dir(HlAppContext *ctx)
{
    return ctx ? ctx->app_vfs.root_dir : NULL;
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
