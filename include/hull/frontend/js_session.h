/*
 * hull/frontend/js_session.h - the RESTRICTED QuickJS tooling runtime (Slice 1).
 *
 * A self-contained QuickJS runtime + context that runs ONLY trusted, bundled Hull tooling
 * JavaScript (the JS source frontend) to PARSE / ANALYZE application source passed as DATA.
 * It is deliberately more restricted than the application JS runtime: no db/fs/http/env/
 * crypto/compute/gpu/network/spawn/module-loading-of-app-source, and NO dynamic code
 * execution at all. Application JavaScript is never executed here.
 *
 * DYNAMIC CODE IS FULLY BLOCKED, not merely hidden. The runtime's eval hook
 * (ctx->eval_internal, set only by JS_AddIntrinsicEval) is NEVER enabled on the session
 * context, so every dynamic-compile path throws "eval is not supported" at its QuickJS
 * source: global `eval`, the `Function` constructor, AND the prototype-reachable
 * constructors that survive deleting the global bindings -- e.g.
 * `({}).constructor.constructor("...")`, `(function(){}).constructor("...")`,
 * `(async function(){}).constructor("...")` -- because they all funnel through the same
 * hook. The trusted bundle is instead compiled to bytecode ONCE, in a throwaway
 * eval-enabled context at create time, and the session loads it via JS_ReadObject (a
 * deserializer that needs no eval hook). The global `eval` / `Function` bindings are also
 * deleted, as defense in depth. Adversarial coverage: test_js_session.c::dynamic_code_blocked.
 *
 * C OWNS this runtime (creation, invocation, limits, transport validation, teardown). The
 * Lua project layer never obtains or operates it (design: docs/javascript_source_frontend_
 * design.md, ratified). The session loads bundled `hull:*` tooling modules from the cli-js
 * registry ONLY. Slice 1 proves the runtime + byte transport + module loading + the full
 * limit contract + exception->diagnostic conversion + lifecycle with a trivial `hull:probe`
 * entry; Slice 2+ add the real lexer/parser.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_FRONTEND_JS_SESSION_H
#define HL_FRONTEND_JS_SESSION_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlJsSession HlJsSession;

/* Tooling-runtime limits. Every breach becomes a structured INDETERMINATE diagnostic
 * (never a raw exception or crash); see hl_js_session_analyze for the code taxonomy.
 * Defaults are the ratified D6 starting points; calibrate against measured corpus
 * high-water marks.
 *
 * `max_instructions` note: QuickJS invokes the interrupt handler once per
 * JS_INTERRUPT_COUNTER_INIT (= 10000) interpreter poll-points (loop back-edges and calls),
 * NOT once per bytecode instruction. So `max_instructions` counts interrupt-handler
 * invocations -- a COARSE budget of roughly 10000 poll-points each, not literal
 * instructions -- and the interrupt error QuickJS raises is uncatchable, so a runaway
 * tool cannot swallow it. */
typedef struct {
    size_t  max_heap_bytes;    /* QuickJS heap limit (JS_SetMemoryLimit) -> js.limit.heap */
    size_t  max_stack_bytes;   /* QuickJS stack limit (JS_SetMaxStackSize) -> js.limit.stack */
    int64_t max_instructions;  /* interrupt budget per invocation (0 = unlimited) -> js.limit.instructions */
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
 * defaults. Precompiles the bundled cli-js tooling modules to bytecode (a throwaway
 * eval-enabled context) so the session itself never enables the eval hook. Returns NULL on
 * allocation failure or if the bundled tooling fails to compile (a build defect). */
HlJsSession *hl_js_session_create(const HlJsSessionLimits *limits);

/* Invoke a bundled tooling entry: the `module` (a `hull:*` cli-js module that sets
 * `globalThis.__hull_frontend`) is loaded once from bytecode, then its `method` (e.g.
 * "analyze") is called with (ArrayBuffer(src[0..src_len)), path, options). Source crosses
 * as raw bytes (length-aware, NUL-safe) -- never a NUL-terminated string. `options_json`
 * (with `options_len`) is untrusted-shaped transport: it is JSON.parse'd length-aware, and
 * malformed / NUL-bearing / trailing-garbage input FAILS CLOSED (js.transport) rather than
 * being silently truncated or nulled.
 *
 * On success returns 0 and sets *out_json to a malloc'd NUL-terminated JSON string (caller
 * frees) of the method's return value, *out_len to its byte length. On ANY failure returns
 * -1 and sets *out_json to a structured INDETERMINATE diagnostic JSON:
 *   {"status":"indeterminate","diagnostics":[{"severity":"error","code":CODE,"message":...}]}
 * where CODE is one of:
 *   js.transport            - bad input transport (null src + nonzero len, malformed options,
 *                             failed argument construction)
 *   js.limit.bytes          - source exceeds max_source_bytes
 *   js.limit.instructions   - interrupt budget exhausted
 *   js.limit.heap           - QuickJS heap limit hit
 *   js.limit.stack          - QuickJS stack limit hit
 *   js.limit.result         - result JSON exceeds max_result_bytes
 *   js.internal             - an ordinary tooling exception or other internal failure
 * Never raises a QuickJS exception across this boundary; never crashes. If `out_json` is
 * NULL the diagnostic is discarded (no leak); the return value still reflects success. */
int hl_js_session_analyze(HlJsSession *s, const char *module, const char *method,
                          const uint8_t *src, size_t src_len,
                          const char *path,
                          const char *options_json, size_t options_len,
                          char **out_json, size_t *out_len);

/* Destroy the session (JS_FreeContext + JS_FreeRuntime + bytecode). Safe on NULL. */
void hl_js_session_destroy(HlJsSession *s);

#endif /* HL_FRONTEND_JS_SESSION_H */
