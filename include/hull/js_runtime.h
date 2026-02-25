/*
 * js_runtime.h — QuickJS runtime for Hull
 *
 * Manages QuickJS VM lifecycle: init, sandbox, allocator integration,
 * interrupt handler, module loader, and request dispatch.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_JS_RUNTIME_H
#define HULL_JS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include "hull/hull_cap.h"

/* Forward declarations for types defined in other headers */
typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct KlRequest KlRequest;
typedef struct KlResponse KlResponse;

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    size_t  max_heap_bytes;       /* JS heap limit (default: 64 MB) */
    size_t  max_stack_bytes;      /* JS stack limit (default: 1 MB) */
    int64_t max_instructions;     /* 0 = unlimited */
    size_t  gc_threshold;         /* bytes before cycle GC (default: 256 KB) */
} HullJSConfig;

/* Sensible defaults */
#define HULL_JS_CONFIG_DEFAULT {        \
    .max_heap_bytes   = 64 * 1024 * 1024,  \
    .max_stack_bytes  = 1  * 1024 * 1024,  \
    .max_instructions = 0,                  \
    .gc_threshold     = 256 * 1024,         \
}

/* ── Runtime context ────────────────────────────────────────────────── */

typedef struct HullJS {
    JSRuntime      *rt;
    JSContext      *ctx;

    /* Capabilities (shared with Lua runtime) */
    sqlite3        *db;
    HullFsConfig   *fs_cfg;
    HullEnvConfig  *env_cfg;
    HullHttpConfig *http_cfg;

    /* Interrupt / gas metering */
    int64_t         instruction_count;
    int64_t         max_instructions;

    /* Module search paths */
    const char     *app_dir;         /* application root directory */

    /* Route handlers (JS function references) */
    /* Stored in JS globalThis.__hull_routes internally */
} HullJS;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/*
 * Initialize the QuickJS runtime with custom allocator routing all
 * allocations through the process malloc (future: KlAllocator).
 * Applies sandbox: removes eval, does not load std/os modules,
 * sets memory limit, sets interrupt handler, registers module loader.
 *
 * Returns 0 on success, -1 on error.
 */
int hull_js_init(HullJS *js, const HullJSConfig *cfg);

/*
 * Load and evaluate the application entry point (app.js).
 * This registers routes, middleware, config, etc.
 * Returns 0 on success, -1 on error.
 */
int hull_js_load_app(HullJS *js, const char *filename);

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
int hull_js_dispatch(HullJS *js, int handler_id,
                     KlRequest *req, KlResponse *res);

/*
 * Run pending microtasks / jobs (e.g. from app.spawn()).
 * Call between event loop iterations with a time budget.
 * Returns number of jobs executed.
 */
int hull_js_run_jobs(HullJS *js);

/*
 * Optional: run garbage collector between requests if memory pressure
 * is high. Safe to call at any time.
 */
void hull_js_gc(HullJS *js);

/*
 * Reset per-request state (instruction counter, etc.).
 * Call before each request dispatch.
 */
void hull_js_reset_request(HullJS *js);

/*
 * Destroy the QuickJS runtime and free all resources.
 */
void hull_js_free(HullJS *js);

/* ── Module registration ────────────────────────────────────────────── */

/*
 * Register all hull:* built-in modules (app, db, json, session, etc.).
 * Called internally by hull_js_init().
 */
int hull_js_register_modules(HullJS *js);

/* ── Error reporting ────────────────────────────────────────────────── */

/*
 * Print the current JS exception to stderr with stack trace.
 * Call after any JS_IsException() check.
 */
void hull_js_dump_error(HullJS *js);

#endif /* HULL_JS_RUNTIME_H */
