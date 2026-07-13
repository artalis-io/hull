/**
 * @file app_context.h
 * @brief Reusable application context for commands.
 *
 * Consolidates the `VFS + DB + runtime` init sequence shared by
 * `hull test`, `hull agent`, `hull mcp`, and the dev server in
 * `main.c`. Each consumer calls @ref hl_app_context_init instead of
 * duplicating the init sequence.
 *
 * @par Lifecycle:
 *   `hl_app_context_init` → use → `hl_app_context_free`.
 *
 * Stability: Tier 3 (internal — used across commands but not part of
 * the v0.1.0 public surface).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_APP_CONTEXT_H
#define HL_APP_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef struct HlRuntime HlRuntime;
typedef struct HlVfs HlVfs;
typedef struct HlStmtCache HlStmtCache;
typedef struct HlWasmCache HlWasmCache;
typedef struct HlGpuCtx HlGpuCtx;
typedef struct HlDbHandle HlDbHandle;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;
struct KlCompressConfig;

typedef struct HlAppContextOpts {
    const char   *app_dir;        /* required */
    const char   *entry_point;    /* NULL = auto-detect (app.lua / app.js) */
    const char   *db_path;        /* NULL = ":memory:" */
    int           no_migrate;     /* 1 = skip migrations */
    int           sandbox;        /* 1 = sandboxed runtime (default) */
    HlAllocator  *alloc;          /* NULL = use raw malloc */

    /* Server-specific runtime wiring (all optional, NULL/0 = not used) */
    HlAsyncBackendPool       *thread_pool;      /* wired to rt->thread_pool */
    const char               *worker_db_path;   /* for hl_worker_db_init */
    struct KlCompressConfig  *compress;         /* wired to rt->compress */

    /* CLI limit overrides (0 = use defaults) */
    long          heap_limit;
    long          stack_limit;       /* JS only */
    long          instruction_limit;

    /* External WASM/GPU caches (server owns these, context just wires them) */
    HlWasmCache  *wasm_cache;       /* NULL = init internal cache */
    HlGpuCtx     *gpu_ctx;          /* NULL = no GPU */
    int           gpu_device;       /* -1 = default */

    /* Pure compute mode: 1 = skip database entirely.
     * db_handle / db_registry will be NULL, db global not registered. */
    int           no_db;

    /* Deferred loading: 1 = init runtime but don't load app.
     * Use hl_app_context_load() to load app code later. */
    int           no_load;

    /* Module-system gating:
     *   1 = run the resolver after manifest extraction and wire the
     *       result onto rt->module_set so require/import enforce the
     *       declared modules. Used by every consumer that runs user
     *       app code: hull test, hull agent (warm context), hull mcp.
     *       hull dev / serve does its own resolution + wiring in
     *       main.c::hl_serve_wire_caps.
     *   0 = leave rt->module_set = NULL (permissive). Reserved for
     *       read-only introspection paths that load but don't execute
     *       user code under the gate (e.g. tooling that just wants
     *       the runtime + VFS).
     *
     * Defaults to 0 so a zero-initialized opts struct is still a
     * legal (permissive) call; new consumers should set it explicitly. */
    int           gate_modules;
} HlAppContextOpts;

typedef struct HlAppContext HlAppContext;

/*
 * Initialize an app context: open DB, init VFS, create runtime, load app.
 * If opts->no_load is set, runtime is initialized but app is not loaded —
 * call hl_app_context_load() separately.
 * Returns 0 on success, -1 on error.
 */
int hl_app_context_init(HlAppContext **out, const HlAppContextOpts *opts);

/*
 * Load app code into an already-initialized context.
 * Only needed when opts->no_load was set during init.
 * Returns 0 on success, -1 on error.
 */
int hl_app_context_load(HlAppContext *ctx, const char *entry_point);

/*
 * Free all resources. Safe to call on NULL. Idempotent.
 */
void hl_app_context_free(HlAppContext *ctx);

/* Accessors */

/*
 * Raw SQLite handle. Returns NULL if the context has no DB (no_db mode)
 * or if the active backend isn't SQLite. The return type is declared as
 * `struct sqlite3 *` rather than the `sqlite3` typedef so that this header
 * does not need to know about SQLite — callers that want to use the result
 * include <sqlite3.h> themselves. New code should prefer
 * hl_app_context_db_handle() and the HlDbBackend vtable.
 */
struct sqlite3 *hl_app_context_db(HlAppContext *ctx);

/*
 * Vtable-based DB handle. Returns NULL if the context has no DB.
 * Backend-agnostic — works with SQLite today, with future backends
 * (PostgreSQL, DuckDB) transparently.
 */
HlDbHandle   *hl_app_context_db_handle(HlAppContext *ctx);

HlRuntime    *hl_app_context_runtime(HlAppContext *ctx);
const HlVfs  *hl_app_context_app_vfs(HlAppContext *ctx);
const HlVfs  *hl_app_context_platform_vfs(HlAppContext *ctx);
HlStmtCache  *hl_app_context_stmt_cache(HlAppContext *ctx);
int           hl_app_context_is_lua(HlAppContext *ctx);
const char   *hl_app_context_app_dir(HlAppContext *ctx);

#ifdef HL_ENABLE_LUA
struct HlLua;
struct HlLua *hl_app_context_lua(HlAppContext *ctx);  /* NULL if JS */
#endif
#ifdef HL_ENABLE_JS
struct HlJS;
struct HlJS *hl_app_context_js(HlAppContext *ctx);    /* NULL if Lua */
#endif

#endif /* HL_APP_CONTEXT_H */
