/*
 * runtime/test.h — Runtime-layer test bindings (Lua + QuickJS)
 *
 * Declares the per-runtime `test` module — registration, clear, run.
 * Sources live in src/hull/runtime/{lua,js}/mod_test.c so the cap
 * layer stays free of runtime knowledge.
 *
 * Pure-C HTTP dispatch helper used by both runtimes is in
 * include/hull/cap/test.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_TEST_H
#define HL_RUNTIME_TEST_H

#include <stdio.h>
#include "hull/cap/test.h"   /* HlTestCaseResult */

/* Forward declarations */
typedef struct lua_State lua_State;
typedef struct JSContext JSContext;
typedef struct KlRouter  KlRouter;
typedef struct HlLua     HlLua;
typedef struct HlJS      HlJS;

/* ── Lua bindings ──────────────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

/*
 * Register the `test` global in the Lua state.
 * test is a callable table (via __call metamethod) for registering tests,
 * and also has methods: get, post, put, delete, patch, eq, ok, err.
 */
void hl_lua_test_register(lua_State *L, KlRouter *router, HlLua *lua);

/*
 * Clear registered test cases (between files).
 */
void hl_lua_test_clear(lua_State *L);

/*
 * Run all registered test cases and report results.
 * If out is NULL, output is suppressed (for JSON mode).
 * If results is non-NULL, per-test results are collected (max max_results).
 */
void hl_lua_test_run(lua_State *L, int *total, int *passed, int *failed,
                     FILE *out, HlTestCaseResult *results, int max_results);

#endif /* HL_ENABLE_LUA */

/* ── JS bindings ───────────────────────────────────────────────────── */

#ifdef HL_ENABLE_JS

void hl_js_test_register(JSContext *ctx, KlRouter *router, HlJS *js);
void hl_js_test_free(JSContext *ctx);
void hl_js_test_clear(JSContext *ctx);
void hl_js_test_run(JSContext *ctx, int *total, int *passed, int *failed,
                    FILE *out, HlTestCaseResult *results, int max_results);

#endif /* HL_ENABLE_JS */

#endif /* HL_RUNTIME_TEST_H */
