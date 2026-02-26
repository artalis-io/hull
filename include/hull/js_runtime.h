/*
 * js_runtime.h — QuickJS runtime for Hull
 *
 * Manages QuickJS VM lifecycle: init, sandbox, allocator integration,
 * interrupt handler, module loader, and request dispatch.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_JS_RUNTIME_H
#define HL_JS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include "hull/hull_limits.h"
#include "hull/hull_cap.h"

/* Forward declarations for types defined in other headers */
typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct KlRequest KlRequest;
typedef struct KlResponse KlResponse;
typedef struct SHArena SHArena;
typedef struct HlAllocator HlAllocator;

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    size_t  max_heap_bytes;       /* JS heap limit (default: 64 MB) */
    size_t  max_stack_bytes;      /* JS stack limit (default: 1 MB) */
    int64_t max_instructions;     /* 0 = unlimited */
    size_t  gc_threshold;         /* bytes before cycle GC (default: 256 KB) */
} HlJSConfig;

/* Sensible defaults */
#define HL_JS_CONFIG_DEFAULT {        \
    .max_heap_bytes   = HL_JS_DEFAULT_HEAP,  \
    .max_stack_bytes  = HL_JS_DEFAULT_STACK, \
    .max_instructions = 0,                   \
    .gc_threshold     = HL_JS_GC_THRESHOLD,  \
}

/* ── Runtime context ────────────────────────────────────────────────── */

typedef struct HlJS {
    JSRuntime      *rt;
    JSContext      *ctx;

    /* Capabilities (shared with Lua runtime) */
    sqlite3        *db;
    HlFsConfig   *fs_cfg;
    HlEnvConfig  *env_cfg;
    HlHttpConfig *http_cfg;

    /* Process-level tracking allocator (NULL = raw malloc) */
    HlAllocator    *alloc;

    /* Interrupt / gas metering */
    int64_t         instruction_count;
    int64_t         max_instructions;

    /* Module search paths */
    const char     *app_dir;         /* application root directory */
    size_t          app_dir_size;    /* allocation size for tracked free */

    /* Per-request scratch arena (reset between dispatches) */
    SHArena        *scratch;

    /* Per-request response body (allocated via alloc, freed after dispatch) */
    char           *response_body;
    size_t          response_body_size;
} HlJS;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/*
 * Initialize the QuickJS runtime with custom allocator routing all
 * allocations through the process malloc (future: KlAllocator).
 * Applies sandbox: removes eval, does not load std/os modules,
 * sets memory limit, sets interrupt handler, registers module loader.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_js_init(HlJS *js, const HlJSConfig *cfg);

/*
 * Load and evaluate the application entry point (app.js).
 * This registers routes, middleware, config, etc.
 * Returns 0 on success, -1 on error.
 */
int hl_js_load_app(HlJS *js, const char *filename);

/*
 * Dispatch an HTTP request to the JS handler that matched the route.
 * Called from Hull's Keel middleware/handler bridge.
 *
 * `handler_id` is the route index registered during app loading.
 * Creates JS request/response objects, calls the handler, and
 * marshals the response back to KlResponse.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_js_dispatch(HlJS *js, int handler_id,
                     KlRequest *req, KlResponse *res);

/*
 * Run pending microtasks / jobs (e.g. from app.spawn()).
 * Call between event loop iterations with a time budget.
 * Returns number of jobs executed.
 */
int hl_js_run_jobs(HlJS *js);

/*
 * Optional: run garbage collector between requests if memory pressure
 * is high. Safe to call at any time.
 */
void hl_js_gc(HlJS *js);

/*
 * Reset per-request state (instruction counter, etc.).
 * Call before each request dispatch.
 */
void hl_js_reset_request(HlJS *js);

/*
 * Destroy the QuickJS runtime and free all resources.
 */
void hl_js_free(HlJS *js);

/* ── Module registration ────────────────────────────────────────────── */

/*
 * Register all hull:* built-in modules (app, db, json, session, etc.).
 * Called internally by hl_js_init().
 */
int hl_js_register_modules(HlJS *js);

/* ── Error reporting ────────────────────────────────────────────────── */

/*
 * Print the current JS exception to stderr with stack trace.
 * Call after any JS_IsException() check.
 */
void hl_js_dump_error(HlJS *js);

#endif /* HL_JS_RUNTIME_H */
