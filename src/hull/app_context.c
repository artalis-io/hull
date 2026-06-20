/*
 * app_context.c — Reusable application context for commands
 *
 * Consolidates the VFS + DB + runtime init sequence that was
 * previously duplicated in agent_lib.c, commands/test.c, and main.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/entry.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h"
#include "hull/runtime/factory.h"
#include "hull/vfs.h"

#ifdef HL_ENABLE_DB
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/migrate.h"
#include "hull/worker_db.h"
#include <sqlite3.h>
#endif

/* app_context is meant to stay runtime-agnostic — it dispatches
 * through the HlRuntimeFactory vtable and never touches Lua/JS
 * internals directly. The hl_app_context_lua / _js accessors below
 * return typed pointers (HlLua * / HlJS *), but a cast to a typed
 * pointer only needs a forward declaration of the type, not the full
 * struct shape. Pulling in hull/runtime/lua.h or hull/runtime/js.h
 * here would surface the full HlLua / HlJS struct layout to a layer
 * that has no business inspecting it. */
#ifdef HL_ENABLE_LUA
typedef struct HlLua HlLua;
#endif
#ifdef HL_ENABLE_JS
typedef struct HlJS HlJS;
#endif
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"
#endif
#ifdef HL_ENABLE_GPU
#include "hull/cap/gpu.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Opaque struct ─────────────────────────────────────────────────── */

struct HlAppContext {
#ifdef HL_ENABLE_DB
    HlDbHandle     db_handle;     /* vtable-based DB handle */
    sqlite3       *db;            /* raw sqlite3* for migrate/agent (from handle) */
    int            db_open;       /* 1 = db was opened */
#endif
    HlVfs          app_vfs;
    HlVfs          platform_vfs;
    HlRuntime     *rt;

    /* Resolved module set (opt-in via opts.gate_modules). Lives here so
     * its lifetime matches the runtime — rt->module_set borrows. */
    HlResolvedModuleSet module_set;
    int                 module_set_wired;

    /* Factory that built `rt`, owns the heap storage. NULL until init. */
    const HlRuntimeFactory *factory;

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
        /* Discover entry by trying each registered factory's extension
         * in order. First match wins (Lua then JS today). */
        size_t fcount = 0;
        const HlRuntimeFactory *const *factories = hl_runtime_factories(&fcount);
        for (size_t i = 0; i < fcount; i++) {
            const HlRuntimeFactory *f = factories[i];
            if (!f || !f->entry_extension) continue;
            /* extension is ".lua" / ".js" — strip leading dot for detect_entry */
            const char *ext = f->entry_extension;
            if (ext[0] == '.') ext++;
            entry = detect_entry(opts->app_dir, ext, entry_buf, buf_size);
            if (entry) {
                ctx->factory = f;
                break;
            }
        }
    } else {
        /* Caller provided entry point — match factory by file extension. */
        ctx->factory = hl_runtime_factory_for_filename(entry);
    }

    // cppcheck-suppress knownConditionTrueFalse
    if (!entry || !ctx->factory)
        return -1;

    *out_entry = entry;
    return 0;
}

/* ── Internal: load app code into an initialized runtime ───────────── */

static int load_app_code(HlAppContext *ctx, const char *entry)
{
    if (!ctx->rt || !ctx->rt->vt || !ctx->rt->vt->load_app) return -1;
    if (ctx->rt->vt->load_app(ctx->rt, entry) != 0)
        return -1;
    ctx->app_loaded = 1;
    return 0;
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

    /* Open database via vtable (skip in pure compute mode) */
#ifdef HL_ENABLE_DB
    if (!opts->no_db) {
        const char *db_path = opts->db_path ? opts->db_path : ":memory:";
        ctx->db_handle.backend = &hl_db_backend_sqlite;
        if (hl_db_backend_sqlite.open(&ctx->db_handle.ctx, db_path,
                                       opts->alloc) != 0) {
            free(ctx);
            return -1;
        }
        ctx->db_open = 1;
        ctx->db = hl_db_sqlite_raw(&ctx->db_handle);
    }
#endif

    /* Init VFS instances */
    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    hl_vfs_init(&ctx->app_vfs, hl_app_entries, opts->app_dir);
    hl_vfs_init(&ctx->platform_vfs, hl_stdlib_entries, NULL);

#ifdef HL_ENABLE_DB
    /* Run migrations (fail on error to prevent starting with broken schema) */
    if (!opts->no_migrate && ctx->db_open) {
        int migrated = hl_migrate_run(&ctx->db_handle, &ctx->app_vfs);
        if (migrated == HL_MIGRATE_ERR) {
            hl_app_context_free(ctx);
            return -1;
        }
    }
#endif

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

    /* Build base config bundle — wired into rt->base BEFORE init. */
    HlRuntimeBaseConfig base = {0};
#ifdef HL_ENABLE_DB
    if (ctx->db_open) {
        base.db_handle      = &ctx->db_handle;
        base.hull_db_handle = &ctx->db_handle;
    }
#endif
    base.alloc        = opts->alloc;
    base.app_vfs      = &ctx->app_vfs;
    base.platform_vfs = &ctx->platform_vfs;
    base.thread_pool  = opts->thread_pool;
    base.db_path      = opts->worker_db_path;
    base.compress     = opts->compress;
#ifdef HL_ENABLE_WASM
    if (ctx->wasm_ok)
        base.wasm_cache = opts->wasm_cache ? opts->wasm_cache : &ctx->wasm_cache;
#endif
#ifdef HL_ENABLE_GPU
    base.gpu_ctx = opts->gpu_ctx;
#endif

    /* Init runtime via the factory — table-driven, single-path. */
    if (ctx->factory->create(&ctx->rt, opts, &base) != 0) {
        hl_app_context_free(ctx);
        return -1;
    }
    ctx->rt_init = 1;

    if (!opts->no_load) {
        if (load_app_code(ctx, entry) != 0) {
            hl_app_context_free(ctx);
            return -1;
        }

        /* Module-system gating (opt-in via opts.gate_modules).
         *
         * After the app's top-level code has run (which sets the
         * manifest) and before any handler is invoked, extract the
         * manifest, run the resolver, and wire the resulting set onto
         * the runtime so require/import enforce the declared module
         * surface. This is the same mechanism the server path
         * (main.c::hl_serve_wire_caps) uses, lifted into the shared
         * context so agent/mcp can opt in without re-implementing.
         *
         * The set lives in HlAppContext (lifetime matches the runtime).
         * Resolver failure is fatal — caller sees init failure. */
        if (opts->gate_modules) {
            HlManifest m = {0};
            if (ctx->rt->vt->extract_manifest(ctx->rt, &m) == 0) {
                char err[HL_MODULE_RESOLVER_ERR_MAX] = {0};
                int rc = hl_module_resolver_resolve(&m, &ctx->module_set,
                                                     err, sizeof(err));
                hl_manifest_free(&m);
                if (rc != 0) {
                    fprintf(stderr, "[app-context] module resolver: %s\n", err);
                    hl_app_context_free(ctx);
                    return -1;
                }
            } else {
                /* No manifest declared → resolver still admits intrinsics. */
                hl_module_resolver_resolve(NULL, &ctx->module_set, NULL, 0);
            }
            ctx->rt->module_set = &ctx->module_set;
            ctx->module_set_wired = 1;
        }
    }

    /* Init worker DB after runtime init (needs db_path). */
#ifdef HL_ENABLE_DB
    if (opts->worker_db_path)
        hl_worker_db_init(opts->worker_db_path);
#endif

    *out = ctx;
    return 0;
}

/* ── Load (deferred) ──────────────────────────────────────────────── */

int hl_app_context_load(HlAppContext *ctx, const char *entry_point)
{
    if (!ctx || !entry_point) return -1;
    if (ctx->app_loaded) return 0; /* already loaded */
    if (!ctx->rt_init) return -1;  /* runtime not initialized */
    /* Note: the gate_modules pass in hl_app_context_init only runs when
     * !opts->no_load. Callers that defer loading via hl_app_context_load
     * must also drive the resolver themselves (main.c does this in
     * hl_serve_wire_caps). Keeping load() narrow so server's existing
     * orchestration is unchanged. */
    return load_app_code(ctx, entry_point);
}

/* ── Free ──────────────────────────────────────────────────────────── */

void hl_app_context_free(HlAppContext *ctx)
{
    if (!ctx) return;

    if (ctx->rt_init && ctx->factory && ctx->factory->destroy) {
        ctx->factory->destroy(ctx->rt);
        ctx->rt = NULL;
    }

#ifdef HL_ENABLE_WASM
    if (ctx->wasm_ok && !ctx->wasm_external)
        hl_cap_wasm_destroy(&ctx->wasm_cache);
#endif

#ifdef HL_ENABLE_DB
    if (ctx->db_open) {
        ctx->db_handle.backend->close(&ctx->db_handle);
        ctx->db_handle.ctx = NULL;
        ctx->db = NULL;
    }
    ctx->db = NULL;
    ctx->db_open = 0;
#endif

    ctx->rt = NULL;
    ctx->rt_init = 0;
    ctx->app_loaded = 0;
    free(ctx);
}

/* ── Accessors ─────────────────────────────────────────────────────── */

struct sqlite3 *hl_app_context_db(HlAppContext *ctx)
{
#ifdef HL_ENABLE_DB
    return ctx ? ctx->db : NULL;
#else
    (void)ctx;
    return NULL;
#endif
}

HlDbHandle *hl_app_context_db_handle(HlAppContext *ctx)
{
#ifdef HL_ENABLE_DB
    if (!ctx || !ctx->db_handle.backend) return NULL;
    return &ctx->db_handle;
#else
    (void)ctx;
    return NULL;
#endif
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
#ifdef HL_ENABLE_DB
    return ctx ? hl_db_sqlite_cache(&ctx->db_handle) : NULL;
#else
    (void)ctx;
    return NULL;
#endif
}

int hl_app_context_is_lua(HlAppContext *ctx)
{
    /* Identify via the factory pointer — avoids a stored is_lua flag.
     * &hl_lua_vtable / &hl_js_vtable comparisons would also work but
     * the factory pointer is the canonical source of truth post-K. */
#ifdef HL_ENABLE_LUA
    extern const HlRuntimeFactory hl_lua_factory;
    return (ctx && ctx->factory == &hl_lua_factory) ? 1 : 0;
#else
    (void)ctx;
    return 0;
#endif
}

const char *hl_app_context_app_dir(HlAppContext *ctx)
{
    return ctx ? ctx->app_vfs.root_dir : NULL;
}

#ifdef HL_ENABLE_LUA
HlLua *hl_app_context_lua(HlAppContext *ctx)
{
    extern const HlRuntimeFactory hl_lua_factory;
    if (!ctx || ctx->factory != &hl_lua_factory) return NULL;
    return (HlLua *)ctx->rt;   /* base is the first field of HlLua */
}
#endif

#ifdef HL_ENABLE_JS
HlJS *hl_app_context_js(HlAppContext *ctx)
{
    extern const HlRuntimeFactory hl_js_factory;
    if (!ctx || ctx->factory != &hl_js_factory) return NULL;
    return (HlJS *)ctx->rt;
}
#endif
