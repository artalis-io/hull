/*
 * runtime/lua/mod_test.c - Lua test bindings
 *
 * Lua-specific test registration, HTTP dispatch wrappers, and assertions.
 * Uses the shared pure-C dispatch helper (cap/test.c::hl_cap_test_dispatch);
 * its source moved from cap/ to runtime/lua/ as part of architectural
 * roadmap item E (restore the cap-layer "no runtime knowledge" invariant).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_LUA

#include "hull/cap/test.h"
#include "hull/runtime/test.h"
#include "hull/runtime/lua.h"
#include "hull/utils/alloc.h"
#include "hull/cap/db_registry.h"   /* hl_db_registry_guard_stale_txns */
#include "internal.h"               /* HL_LUA_ARM, get_hl_lua_from_L */

#include <keel/http_request.h>

#include "lua.h"
#include "lauxlib.h"

#include <string.h>

/* Registry keys for stored state */
#define TEST_ROUTER_KEY   "__hull_test_router"
#define TEST_CASES_KEY    "__hull_test_cases"

static KlHttpRouter *get_test_router(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, TEST_ROUTER_KEY);
    KlHttpRouter *r = (KlHttpRouter *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return r;
}

static HlLua *get_test_lua(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_test_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua;
}

/* ── test(...) - register a test case ──────────────────────────────── */

static int l_test_call(lua_State *L)
{
    /* Called as test("desc", fn) - first arg is the test table (self) */
    const char *desc = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    /* Get test cases table */
    lua_getfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);
    int idx = (int)luaL_len(L, -1) + 1;

    /* Store { desc = desc, fn = fn } */
    lua_newtable(L);
    lua_pushstring(L, desc);
    lua_setfield(L, -2, "desc");
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "fn");

    lua_rawseti(L, -2, idx);
    lua_pop(L, 1); /* pop test cases table */
    return 0;
}

/* ── test.get/post/put/delete/patch ────────────────────────────────── */

typedef struct {
    HlTestResult *result;
    HlLua        *lua;
} HlTestBuild;

/* (arg: HlTestBuild) -> the result table. Under lua_pcall (l_test_http). */
static int test_build_result_k(lua_State *L)
{
    HlTestBuild *tb = (HlTestBuild *)lua_touserdata(L, 1);
    HlTestResult *r = tb->result;
    lua_settop(L, 0);
    lua_newtable(L);

    lua_pushinteger(L, r->status);
    lua_setfield(L, -2, "status");

    if (r->body && r->body_len > 0) {
        lua_pushlstring(L, r->body, r->body_len);
        lua_setfield(L, -2, "body");

        /* Auto-decode JSON if body looks like JSON */
        if (r->body[0] == '{' || r->body[0] == '[') {
            /* The runtime's own json (registry stash), not require(): the
             * harness decodes for every test, declared hull/json or not, and
             * a require from here runs with the test file as its caller - app
             * code, which the module gate refuses an undeclared module. */
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_json_internal");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "decode");
                lua_pushlstring(L, r->body, r->body_len);
                if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                    lua_setfield(L, -3, "json");
                } else {
                    lua_pop(L, 1); /* pop error */
                }
                lua_pop(L, 1); /* pop json module */
            } else {
                lua_pop(L, 1);
            }
        }

    }

    /* Parse response headers into a table */
    if (r->hdr_buf && r->hdr_len > 0) {
        lua_newtable(L);
        const char *p = r->hdr_buf;
        const char *end = p + r->hdr_len;
        while (p < end) {
            const char *colon = memchr(p, ':', (size_t)(end - p));
            if (!colon) break;
            const char *eol = memchr(colon, '\r', (size_t)(end - colon));
            if (!eol) eol = memchr(colon, '\n', (size_t)(end - colon));
            if (!eol) eol = end;
            /* Skip ": " after colon */
            const char *val = colon + 1;
            while (val < eol && *val == ' ') val++;
            /* Push lowercase header name as key */
            size_t name_len = (size_t)(colon - p);
            /* Lowercased in a Lua buffer: a malloc'd copy leaked when the
             * push after it raised. */
            luaL_Buffer lb;
            char *lower = luaL_buffinitsize(L, &lb, name_len);
            for (size_t i = 0; i < name_len; i++)
                lower[i] = (char)(p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]);
            luaL_pushresultsize(&lb, name_len);
            lua_pushlstring(L, val, (size_t)(eol - val));
            lua_settable(L, -3);
            /* Advance past \r\n */
            p = eol;
            while (p < end && (*p == '\r' || *p == '\n')) p++;
        }
        lua_setfield(L, -2, "headers");
    }

    return 1;
}

/* After an in-process request: the case's budget again, charged with what
 * the request used (it ran on the case's time), and tripped when over. */
static void test_budget_rejoin(HlLua *lua, const HlLuaBudget *saved)
{
    int64_t nested = lua->budget.used;
    int nested_tripped = lua->budget.tripped;
    int nested_timed_out = lua->budget.timed_out;
    lua->budget = *saved;
    lua->budget.used = nested > INT64_MAX - saved->used
                       ? INT64_MAX : saved->used + nested;
    if (nested_tripped ||
        (lua->budget.limit > 0 && lua->budget.used >= lua->budget.limit))
        lua->budget.tripped = 1;
    if (nested_timed_out)
        lua->budget.timed_out = 1;
}

static void test_result_free(HlLua *lua, HlTestResult *result)
{
    if (result->body)
        hl_alloc_free_const(lua->base.alloc, result->body, result->body_len + 1);
    if (result->hdr_buf)
        hl_alloc_free_const(lua->base.alloc, result->hdr_buf, result->hdr_len + 1);
    result->body = NULL;
    result->hdr_buf = NULL;
}

static int l_test_http(lua_State *L, const char *method)
{
    const char *path = luaL_checkstring(L, 1);
    KlHttpRouter *router = get_test_router(L);
    HlLua *lua = get_test_lua(L);

    if (!router || !lua) {
        return luaL_error(L, "test module not initialized");
    }

    /* Parse optional opts table (arg 2) */
    const char *body_str = NULL;
    size_t body_len = 0;
    const char *header_names[KL_MAX_HEADERS];
    const char *header_values[KL_MAX_HEADERS];
    int num_headers = 0;
    const char *ctx_json = NULL;
    int run_middleware = 0;

    /* 3: anchors. The body and header strings are held here, not only by
     * the opts table: encoding opts.ctx runs app metamethods, which could
     * clear those fields and collect the strings before dispatch reads
     * them. */
    lua_settop(L, 2);
    lua_newtable(L);

    if (lua_istable(L, 2)) {
        /* opts.middleware */
        lua_getfield(L, 2, "middleware");
        if (lua_toboolean(L, -1))
            run_middleware = 1;
        lua_pop(L, 1);
        /* opts.body */
        /* Real strings only (not numbers): those stay anchored by the opts
         * table after the pop, while a number's converted string was owned by
         * the popped stack slot alone - and dispatch runs Lua, so it could be
         * collected before the body was read. */
        lua_getfield(L, 2, "body");
        if (lua_type(L, -1) == LUA_TSTRING) {
            body_str = lua_tolstring(L, -1, &body_len);
            lua_pushvalue(L, -1);
            lua_rawseti(L, 3, (lua_Integer)lua_rawlen(L, 3) + 1);
        }
        lua_pop(L, 1);

        /* opts.headers */
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0 && num_headers < KL_MAX_HEADERS) {
                /* Real strings only: lua_tostring on a number KEY converts it
                 * in place and breaks lua_next, and a converted value is not
                 * anchored by the table (see body above). */
                if (lua_type(L, -2) == LUA_TSTRING &&
                    lua_type(L, -1) == LUA_TSTRING) {
                    header_names[num_headers] = lua_tostring(L, -2);
                    header_values[num_headers] = lua_tostring(L, -1);
                    num_headers++;
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, 3, (lua_Integer)lua_rawlen(L, 3) + 1);
                    lua_pushvalue(L, -1);
                    lua_rawseti(L, 3, (lua_Integer)lua_rawlen(L, 3) + 1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        /* opts.ctx - JSON-encode to pass through as req.ctx.
         * The encoded string must remain on the Lua stack (anchored)
         * until after hl_cap_test_dispatch() returns. */
        lua_getfield(L, 2, "ctx");
        if (lua_istable(L, -1)) {
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_json_internal");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "encode");
                lua_pushvalue(L, -3); /* push ctx table */
                if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1)) {
                    ctx_json = lua_tostring(L, -1);
                    /* Stack: ... ctx_tbl, json_mod, json_str
                     * Remove json_mod and ctx_tbl, leave json_str anchored */
                    lua_remove(L, -2); /* remove json_mod */
                    lua_remove(L, -2); /* remove ctx_tbl */
                } else {
                    lua_pop(L, 1); /* pop error */
                    lua_pop(L, 1); /* pop json module */
                    lua_pop(L, 1); /* pop ctx table */
                }
            } else {
                lua_pop(L, 1); /* pop non-table json */
                lua_pop(L, 1); /* pop ctx table */
            }
        } else {
            lua_pop(L, 1); /* pop non-table opts.ctx */
        }
    }

    /* The request runs inside this test case (audit 10). Its dispatch is an
     * entry and re-arms the budget, which reset the case's count: a case
     * looping over test.get never hit the limit. The case's count is kept
     * and the request's added to it. And the dispatch's stale-transaction
     * guard rolled back a transaction the case has open (a test.get inside
     * db.batch lost the batch's writes): held off while the case has one. */
    HlLuaBudget saved_budget = lua->budget;
    /* Likewise the case's wall-clock deadline: the request's arm set one of
     * its own, and the case runs on to its own (cap/run_watchdog.h). */
    uint64_t saved_deadline = hl_run_watch_save(&lua->run_watch);
    int hold = hl_db_registry_open_txn(lua->base.db_registry) != NULL;
    if (hold) hl_db_registry_guard_hold(1);
    HlTestResult result;
    int drc = hl_cap_test_dispatch(router, method, path, body_str, body_len,
                      header_names, header_values, num_headers,
                      ctx_json, lua->base.alloc, run_middleware,
                      &result);
    if (hold) hl_db_registry_guard_hold(-1);
    test_budget_rejoin(lua, &saved_budget);
    hl_run_watch_restore(&lua->run_watch, saved_deadline);
    if (hl_run_watch_stopped(&lua->run_watch) && !lua->budget.tripped) {
        lua->budget.tripped = 1;
        lua->budget.timed_out = 1;
    }
    if (drc != 0)
        return luaL_error(L, "test dispatch failed");
    if (lua->budget.tripped) {
        test_result_free(lua, &result);
        return hl_lua_budget_raise(L);
    }

    /* The result table is built under lua_pcall, and the result's C buffers
     * are freed after it either way: built directly, a raise while pushing
     * (out of memory) skipped the frees. */
    HlTestBuild tb = { &result, lua };
    int built = lua_checkstack(L, 4);
    if (built) {
        lua_pushcfunction(L, test_build_result_k);
        lua_pushlightuserdata(L, &tb);
        built = lua_pcall(L, 1, 1, 0) == LUA_OK;
    }
    test_result_free(lua, &result);
    if (!built)
        return lua_error(L);
    return 1;
}

static int l_test_get(lua_State *L)    { return l_test_http(L, "GET"); }
static int l_test_post(lua_State *L)   { return l_test_http(L, "POST"); }
static int l_test_put(lua_State *L)    { return l_test_http(L, "PUT"); }
static int l_test_delete(lua_State *L) { return l_test_http(L, "DELETE"); }
static int l_test_patch(lua_State *L)  { return l_test_http(L, "PATCH"); }

/* ── test.eq(a, b, msg?) ──────────────────────────────────────────── */

static int l_test_eq(lua_State *L)
{
    int equal = 0;

    if (lua_type(L, 1) == lua_type(L, 2)) {
        if (lua_isstring(L, 1) && lua_isstring(L, 2)) {
            equal = (strcmp(lua_tostring(L, 1), lua_tostring(L, 2)) == 0);
        } else if (lua_isnumber(L, 1) && lua_isnumber(L, 2)) {
            equal = (lua_tonumber(L, 1) == lua_tonumber(L, 2));
        } else if (lua_isboolean(L, 1) && lua_isboolean(L, 2)) {
            equal = (lua_toboolean(L, 1) == lua_toboolean(L, 2));
        } else if (lua_isnil(L, 1) && lua_isnil(L, 2)) {
            equal = 1;
        } else {
            /* For tables/userdata, compare by reference */
            equal = lua_rawequal(L, 1, 2);
        }
    }

    if (!equal) {
        const char *msg = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;
        const char *a_str = luaL_tolstring(L, 1, NULL);
        const char *b_str = luaL_tolstring(L, 2, NULL);
        if (msg)
            return luaL_error(L, "test.eq failed: %s\n  expected: %s\n  actual:   %s",
                              msg, b_str, a_str);
        else
            return luaL_error(L, "test.eq failed\n  expected: %s\n  actual:   %s",
                              b_str, a_str);
    }

    return 0;
}

/* ── test.ok(val, msg?) ───────────────────────────────────────────── */

static int l_test_ok(lua_State *L)
{
    if (!lua_toboolean(L, 1)) {
        const char *msg = lua_isstring(L, 2) ? lua_tostring(L, 2) : "test.ok failed";
        return luaL_error(L, "%s", msg);
    }
    return 0;
}

/* ── test.err(fn, pattern?) ───────────────────────────────────────── */

static int l_test_err(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const char *pattern = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;

    lua_pushvalue(L, 1);
    int rc = lua_pcall(L, 0, 0, 0);

    if (rc == LUA_OK)
        return luaL_error(L, "test.err: expected error but function succeeded");

    if (pattern) {
        const char *err = lua_tostring(L, -1);
        if (!err || strstr(err, pattern) == NULL) {
            return luaL_error(L, "test.err: error '%s' does not match pattern '%s'",
                              err ? err : "(nil)", pattern);
        }
    }

    lua_pop(L, 1); /* pop error message */
    return 0;
}

/* ── Registration ──────────────────────────────────────────────────── */

static const luaL_Reg test_methods[] = {
    { "get",    l_test_get },
    { "post",   l_test_post },
    { "put",    l_test_put },
    { "delete", l_test_delete },
    { "patch",  l_test_patch },
    { "eq",     l_test_eq },
    { "ok",     l_test_ok },
    { "err",    l_test_err },
    { NULL, NULL }
};

void hl_lua_test_register(lua_State *L, KlHttpRouter *router, HlLua *lua)
{
    /* Store router and lua context in registry */
    lua_pushlightuserdata(L, router);
    lua_setfield(L, LUA_REGISTRYINDEX, TEST_ROUTER_KEY);

    lua_pushlightuserdata(L, lua);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_test_lua");

    /* Create test cases table */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);

    /* Create the test table with methods */
    luaL_newlib(L, test_methods);

    /* Create metatable with __call for test("desc", fn) */
    lua_newtable(L); /* metatable */
    lua_pushcfunction(L, l_test_call);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);

    lua_setglobal(L, "test");
}

void hl_lua_test_clear(lua_State *L)
{
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);
}

void hl_lua_test_run(lua_State *L, int *total, int *passed, int *failed,
                         FILE *out, HlTestCaseResult *results, int max_results)
{
    /* All three count pointers are required (callers always pass &locals). */
    if (!total || !passed || !failed)
        return;
    *total = 0;
    *passed = 0;
    *failed = 0;

    lua_getfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    HlLua *lua = get_hl_lua_from_L(L);
    int count = (int)luaL_len(L, -1);
    for (int i = 1; i <= count; i++) {
        /* Each case is a run of its own, as each request is: the budget
         * re-armed (one case that tripped the sticky limit failed every
         * later case) and a transaction an earlier case left open rolled
         * back, so the next case is not inside it (the JS harness does the
         * same per case). */
        if (lua) {
            hl_db_registry_guard_stale_txns(lua->base.db_registry);
            HL_LUA_ARM(lua, L);
        }
        lua_rawgeti(L, -1, i);

        lua_getfield(L, -1, "desc");
        const char *desc = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "fn");

        int idx = *total;
        (*total)++;
        int rc = lua_pcall(L, 0, 0, 0);
        if (rc == LUA_OK) {
            if (out)
                fprintf(out, "  PASS  %s\n", desc ? desc : "(unnamed)");
            if (results && idx < max_results) {
                snprintf(results[idx].name, sizeof(results[idx].name),
                         "%s", desc ? desc : "(unnamed)");
                results[idx].passed = 1;
                results[idx].error[0] = '\0';
            }
            (*passed)++;
        } else {
            const char *err = lua_tostring(L, -1);
            if (out)
                fprintf(out, "  FAIL  %s\n    %s\n", desc ? desc : "(unnamed)",
                       err ? err : "unknown error");
            if (results && idx < max_results) {
                snprintf(results[idx].name, sizeof(results[idx].name),
                         "%s", desc ? desc : "(unnamed)");
                results[idx].passed = 0;
                snprintf(results[idx].error, sizeof(results[idx].error),
                         "%s", err ? err : "unknown error");
            }
            lua_pop(L, 1); /* pop error */
            (*failed)++;
        }

        lua_pop(L, 1); /* pop test case table */
    }
    if (lua)
        hl_db_registry_guard_stale_txns(lua->base.db_registry);

    lua_pop(L, 1); /* pop test cases table */
}

#endif /* HL_ENABLE_LUA */
