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
 * Stability: Tier 3 (internal - used across commands but not part of
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
typedef struct HlRuntimeFactory HlRuntimeFactory;
typedef struct HlVfs HlVfs;
typedef struct HlStmtCache HlStmtCache;
typedef struct HlWasmCache HlWasmCache;
typedef struct HlGpuCtx HlGpuCtx;
typedef struct HlWasmCliLimits HlWasmCliLimits;
typedef struct HlDbHandle HlDbHandle;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;
struct KlCompressConfig;

typedef struct HlAppContextOpts {
    const char   *app_dir;        /* required */
    const char   *entry_point;    /* NULL = auto-detect (app.lua / app.js) */
    const char   *db_path;        /* NULL = ":memory:" */
    int           no_migrate;     /* 1 = skip migrations */
    int           sandbox;        /* NOT READ by the context: the kernel
                                   * sandbox is applied by the entry point
                                   * (serve.c / serve_cli.c). Callers that
                                   * do not apply one - hull test, agent
                                   * introspection, MCP - run the app's
                                   * top-level code without it. */
    HlAllocator  *alloc;          /* NULL = use raw malloc */

    /* Runtime wiring for server and CLI entry points (optional). */
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
    /* The operator's --wasm-* overrides (NULL = none). With gate_modules the
     * context resolves them with the manifest's `wasm` limits onto
     * rt->wasm_config (include/hull/wasm_config.h), the ceilings the compute
     * bindings clamp to - as serve.c does for the server. */
    const HlWasmCliLimits *wasm_cli;
    int           gpu_device;       /* -1 = default */

    /* Pure compute mode: 1 = skip database entirely.
     * db_handle / db_registry will be NULL, db global not registered. */
    int           no_db;

    /* Deferred loading: 1 = init runtime but don't load app.
     * Use hl_app_context_load() to load app code later. */
    int           no_load;

    /* Tolerate a top-level error while loading the app (read-only extraction
     * paths only). app.manifest() stores the manifest the moment it is called,
     * so an app that errors LATER in its top-level code (e.g. does DB work at
     * module scope against an unmigrated :memory: schema) still has an
     * extractable manifest. With this set, load_app's failure is logged but not
     * fatal: init succeeds, app_loaded stays 0, gate_modules is skipped, and the
     * caller can still call extract_manifest. Consumers that RUN the app
     * (serve/test) leave this 0 so a broken app fails loudly. */
    int           tolerate_load_error;

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

    /* --keep-internal-in-app-db: the stdlib's _hull_* tables stay on the
     * default connection (the pre-§7.2 layout) instead of the Hull-owned
     * internal database (cap/db_internal.h). The default internal path is
     * still recorded, so the startup step can move tables back. */
    int           keep_internal_in_app_db;
    /* 1 when db_path is the OPERATOR's (-d): never replaced by the
     * manifest's. 0 when it is the implicit default (<app_dir>/data.db under
     * serve, :memory: elsewhere), which manifest_default_db may replace. */
    int           db_path_explicit;
    /* 1 (the runners, `hull migrate`, the agent's database commands): when
     * the manifest declares `databases.named.default` and db_path is not the
     * operator's, the app's default database is THAT one - re-pointed after
     * the manifest is read, migrated, the internal database configured for
     * it, and a file the implicit default created meanwhile removed. The
     * way an app.main tool keeps its database under "@state/"
     * (shared/state_dir.h). 0 (hull test, inspection commands): the implicit
     * default stays. */
    int           manifest_default_db;
    /* With manifest_default_db: only LOOK (the read-only `hull agent db` /
     * `sql` commands, audit 44). The manifest's default is resolved and
     * reported by hl_app_context_default_db_path, but neither opened nor
     * migrated, and the state directory is not made - a missing one is no
     * state directory - so inspecting an app creates no database. */
    int           manifest_default_db_peek;
} HlAppContextOpts;

struct HlManifest;
typedef struct HlAppContext HlAppContext;

/*
 * Initialize an app context: open DB, init VFS, create runtime, load app.
 * If opts->no_load is set, runtime is initialized but app is not loaded -
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
 * does not need to know about SQLite - callers that want to use the result
 * include <sqlite3.h> themselves. New code should prefer
 * hl_app_context_db_handle() and the HlDbBackend vtable.
 */
struct sqlite3 *hl_app_context_db(HlAppContext *ctx);

/*
 * Vtable-based DB handle. Returns NULL if the context has no DB.
 * Backend-agnostic - works with SQLite today, with future backends
 * (PostgreSQL, DuckDB) transparently.
 */
HlDbHandle   *hl_app_context_db_handle(HlAppContext *ctx);

HlRuntime    *hl_app_context_runtime(HlAppContext *ctx);
const HlVfs  *hl_app_context_app_vfs(HlAppContext *ctx);
const HlVfs  *hl_app_context_platform_vfs(HlAppContext *ctx);
HlStmtCache  *hl_app_context_stmt_cache(HlAppContext *ctx);
int           hl_app_context_is_lua(HlAppContext *ctx);
const char   *hl_app_context_app_dir(HlAppContext *ctx);
/* The app's per-user state directory (shared/state_dir.h; manifest
 * state.name), resolved and made at init, or NULL when none is declared. The
 * app.main runner grants it to the kernel sandbox. */
const char   *hl_app_context_state_dir(HlAppContext *ctx);
/* The app's default database DSN when the manifest's databases.named.default
 * replaced the implicit one (HlAppContextOpts.manifest_default_db) -
 * confined, absolute for a file - else NULL. What `hull migrate` and the
 * kernel sandbox take as the default database. */
const char   *hl_app_context_default_db_path(HlAppContext *ctx);
/* The manifest step for the databases (gap 3): tell the registry the state
 * directory (NULL = none) so "@state/" DSNs confine to it, and - under
 * HlAppContextOpts.manifest_default_db with no operator -d - make the
 * manifest's `databases.named.default` the app's default (closing the
 * implicit one, migrating, configuring the internal database). The context
 * runs it itself when it loads the app; serve.c, which defers the load,
 * calls it once it has the manifest, before the internal-table move. 0 /
 * -1 (reported). */
int           hl_app_context_manifest_db(HlAppContext *ctx, const struct HlManifest *m,
                                         const char *state_dir);
/* The resolved runtime factory (NULL before resolution). Agnostic; the
 * runtime-typed accessors below build on it. */
const HlRuntimeFactory *hl_app_context_factory(HlAppContext *ctx);

#ifdef HL_ENABLE_LUA
struct HlLua;
struct HlLua *hl_app_context_lua(HlAppContext *ctx);  /* NULL if JS */
#endif
#ifdef HL_ENABLE_JS
struct HlJS;
struct HlJS *hl_app_context_js(HlAppContext *ctx);    /* NULL if Lua */
#endif

#endif /* HL_APP_CONTEXT_H */
