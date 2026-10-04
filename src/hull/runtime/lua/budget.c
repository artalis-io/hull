/*
 * budget.c - the Lua instruction budget
 *
 * The count hook used to raise an ordinary error every N instructions of
 * each thread. Ordinary errors are caught: `while true do pcall(function()
 * while true do end end) end` swallowed the limit forever, and every
 * coroutine got a fresh counter of its own, so splitting the work across
 * coroutines reset it too.
 *
 * Now one HlLuaBudget per VM is charged by every thread's hook (a stride
 * at a time), and the trip is STICKY: once over, each hooked thread raises
 * on its next instruction, and pcall / xpcall / coroutine.resume /
 * coroutine.close re-raise the trip instead of returning it, so it reaches
 * the runtime's own protected boundary. An entry point (dispatch, resume,
 * timer, ws / sse callback, worker dispatch) re-arms the budget with
 * hl_lua_budget_arm, so the limit is per uninterrupted run.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "internal.h"

#include "lua.h"
#include "lauxlib.h"

/* Instructions charged per hook call: small enough that a tripped thread
 * notices within a few thousand instructions of the limit, large enough
 * that the hook costs nothing measurable. */
#define HL_LUA_BUDGET_STRIDE 10000

static const char hl_lua_budget_key = 0;

static HlLuaBudget *budget_of(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &hl_lua_budget_key);
    HlLuaBudget *b = (HlLuaBudget *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return b;
}

int hl_lua_budget_tripped(lua_State *L)
{
    HlLuaBudget *b = budget_of(L);
    return b && b->tripped;
}

static void budget_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    HlLuaBudget *b = budget_of(L);
    if (b && !b->tripped) {
        int count = lua_gethookcount(L);
        b->used += count;
        if (b->limit <= 0 || b->used < b->limit) {
            /* A thread that tripped in an earlier run still has the count-1
             * hook below; the next arm re-arms only its own entry thread.
             * Back to the stride, or such a coroutine (kept in a global)
             * ran this hook on every instruction for good. */
            int stride = b->limit > 0 && b->limit < HL_LUA_BUDGET_STRIDE
                         ? (int)b->limit : HL_LUA_BUDGET_STRIDE;
            if (count == 1 && stride > 1)
                lua_sethook(L, budget_hook, LUA_MASKCOUNT, stride);
            return;
        }
        b->tripped = 1;
    }
    /* Every further instruction of this thread raises again. */
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, 1);
    luaL_error(L, "instruction limit exceeded");
}

void hl_lua_instruction_hook(lua_State *L, lua_Debug *ar)
{
    budget_hook(L, ar);
}

void hl_lua_budget_arm(lua_State *thread, HlLuaBudget *b, int64_t limit)
{
    if (!thread || !b) return;
    b->limit = limit;
    b->used = 0;
    b->tripped = 0;
    if (limit <= 0) {
        lua_sethook(thread, NULL, 0, 0);
        return;
    }
    int stride = limit < HL_LUA_BUDGET_STRIDE ? (int)limit : HL_LUA_BUDGET_STRIDE;
    lua_sethook(thread, budget_hook, LUA_MASKCOUNT, stride);
}

/* ── pcall / xpcall that do not swallow the trip ──────────────────────
 * lbaselib's pcall and xpcall, with one change: a failure while the
 * budget is tripped is re-raised instead of returned. */

static int budget_finishpcall(lua_State *L, int status, lua_KContext extra)
{
    if (status != LUA_OK && status != LUA_YIELD) {
        if (hl_lua_budget_tripped(L))
            return lua_error(L);           /* the error object is on top */
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - (int)extra;
}

static int budget_pcall(lua_State *L)
{
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0,
                            budget_finishpcall);
    return budget_finishpcall(L, status, 0);
}

static int budget_xpcall(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);
    int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, budget_finishpcall);
    return budget_finishpcall(L, status, 2);
}

void hl_lua_budget_install(lua_State *L, HlLuaBudget *b)
{
    lua_pushlightuserdata(L, b);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &hl_lua_budget_key);
    lua_pushcfunction(L, budget_pcall);
    lua_setglobal(L, "pcall");
    lua_pushcfunction(L, budget_xpcall);
    lua_setglobal(L, "xpcall");
}
