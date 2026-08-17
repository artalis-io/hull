/*
 * test_lua_source.c — harness for the pure-Lua hull.source.lua layer.
 *
 * The source-analysis layer is pure Lua with NO Hull C dependencies, so it is
 * tested in a VANILLA lua_State (vendored Lua 5.4, no Hull sandbox / module
 * resolver): we point package.path at stdlib/cli/lua and run the co-located Lua
 * test scripts, which self-assert and return { pass, fail }. This keeps the
 * source layer's own tests in Lua (where the data lives) while surfacing pass/
 * fail to the C test runner + CI. Tests run from the repo root (see mk/tests.mk).
 *
 * The Lua 5.4 load() differential conformance corpus (later slice) belongs HERE,
 * in the harness, not in the source-analysis module — the module never calls
 * dynamic compilation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Run a co-located Lua test script in a vanilla state. Returns 0 when the script
 * ran and returned a { pass, fail } table (values via out-params); -1 on any
 * harness-level failure (state alloc, package.path, dofile raise, bad return).
 * Uses NO utest macros -- those are only valid inside a UTEST body. */
static int run_lua_test(const char *script_path, long long *pass_out, long long *fail_out)
{
    *pass_out = 0;
    *fail_out = -1;

    lua_State *L = luaL_newstate();
    if (!L) return -1;
    luaL_openlibs(L);

    /* Resolve require("hull.source.X") from the source tree (repo-root relative). */
    if (luaL_dostring(L,
            "package.path = 'stdlib/cli/lua/?.lua;stdlib/cli/lua/?/init.lua;' .. package.path")
        != LUA_OK) {
        fprintf(stderr, "package.path setup failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return -1;
    }

    if (luaL_dofile(L, script_path) != LUA_OK) {
        fprintf(stderr, "\n%s: %s\n", script_path, lua_tostring(L, -1));
        lua_close(L);
        return -1;   /* the Lua script raised (a bug in the layer or the test) */
    }

    if (!lua_istable(L, -1)) { lua_close(L); return -1; }
    lua_getfield(L, -1, "fail");
    *fail_out = (long long)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "pass");
    *pass_out = (long long)lua_tointeger(L, -1);
    lua_pop(L, 1);

    fprintf(stderr, "  %s: %lld passed, %lld failed\n", script_path, *pass_out, *fail_out);
    lua_close(L);
    return 0;
}

UTEST(lua_source, lexer_slice1)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_lexer.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);              /* harness ran the script to a { pass, fail } return */
    EXPECT_EQ(fail, 0LL);          /* every Lua assertion passed */
    EXPECT_GT(pass, 0LL);          /* and the script actually ran cases */
}

UTEST_MAIN()
