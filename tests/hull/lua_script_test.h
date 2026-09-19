/**
 * @file lua_script_test.h
 * @brief TEST-ONLY: run a co-located Lua test script in a vanilla lua_State.
 *
 * Some Hull layers are pure Lua with no C dependencies -- the `hull.source.*`
 * analysis tree, and most of the user-facing stdlib (csv, validate, form,
 * i18n, the htmx attribute builders). Their tests are best written in Lua,
 * where the data lives, and run against a VANILLA Lua 5.4 state: no Hull
 * sandbox, no module resolver, no capability layer. This helper is that
 * runner, surfacing the script's pass/fail counts to the C test binary so CI
 * gates on them.
 *
 * Contract. The script is loaded from a REPO-ROOT-RELATIVE path (tests run
 * from the repo root -- see mk/tests.mk) and must END WITH:
 *
 *     return { pass = pass, fail = fail }
 *
 * A script that instead prints a summary and calls `os.exit` does not merely
 * fail to report -- `os` IS present in this vanilla state (unlike Hull's
 * sandbox), so `os.exit(1)` terminates the whole UTEST binary and takes every
 * remaining suite with it. Convert such a tail to the return form before
 * wiring a script here.
 *
 * What this state does NOT provide is the capability layer: there is no `db`,
 * `crypto`, `fs`, `http`, or `time`. A script needing any of those belongs in
 * a caps-bearing UTEST in tests/hull/runtime/lua/test_lua.c instead, alongside
 * the existing `lua_stdlib` legs.
 *
 * Returns 0 when the script ran and returned the table (values via out-params);
 * -1 on any harness-level failure (state alloc, package.path, a raise inside
 * the script, or a non-table return). Uses NO utest macros -- those are only
 * valid inside a UTEST body -- so callers assert on the return code.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_TEST_LUA_SCRIPT_TEST_H
#define HULL_TEST_LUA_SCRIPT_TEST_H

#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static inline int run_lua_test(const char *script_path, long long *pass_out,
                               long long *fail_out)
{
    *pass_out = 0;
    *fail_out = -1;

    lua_State *L = luaL_newstate();
    if (!L) return -1;
    luaL_openlibs(L);

    /* Resolve require("hull.X") from the source tree (repo-root relative).
     * stdlib/cli/lua covers the tooling layers (hull.source.*, hull.project.*);
     * stdlib/lua covers the user-facing stdlib and the modules those pull in
     * (e.g. hull.json), which the tool VM would otherwise resolve via the
     * embedded VFS -- the harness has none. */
    if (luaL_dostring(L,
            "package.path = 'stdlib/cli/lua/?.lua;stdlib/cli/lua/?/init.lua;"
            "stdlib/lua/?.lua;stdlib/lua/?/init.lua;' .. package.path")
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

#endif /* HULL_TEST_LUA_SCRIPT_TEST_H */
