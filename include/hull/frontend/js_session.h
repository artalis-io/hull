/*
 * hull/frontend/js_session.h — the RESTRICTED QuickJS tooling runtime (Slice 1).
 *
 * A self-contained QuickJS runtime + context that runs ONLY trusted, bundled Hull tooling
 * JavaScript (the JS source frontend) to PARSE / ANALYZE application source passed as DATA.
 * It is deliberately more restricted than the application JS runtime: no db/fs/http/env/
 * crypto/compute/gpu/network/spawn/module-loading-of-app-source, no eval / Function at the
 * JS level, bounded memory / stack / instructions / source-size / result-size. Application
 * JavaScript is never executed here.
 *
 * C OWNS this runtime (creation, invocation, limits, transport validation, teardown). The
 * Lua project layer never obtains or operates it (design: docs/javascript_source_frontend_
 * design.md, ratified). The session loads bundled `hull:*` tooling modules from the cli-js
 * VFS ONLY. Slice 1 proves the runtime + byte transport + module loading + limits +
 * exception->diagnostic conversion + lifecycle with a trivial `hull:probe` entry; Slice 2+
 * add the real lexer/parser.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_FRONTEND_JS_SESSION_H
#define HL_FRONTEND_JS_SESSION_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlJsSession HlJsSession;

/* Tooling-runtime limits. Every breach becomes a structured INDETERMINATE js.limit.*
 * diagnostic (never a raw exception or crash). Defaults are the ratified D6 starting
 * points; calibrate against measured corpus high-water marks. */
typedef struct {
    size_t  max_heap_bytes;    /* QuickJS heap limit (JS_SetMemoryLimit) */
    size_t  max_stack_bytes;   /* QuickJS stack limit (JS_SetMaxStackSize) */
    int64_t max_instructions;  /* interrupt budget per invocation (0 = unlimited) */
    size_t  max_source_bytes;  /* reject a source larger than this -> js.limit.bytes */
    size_t  max_result_bytes;  /* reject a result JSON larger than this -> js.limit.result */
} HlJsSessionLimits;

#define HL_JS_SESSION_LIMITS_DEFAULT {                 \
    .max_heap_bytes   = (size_t)128 * 1024 * 1024,     \
    .max_stack_bytes  = (size_t)1 * 1024 * 1024,       \
    .max_instructions = (int64_t)150 * 1000 * 1000,    \
    .max_source_bytes = (size_t)4 * 1024 * 1024,       \
    .max_result_bytes = (size_t)16 * 1024 * 1024,      \
}

/* Create a restricted tooling session (its own JSRuntime + JSContext). `limits` NULL =
 * defaults. Returns NULL on allocation failure. */
HlJsSession *hl_js_session_create(const HlJsSessionLimits *limits);

/* Invoke a bundled tooling entry: evaluate `module` (a `hull:*` cli-js module that sets
 * `globalThis.__hull_frontend`) then call its `method` (e.g. "analyze") with
 * (ArrayBuffer(src[0..src_len)), path, JSON.parse(options_json | "null")). Source crosses
 * as raw bytes (length-aware, NUL-safe) -- never a NUL-terminated string.
 *
 * On success returns 0 and sets *out_json to a malloc'd NUL-terminated JSON string (caller
 * frees) of the method's return value, *out_len to its byte length. On ANY failure
 * (limit / exception / bad transport / internal) returns -1 and sets *out_json to a
 * structured INDETERMINATE diagnostic JSON:
 *   {"status":"indeterminate","diagnostics":[{"severity":"error","code":"js.limit.*|js.internal","message":...}]}
 * Never raises a QuickJS exception across this boundary; never crashes. */
int hl_js_session_analyze(HlJsSession *s, const char *module, const char *method,
                          const uint8_t *src, size_t src_len,
                          const char *path, const char *options_json,
                          char **out_json, size_t *out_len);

/* Destroy the session (JS_FreeContext + JS_FreeRuntime). Safe on NULL. */
void hl_js_session_destroy(HlJsSession *s);

#endif /* HL_FRONTEND_JS_SESSION_H */
