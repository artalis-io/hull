/*
 * runtime.h — Pluggable runtime vtable
 *
 * Defines HlRuntime (shared base) and HlRuntimeVtable so main.c
 * can drive Lua or QuickJS through a single polymorphic interface.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_H
#define HL_RUNTIME_H

#include <stddef.h>
#include <stdint.h>  /* uint32_t / int64_t / uint64_t in wasm_config */

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef struct HlTestCaseResult HlTestCaseResult;
typedef struct KlRouter KlRouter;
typedef struct HlFsConfig HlFsConfig;
typedef struct HlEnvConfig HlEnvConfig;
typedef struct HlHttpConfig HlHttpConfig;
typedef struct HlSmtpConfig HlSmtpConfig;
typedef struct HlManifest HlManifest;
typedef struct HlVfs HlVfs;
typedef struct HlWasmCache HlWasmCache;
typedef struct HlGpuCtx HlGpuCtx;
typedef struct HlDbHandle HlDbHandle;
typedef struct HlWsRegistry HlWsRegistry;
typedef struct KlServer KlServer;
typedef struct KlThreadPool KlThreadPool;
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

typedef struct HlRuntimeVtable {
    int   (*init)(HlRuntime *rt, const void *config);
    int   (*load_app)(HlRuntime *rt, const char *filename);
    int   (*wire_routes_server)(HlRuntime *rt, KlServer *server,
                                void *(*alloc_fn)(size_t));
    int   (*extract_manifest)(HlRuntime *rt, HlManifest *out);

    /* Walk every registered route + middleware. Either callback may be
     * NULL to skip that category. Used by agent_lib::routes for
     * introspection — folds parallel Lua/JS code paths into one. */
    void  (*enumerate_routes)(HlRuntime *rt, HlRouteCb cb, void *user);
    void  (*enumerate_middleware)(HlRuntime *rt, HlMiddlewareCb cb, void *user);

    /* Wire app routes + register the `test` global onto the router.
     * Called once per runtime by the shared test runner before any
     * test files load. Returns 0 on success, -1 if no app routes
     * were registered (caller should report "no routes"). */
    int   (*test_setup)(HlRuntime *rt, KlRouter *router);

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

    /* Glob pattern for test files in this runtime (e.g. "test_*.lua").
     * Used by the shared test runner — avoids switching on rt->vt->name
     * to pick the file pattern. */
    const char *test_file_pattern;
} HlRuntimeVtable;

struct HlRuntime {
    const HlRuntimeVtable *vt;
    HlDbHandle   *db_handle;      /* app queries (via vtable) */
    HlDbHandle   *hull_db_handle; /* hull internal (_hull_* tables) */
    HlAllocator  *alloc;
    HlFsConfig   *fs_cfg;
    HlEnvConfig  *env_cfg;
    HlHttpConfig *http_cfg;
    HlSmtpConfig *smtp_cfg;
    const char   *csp_policy;  /* CSP header value for HTML responses (NULL = none) */
    const HlVfs  *app_vfs;       /* app entries (embedded + dev fallback) */
    const HlVfs  *platform_vfs;  /* stdlib entries (always embedded) */
    KlThreadPool *thread_pool;   /* worker pool for async work (NULL if not created) */
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
#ifdef HL_ENABLE_GPU
    HlGpuCtx *gpu_ctx;                /* GPU compute context (NULL if disabled) */
#endif
};

#endif /* HL_RUNTIME_H */
