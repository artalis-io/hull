/*
 * app_context.h — Reusable application context for commands
 *
 * Consolidates VFS + DB + runtime initialization shared by
 * test, agent, MCP commands, and the main server. Each consumer
 * creates an HlAppContext instead of duplicating the init sequence.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_APP_CONTEXT_H
#define HL_APP_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct sqlite3 sqlite3;
typedef struct HlAllocator HlAllocator;
typedef struct HlRuntime HlRuntime;
typedef struct HlVfs HlVfs;
typedef struct HlStmtCache HlStmtCache;
typedef struct HlWasmCache HlWasmCache;
typedef struct HlGpuCtx HlGpuCtx;
typedef struct KlThreadPool KlThreadPool;
struct KlCompressConfig;

typedef struct {
    const char   *app_dir;        /* required */
    const char   *entry_point;    /* NULL = auto-detect (app.lua / app.js) */
    const char   *db_path;        /* NULL = ":memory:" */
    int           no_migrate;     /* 1 = skip migrations */
    int           sandbox;        /* 1 = sandboxed runtime (default) */
    HlAllocator  *alloc;          /* NULL = use raw malloc */

    /* Server-specific runtime wiring (all optional, NULL/0 = not used) */
    KlThreadPool             *thread_pool;      /* wired to rt->thread_pool */
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
     * db_handle and hull_db_handle will be NULL, db global not registered. */
    int           no_db;

    /* Deferred loading: 1 = init runtime but don't load app.
     * Use hl_app_context_load() to load app code later. */
    int           no_load;
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
sqlite3      *hl_app_context_db(HlAppContext *ctx);
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
