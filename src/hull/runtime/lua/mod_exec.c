/* mod_exec.c - hull.exec module: run a command the manifest declares
 * (docs/design/exec_broker.md)
 *
 *   local exec = require("hull.exec")
 *   local r = exec.run("unit-state", { "hs-api.service" })
 *   -- r = { status, signal, stdout, stderr, timed_out, stdout_truncated,
 *   --       stderr_truncated, duration_ms }
 *
 * The name must be declared in the manifest's `exec` section, and the
 * arguments must fill its slots (cap/exec.c, hl_exec_check_call - the one
 * matcher). A mismatch raises, naming the argument and the rule.
 *
 * This version has no command broker yet: outside `hull test` every call
 * raises after the match. Under `hull test`, test.exec_stub(name, result)
 * registers the answer; the call is still matched in full first, so a test
 * catches a bad argument, and the output is cut to the command's
 * stdout_max / stderr_max as the broker will cut it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "mod_exec.h"
#include "refusal.h"
#include "hull/cap/exec.h"
#ifdef HL_ENABLE_DB
#include "hull/cap/db_registry.h"   /* hl_db_registry_open_txn */
#endif

#include <string.h>

#define EXEC_STUBS_KEY "__hull_exec_stubs"

static const HlExecCmd *exec_find(HlLua *lua, const char *name, size_t n,
                                  HlExecCmd *out)
{
    if (!lua || !lua->base.exec_policy) return NULL;
    if (hl_exec_policy_find(lua->base.exec_policy, lua->base.exec_policy_len,
                            name, n, out) != 0)
        return NULL;
    return out;
}

/* r.<key> as an optional string, cut to @p max; sets *cut when it was. */
static void set_output(lua_State *L, int stub, int res, const char *key,
                       uint32_t max, const char *cut_key)
{
    size_t n = 0;
    const char *s = "";
    lua_getfield(L, stub, key);
    if (lua_type(L, -1) == LUA_TSTRING) s = lua_tolstring(L, -1, &n);
    else if (!lua_isnil(L, -1))
        luaL_error(L, "exec stub: %s must be a string", key);
    int cut = n > max;
    lua_pushlstring(L, s, cut ? max : n);
    lua_setfield(L, res, key);
    lua_pop(L, 1);
    lua_pushboolean(L, cut);
    lua_setfield(L, res, cut_key);
}

/* The result table the broker will return, built from a stub's table at
 * @p stub: every field present, the outputs cut to the command's caps. */
static void push_result(lua_State *L, int stub, const HlExecCmd *cmd)
{
    lua_createtable(L, 0, 8);
    int res = lua_gettop(L);

    lua_getfield(L, stub, "status");
    lua_Integer status = 0;
    if (lua_isinteger(L, -1)) status = lua_tointeger(L, -1);
    else if (!lua_isnil(L, -1)) luaL_error(L, "exec stub: status must be an integer");
    lua_pop(L, 1);
    lua_pushinteger(L, status);
    lua_setfield(L, res, "status");

    lua_getfield(L, stub, "signal");
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING)
        luaL_error(L, "exec stub: signal must be a string (\"SIGKILL\") or nil");
    lua_setfield(L, res, "signal");

    set_output(L, stub, res, "stdout", cmd->stdout_max, "stdout_truncated");
    set_output(L, stub, res, "stderr", cmd->stderr_max, "stderr_truncated");

    lua_getfield(L, stub, "timed_out");
    lua_pushboolean(L, lua_toboolean(L, -1));
    lua_setfield(L, res, "timed_out");
    lua_pop(L, 1);

    lua_getfield(L, stub, "duration_ms");
    lua_Integer ms = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    lua_pushinteger(L, ms < 0 ? 0 : ms);
    lua_setfield(L, res, "duration_ms");
}

/* exec.run(name, args?) -> result */
static int lua_exec_run(lua_State *L)
{
    size_t name_len = 0;
    const char *name = luaL_checklstring(L, 1, &name_len);
    int has_args = !lua_isnoneornil(L, 2);
    if (has_args) luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);

    /* The values, read raw (no __index / __len: no app code). Each pointer
     * is into a string the table at 2 holds, and is used only by the match
     * below, before anything allocates - an allocation can run a __gc
     * finalizer, app code that could change the table. */
    const char *args[HL_EXEC_MAX_CALL_ARGS];
    size_t lens[HL_EXEC_MAX_CALL_ARGS];
    size_t n = 0, total = 0;
    if (has_args) {
        lua_Unsigned len = lua_rawlen(L, 2);
        if (len > HL_EXEC_MAX_CALL_ARGS)
            return luaL_error(L, "exec.run: more than %d arguments",
                              HL_EXEC_MAX_CALL_ARGS);
        for (lua_Unsigned i = 1; i <= len; i++) {
            lua_rawgeti(L, 2, (lua_Integer)i);
            if (lua_type(L, -1) != LUA_TSTRING)
                return luaL_error(L, "exec.run: argument %d must be a string",
                                  (int)i);
            args[n] = lua_tolstring(L, -1, &lens[n]);
            total += lens[n];
            n++;
            lua_pop(L, 1);
        }
    }

    /* The match walks the policy and the arguments: charged first. */
    lua_hlwork(L, 64 + total / 8, 0);

    HlLua *lua = get_hl_lua(L);
    HlExecCmd cmd;
    char err[256];
    int rc = lua && lua->base.exec_policy
           ? hl_exec_check_call(lua->base.exec_policy, lua->base.exec_policy_len,
                                name, name_len, args, lens, n, &cmd, err, sizeof err)
           : HL_EXEC_E_UNKNOWN;
    if (rc == HL_EXEC_E_UNKNOWN)
        return luaL_error(L, "exec.run: '%s' is not declared in the manifest's "
                          "exec section", name);
    if (rc != HL_EXEC_OK)
        return luaL_error(L, "exec.run('%s'): %s", name, err);

    /* A run will wait for the command (PR 2); no wait is allowed inside a
     * transaction, so the call refuses there now as it will then. */
#ifdef HL_ENABLE_DB
    const char *txn = lua->base.db_registry
                    ? hl_db_registry_open_txn(lua->base.db_registry) : NULL;
    if (txn)
        return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "exec.run cannot wait while a "
                             "transaction is open on database connection '%s': "
                             "commit or roll back first", txn);
#endif

    lua_getfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
    if (!lua_istable(L, -1))
        return luaL_error(L, "exec.run('%s'): this version of Hull has no "
                          "command broker yet, so a declared command can only "
                          "be answered by a hull test stub (test.exec_stub)", name);
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    int stub_type = lua_type(L, -1);
    if (stub_type == LUA_TNIL)
        return luaL_error(L, "exec.run: no stub for '%s' in hull test (register "
                          "one with test.exec_stub)", name);
    if (stub_type == LUA_TFUNCTION) {
        /* stub(args, name): a fresh copy of the arguments, so the stub
         * cannot change the caller's table. */
        lua_createtable(L, (int)n, 0);
        for (size_t i = 1; i <= n; i++) {
            lua_rawgeti(L, 2, (lua_Integer)i);
            lua_rawseti(L, -2, (lua_Integer)i);
        }
        lua_pushvalue(L, 1);
        lua_call(L, 2, 1);
        if (!lua_istable(L, -1))
            return luaL_error(L, "exec stub for '%s' must return a result table",
                              name);
    }
    push_result(L, lua_gettop(L), &cmd);
    return 1;
}

static const luaL_Reg exec_funcs[] = {
    {"run", lua_exec_run},
    {NULL, NULL}
};

int luaopen_hull_exec(lua_State *L)
{
    hl_lua_newlib(L, exec_funcs);
    return 1;
}

/* test.exec_stub(name, result | fn | nil) */
int hl_lua_exec_stub(lua_State *L)
{
    size_t n = 0;
    const char *name = luaL_checklstring(L, 1, &n);
    int t = lua_type(L, 2);
    if (t != LUA_TTABLE && t != LUA_TFUNCTION && t != LUA_TNIL && t != LUA_TNONE)
        return luaL_error(L, "test.exec_stub(name, result): result is a table, "
                          "a function, or nil to remove the stub");
    HlExecCmd cmd;
    if (!exec_find(get_hl_lua(L), name, n, &cmd))
        return luaL_error(L, "test.exec_stub: '%s' is not declared in the "
                          "manifest's exec section", name);
    lua_settop(L, 2);
    lua_getfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
    if (!lua_istable(L, -1))
        return luaL_error(L, "test.exec_stub is only available in hull test");
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    return 0;
}

void hl_lua_exec_stubs_reset(lua_State *L)
{
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
}
