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

/* Forward declarations */
typedef struct lua_State lua_State;
typedef struct JSContext JSContext;
typedef struct KlRouter KlRouter;
typedef struct HlLua HlLua;
typedef struct HlJS HlJS;

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
 */
void hl_cap_test_run_lua(lua_State *L, int *total, int *passed, int *failed);

#endif /* HL_ENABLE_LUA */

/* ── JS bindings ───────────────────────────────────────────────────── */

#ifdef HL_ENABLE_JS

void hl_cap_test_register_js(JSContext *ctx, KlRouter *router, HlJS *js);
void hl_cap_test_clear_js(JSContext *ctx);
void hl_cap_test_run_js(JSContext *ctx, int *total, int *passed, int *failed);

#endif /* HL_ENABLE_JS */

#endif /* HL_CAP_TEST_H */
