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

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef struct HlFsConfig HlFsConfig;
typedef struct HlEnvConfig HlEnvConfig;
typedef struct HlHttpConfig HlHttpConfig;
typedef struct HlSmtpConfig HlSmtpConfig;
typedef struct HlManifest HlManifest;
typedef struct HlStmtCache HlStmtCache;
typedef struct HlVfs HlVfs;
typedef struct HlWasmCache HlWasmCache;
typedef struct HlGpuCtx HlGpuCtx;
typedef struct sqlite3 sqlite3;
typedef struct KlServer KlServer;
typedef struct KlThreadPool KlThreadPool;
typedef struct HlRuntime HlRuntime;

typedef struct HlRuntimeVtable {
    int   (*init)(HlRuntime *rt, const void *config);
    int   (*load_app)(HlRuntime *rt, const char *filename);
    int   (*wire_routes_server)(HlRuntime *rt, KlServer *server,
                                void *(*alloc_fn)(size_t));
    int   (*extract_manifest)(HlRuntime *rt, HlManifest *out);
    void  (*destroy)(HlRuntime *rt);
    const char *name;
} HlRuntimeVtable;

struct HlRuntime {
    const HlRuntimeVtable *vt;
    sqlite3      *db;
    HlStmtCache  *stmt_cache;
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
