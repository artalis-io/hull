/*
 * cap/test.h — In-process test module for hull test
 *
 * Provides:
 *   - test("desc", fn)       — register a test case
 *   - test.get/post/...      — HTTP dispatch without TCP
 *   - test.eq/ok/err         — assertions
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TEST_H
#define HL_CAP_TEST_H

#include <stdio.h>

/* Forward declarations */
typedef struct lua_State lua_State;
typedef struct JSContext JSContext;
typedef struct KlRouter KlRouter;
typedef struct HlLua HlLua;
typedef struct HlJS HlJS;

/* ── Test result collection ─────────────────────────────────────────── */

/* Per-test result for structured (JSON) output */
typedef struct {
    char name[256];
    int  passed;       /* 1 = pass, 0 = fail */
    char error[1024];  /* error message if failed, empty if passed */
} HlTestCaseResult;

/* ── Lua bindings ──────────────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

/*
 * Register the `test` global in the Lua state.
 * test is a callable table (via __call metamethod) for registering tests,
 * and also has methods: get, post, put, delete, patch, eq, ok, err.
 */
void hl_cap_test_register_lua(lua_State *L, KlRouter *router, HlLua *lua);

/*
 * Clear registered test cases (between files).
 */
void hl_cap_test_clear_lua(lua_State *L);

/*
 * Run all registered test cases and report results.
 * If out is NULL, output is suppressed (for JSON mode).
 * If results is non-NULL, per-test results are collected (max max_results).
 */
void hl_cap_test_run_lua(lua_State *L, int *total, int *passed, int *failed,
                         FILE *out, HlTestCaseResult *results, int max_results);

#endif /* HL_ENABLE_LUA */

/* ── JS bindings ───────────────────────────────────────────────────── */

#ifdef HL_ENABLE_JS

void hl_cap_test_register_js(JSContext *ctx, KlRouter *router, HlJS *js);
void hl_cap_test_free_js(JSContext *ctx);
void hl_cap_test_clear_js(JSContext *ctx);
void hl_cap_test_run_js(JSContext *ctx, int *total, int *passed, int *failed,
                        FILE *out, HlTestCaseResult *results, int max_results);

#endif /* HL_ENABLE_JS */

#endif /* HL_CAP_TEST_H */
