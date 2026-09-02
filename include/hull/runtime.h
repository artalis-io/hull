/**
 * @file runtime.h
 * @brief Pluggable runtime vtable.
 *
 * Defines #HlRuntime (shared base) and #HlRuntimeVtable so main.c can
 * drive Lua or QuickJS through a single polymorphic interface. Each
 * concrete runtime (`HlLua`, `HlJS`) keeps `HlRuntime` as its first
 * member so the base pointer can be cast back to the concrete type.
 *
 * @par Lifecycle:
 *   1. The factory registry (`runtime/factory.h`) selects a runtime
 *      based on the entry-point file extension (`.lua` / `.js`).
 *   2. The factory's `create()` returns a heap-allocated runtime with
 *      `HlRuntime::vt` pointing at the appropriate vtable.
 *   3. The runtime loads the app, extracts the manifest, wires routes
 *      onto Keel's router via the vtable hooks, and serves.
 *   4. `vt->destroy()` tears down all module state and frees the
 *      runtime.
 *
 * Stability: Tier 3 (internal API; will evolve with the runtime split).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_H
#define HL_RUNTIME_H

#include <stddef.h>
#include <stdint.h>  /* uint32_t / int64_t / uint64_t in wasm_config */

#include "hull/limits/core.h"  /* HL_MODULE_BITSET_WORDS / other limits */
#include "hull/manifest.h"     /* HL_MANIFEST_MAX_MODULES (import tracker) */

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef struct HlAsyncBackendCtx  HlAsyncBackendCtx;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;
typedef struct HlNetBackendCtx    HlNetBackendCtx;
typedef struct HlTestCaseResult HlTestCaseResult;
typedef struct KlHttpRouter KlHttpRouter;
typedef struct HlFsConfig HlFsConfig;
typedef struct HlEnvConfig HlEnvConfig;
typedef struct HlHttpConfig HlHttpConfig;
typedef struct HlSmtpConfig HlSmtpConfig;
typedef struct HlSmtpServerCtx HlSmtpServerCtx;
typedef struct HlManifest HlManifest;
typedef struct HlResolvedModuleSet HlResolvedModuleSet;
typedef struct HlVfs HlVfs;
typedef struct HlWasmCache HlWasmCache;
typedef struct HlGpuCtx HlGpuCtx;
typedef struct HlDbHandle HlDbHandle;
typedef struct HlDbRegistry HlDbRegistry;
typedef struct HlWsRegistry HlWsRegistry;
typedef struct KlHttpServer KlHttpServer;
typedef struct HlRuntime HlRuntime;

/*
 * Route enumeration callback. Invoked once per registered route handler.
 * `method` is the HTTP method ("GET", "POST", ...) or "*" for any.
 * `pattern` is the path pattern (e.g. "/users/:id").
 */
typedef void (*HlRouteCb)(void *user, const char *method, const char *pattern);

/*
 * Middleware enumeration callback. Invoked once per registered middleware.
 * `phase` is "pre" or "post" (pre-body vs post-body).
 */
typedef void (*HlMiddlewareCb)(void *user,
                               const char *method, const char *pattern,
                               const char *phase);

/*
 * Runtime discriminator, carried in the vtable. Lets consumers branch on the
 * concrete runtime (rt->vt->kind == HL_RT_LUA) WITHOUT taking the address of a
 * concrete runtime symbol (&hl_lua_vtable), so base-lib code links cleanly
 * against a build that composes only one runtime. See docs/runtime_feature_phase1.md.
 */
typedef enum {
    HL_RT_LUA = 1,
    HL_RT_JS  = 2,
} HlRuntimeKind;

typedef struct HlRuntimeVtable {
    int   (*init)(HlRuntime *rt, const void *config);
    int   (*load_app)(HlRuntime *rt, const char *filename);
    int   (*wire_routes_server)(HlRuntime *rt, KlHttpServer *server,
                                void *(*alloc_fn)(size_t));
    int   (*extract_manifest)(HlRuntime *rt, HlManifest *out);

    /* Walk every registered route + middleware. Either callback may be
     * NULL to skip that category. Used by agent_lib::routes for
     * introspection - folds parallel Lua/JS code paths into one. */
    void  (*enumerate_routes)(HlRuntime *rt, HlRouteCb cb, void *user);
    void  (*enumerate_middleware)(HlRuntime *rt, HlMiddlewareCb cb, void *user);

    /* Wire app routes + register the `test` global onto the router.
     * Called once per runtime by the shared test runner before any
     * test files load. Returns 0 on success, -1 if no app routes
     * were registered (caller should report "no routes"). */
    int   (*test_setup)(HlRuntime *rt, KlHttpRouter *router);

    /* Run a single test file end-to-end:
     *   1. clear test cases left over from a previous file
     *   2. load + execute file_path (registers test cases via test(name,fn))
     *   3. run all registered test cases, filling results[] up to max_results
     *
     * On success: returns 0, fills *file_total/passed/failed and results[].
     * On load error: returns -1, sets *load_err to a static string.
     * Used by the shared test runner (src/hull/test_runner.c). */
    int   (*run_test_file)(HlRuntime *rt, const char *file_path,
                           HlTestCaseResult *results, int max_results,
                           int *file_total, int *file_passed, int *file_failed,
                           const char **load_err);

    void  (*destroy)(HlRuntime *rt);
    const char *name;

    /* Runtime discriminator (HL_RT_LUA / HL_RT_JS). Consumers branch on this
     * instead of `rt->vt == &hl_<rt>_vtable` so no base TU references a concrete
     * runtime symbol (the precondition for a runtime-slim link). */
    HlRuntimeKind kind;

    /* Glob pattern for test files in this runtime (e.g. "test_*.lua").
     * Used by the shared test runner - avoids switching on rt->vt->name
     * to pick the file pattern. */
    const char *test_file_pattern;

    /* Returns 1 if the loaded app registered app.main(fn), 0 otherwise.
     * Returns 0 if the runtime hasn't been told to look (no app loaded).
     * Used by serve.c to decide whether to invoke the startup hook. */
    int   (*has_main)(HlRuntime *rt);

    /* Returns 1 if the app registered any route, middleware, timer,
     * WebSocket endpoint, or SSE endpoint. Used by serve.c to decide
     * whether to enter the server loop after app.main returns (or as
     * the primary dispatch when there's no main). 0 means "nothing
     * for the server loop to do - just exit when main completes." */
    int   (*has_server_handlers)(HlRuntime *rt);

    /* Run the registered app.main(fn) and return its exit code via
     * `exit_code_out` (clamped to 0..255).
     *
     *   server          : Keel server whose event loop drives async ops
     *                     inside main (compute.async, gpu.async, http.fetch,
     *                     hull.sleep). May be NULL only if the runtime can
     *                     guarantee main is purely synchronous; otherwise
     *                     async ops will fail with "requires an active
     *                     server".
     *   argv/argc       : positional args passed to main as `ctx.args`
     *   env_allowlist   : NULL-terminated array of env-var names allowed
     *                     by the manifest; only these populate `ctx.env`.
     *                     NULL or empty → `ctx.env` is an empty table.
     *
     * Returns 0 on success, -1 if main was not registered, or if the
     * function threw / failed to coerce a return value. On failure,
     * exit_code_out is set to 1. */
    int   (*run_main)(HlRuntime *rt, KlHttpServer *server,
                      int argc, char **argv,
                      const char *const *env_allowlist,
                      int *exit_code_out);
} HlRuntimeVtable;

struct HlRuntime {
    const HlRuntimeVtable *vt;
    HlDbRegistry *db_registry;    /* owns all DB connections, incl. "default".
                                   * Resolve the default via
                                   * hl_db_registry_default(); there is no
                                   * separate default-handle field. NULL under
                                   * --no-db / pure-compute builds. */
    HlAllocator  *alloc;
    HlFsConfig   *fs_cfg;
    /* kv.open(dsn) allowlist policy (manifest kv.dynamic). Borrowed pointer into
     * the sealed manifest; NULL means no policy (kv.open fails closed). */
    const HlManifestKvDynamic *kv_policy;
    HlEnvConfig  *env_cfg;
    HlHttpConfig *http_cfg;
    HlSmtpConfig *smtp_cfg;
    HlSmtpServerCtx *smtp_async;  /* model-2 async SMTP ctx (admission+registry+trust) */
    const char   *csp_policy;  /* CSP header value for HTML responses (NULL = none) */
    const HlVfs  *app_vfs;       /* app entries (embedded + dev fallback) */
    const HlVfs  *platform_vfs;  /* stdlib entries (always embedded) */
    /* Frozen module set: which first-party stdlib modules the app may
     * import. Borrowed pointer; lifetime exceeds the runtime. NULL means
     * no gating (legacy entry points: test runner, agent, mcp). When
     * non-NULL, gating fires for any registry-known name not in the set. */
    const HlResolvedModuleSet *module_set;
    /* Pre-manifest import tracker. Top-level Lua require() / JS import
     * statements run before module_set is wired (the manifest hasn't
     * been extracted yet, so the resolver hasn't run). To prevent
     * silent-bypass, the gate records every registry-known canonical
     * name it lets through during that window into this fixed-size
     * array. After the resolver runs in serve.c, the validator walks
     * the array and rejects any name that the resolved set doesn't
     * cover. See hl_import_tracker_record / _validate in
     * module_resolver.c.
     *
     * Capacity is HL_MANIFEST_MAX_MODULES because no app can declare
     * (or transitively use) more registry-known modules than that. */
    int import_tracker_count;
    const char *import_tracker_names[HL_MANIFEST_MAX_MODULES];
    HlAsyncBackendPool *thread_pool; /* worker pool for async work (NULL if not created) */
    /* HlAsyncBackend context - the event loop primitives layer. In
     * server-mode builds this is a wrap around the KlHttpServer's
     * embedded event ctx; in CLI-mode builds it's an independent
     * loop created via backend->init(). Consumers use the vtable
     * (hl_async_backend()->timer_add(rt->async_ctx, ...)) instead
     * of touching KlHttpServer / KlEventCtx directly. NULL means async
     * primitives are unavailable on this runtime (rare). */
    HlAsyncBackendCtx *async_ctx;
    /* HlNetBackend context - the HTTP/connection lifecycle layer.
     * Borrowed from HlServerState (which owns the wrap around KlHttpServer).
     * Consumers route connection-bound async-op suspend/complete
     * (hl_net_op_suspend / hl_net_op_complete) through this so they
     * don't need to know about Keel directly. NULL in CLI-mode builds
     * (no net layer). */
    HlNetBackendCtx *net_ctx;
    const char   *db_path;       /* SQLite file path (borrowed, for worker connections) */
    struct KlCompressConfig *compress;  /* response compression config (NULL = disabled) */
    HlWsRegistry *ws_registry;          /* WebSocket connection registry (NULL if no WS endpoints) */
#ifdef HL_ENABLE_WASM
    HlWasmCache *wasm_cache;           /* WAMR compute module cache (NULL if disabled) */
    struct {
        uint32_t heap_size;   /* ceiling: 0 = use compile-time default */
        uint32_t stack_size;
        int64_t  gas;
        uint64_t max_input;
        uint64_t max_output;
    } wasm_config;                     /* three-tier resolved limits (CLI > manifest > defaults) */
#endif
    HlGpuCtx *gpu_ctx;                /* GPU compute context; NULL when no backend
                                        * (base build without a composed gpu feature) */
    /* Phase gate for the `app.X` registration bindings (app.get / use /
     * use_post / ws / sse / every / daily).  Set to 1 by serve.c
     * (hl_serve_wire_routes) after the route registry has been flushed
     * to Keel and before kl_http_server_freeze.
     *
     * Once set, the bindings throw a structured Lua error / JS exception
     * - "app.X() can only be called at app startup ..." - instead of
     * silently accepting the registration and stashing it in a table
     * that no consumer reads (the underlying KlHttpRouter is also frozen
     * post this point, so even if the binding did push, kl_http_router_add
     * would return -1).
     *
     * Top-level app code, manifest extraction, and app.main(fn) all run
     * BEFORE the flag is flipped, so they continue to register normally.
     * Request handlers, app.every/app.daily timer callbacks, and any
     * other code that executes after the serve loop begins see the flag
     * set and get the clear error.
     *
     * Build-time gated: only meaningful on HL_ENABLE_HTTP_SERVER builds
     * (CLI builds don't compile the registration bindings).  Kept on
     * the base struct unconditionally so consumers don't need ifdefs. */
    int registration_closed;
};

#endif /* HL_RUNTIME_H */
