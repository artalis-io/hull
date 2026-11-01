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
 * The trip is raised from inside the count hook, where Lua has every hook
 * turned off (L->allowhook = 0). Whatever Lua code runs before the error
 * leaves the hook runs unmetered, so the raise must run none:
 *   - it allocates nothing. luaL_error built "chunk:line: message", and the
 *     allocation could take a GC step that ran pending __gc finalizers -
 *     still with hooks off (HULL PATCH 0001 now turns the count hook back on
 *     in finalizers regardless; docs/lua_patches.md). The message is a
 *     string made once at install and kept in the registry.
 *   - xpcall's message handler is skipped on a trip: Lua calls it before
 *     the error leaves the hook, so an app handler ran with no limit at all
 *     (`xpcall(spin, function() while true do end end)`). The trip is
 *     re-raised whatever the handler returns, so nothing is lost.
 *
 * The run's wall-clock deadline (cap/run_watchdog.h) trips the same way: the
 * watchdog thread raises the watch's stop flag, Lua HULL PATCH 0005 makes
 * the next instruction of any hooked thread call this hook, and the hook
 * trips the budget with its own message. It bounds what the count cannot
 * see - work inside one instruction that no patch charges.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"

#include "internal.h"

#include "lua.h"
#include "lauxlib.h"

#include "hull/shared/dispatch_meter.h"

#include <string.h>

/* Work done inside one binding call, where the count hook never fires:
 * charged straight to the budget, never raising. 1 once tripped. */
static int budget_charge(HlLuaBudget *b, int64_t units)
{
    if (b->tripped) return 1;
    if (hl_run_watch_stopped(b->watch)) {   /* the run's deadline passed */
        b->tripped = 1;
        b->timed_out = 1;
        return 1;
    }
    b->charged += (uint64_t)units;      /* the dispatcher's meter */
    if (b->limit <= 0) return 0;
    b->used = units > INT64_MAX - b->used ? INT64_MAX : b->used + units;
    if (b->used >= b->limit) b->tripped = 1;
    return b->tripped;
}

#ifdef HL_ENABLE_DB
#include "hull/cap/db_budget.h"

/* SQL work, charged by the SQLite progress handler (cap/db_budget.h): a
 * statement runs inside one binding call, where the count hook never fires.
 * Once over, the statement is interrupted and the binding raises the trip
 * (hl_lua_raise_copy). */
static int budget_db_charge(void *ud, int64_t units)
{
    return budget_charge((HlLuaBudget *)ud, units);
}
#endif

/* Instructions charged per hook call: small enough that a tripped thread
 * notices within a few thousand instructions of the limit, large enough
 * that the hook costs nothing measurable. */
#define HL_LUA_BUDGET_STRIDE 10000

static const char hl_lua_budget_key = 0;
static const char hl_lua_budget_err_key = 0;   /* the trip's message */
static const char hl_lua_budget_time_key = 0;  /* the deadline's message */

static HlLuaBudget *budget_of(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &hl_lua_budget_key);
    HlLuaBudget *b = (HlLuaBudget *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return b;
}

int hl_lua_budget_charge(lua_State *L, int64_t units)
{
    HlLuaBudget *b = budget_of(L);
    return b ? budget_charge(b, units > 0 ? units : 0) : 0;
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
    if (b && !b->tripped && hl_run_watch_stopped(b->watch)) {
        b->tripped = 1;                 /* the run's wall-clock deadline */
        b->timed_out = 1;
    }
    if (b && !b->tripped) {
        int count = lua_gethookcount(L);
        /* Plus the work charged past the hook's count (HULL PATCH 0004:
         * a long-string compare, a big allocation, a table shift - work
         * one instruction did; docs/lua_patches.md). */
        size_t owed = lua_hltakeowed(L);
        b->charged += (uint64_t)count + owed;   /* the dispatcher's meter */
        int64_t room = INT64_MAX - b->used - count;
        b->used += count + (owed > (uint64_t)room ? room : (int64_t)owed);
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
    hl_lua_budget_raise(L);
}

int hl_lua_budget_raise(lua_State *L)
{
    HlLuaBudget *b = budget_of(L);
    int timed = b && b->timed_out;
    /* A registry read: no allocation (luaD_hook left LUA_MINSTACK free). */
    if (lua_rawgetp(L, LUA_REGISTRYINDEX,
                    timed ? &hl_lua_budget_time_key
                          : &hl_lua_budget_err_key) == LUA_TSTRING)
        return lua_error(L);
    lua_pop(L, 1);
    return luaL_error(L, timed ? HL_RUN_TIME_LIMIT_MSG
                               : "instruction limit exceeded");   /* not installed */
}

void hl_lua_instruction_hook(lua_State *L, lua_Debug *ar)
{
    budget_hook(L, ar);
}

void hl_lua_budget_arm(lua_State *thread, HlLuaBudget *b, int64_t limit,
                       uint32_t run_ms)
{
    if (!thread || !b) return;
    b->limit = limit;
    b->used = 0;
    b->tripped = 0;
    b->timed_out = 0;
    /* The binding meter's CPU reference (shared/dispatch_meter.h): only
     * when no binding call is in progress (a nested run arms inside one). */
    if (b->nframes == 0 && !b->meter_off)
        hl_meter_arm(&b->meter);
    /* The run's wall-clock deadline: armed (and its flag cleared) before any
     * of the run's code. */
    if (b->watch) hl_run_watch_arm(b->watch, run_ms);
    else run_ms = 0;
#ifdef HL_ENABLE_DB
    /* This thread's SQL is this run's from here on (audit 9 H4). */
    (void)hl_db_budget_swap(budget_db_charge, b);
#endif
    if (limit <= 0 && run_ms == 0) {
        lua_sethook(thread, NULL, 0, 0);
        return;
    }
    /* With no count limit the hook still runs, for the deadline (Lua HULL
     * PATCH 0005 calls it only on a hooked thread). */
    int stride = limit > 0 && limit < HL_LUA_BUDGET_STRIDE
                 ? (int)limit : HL_LUA_BUDGET_STRIDE;
    lua_sethook(thread, budget_hook, LUA_MASKCOUNT, stride);
}

const char *hl_lua_trip_reason(const HlLuaBudget *b)
{
    return b && b->timed_out ? HL_RUN_TIME_LIMIT_MSG
                             : "instruction limit exceeded";
}

/* ── pcall / xpcall that do not swallow the trip ──────────────────────
 * lbaselib's pcall and xpcall, with one change: a failure while the
 * budget is tripped is re-raised instead of returned. */

static int budget_finishpcall(lua_State *L, int status, lua_KContext extra)
{
    if (status != LUA_OK && status != LUA_YIELD) {
        /* Raise the trip itself, not the error object on top: the call
         * that failed may have raised its own error and then been tripped
         * by the dispatcher's charge as it unwound (meter_throw). */
        if (hl_lua_budget_tripped(L))
            return hl_lua_budget_raise(L);
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

/* xpcall's message handler, upvalue 1: not run on a trip (see the top). */
static int budget_msgh(lua_State *L)
{
    if (hl_lua_budget_tripped(L))
        return 1;                          /* the error object, as it is */
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

static int budget_xpcall(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, budget_msgh, 1);
    lua_replace(L, 2);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);
    int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, budget_finishpcall);
    return budget_finishpcall(L, status, 2);
}

/* ── The binding dispatcher (shared/dispatch_meter.h) ─────────────────
 *
 * Every binding registered with hl_lua_setfuncs / hl_lua_pushcclosure is a
 * C closure over its real function, the HlLuaBudget and the binding's own
 * upvalues (first, so lua_upvalueindex(i) still names them). The trampoline
 * calls the real function DIRECTLY - the same CallInfo, so lua_getstack,
 * the caller's name and the stdlib checks see what they saw before, and a
 * lua_yieldk inside it suspends this very frame (its continuation is
 * called by Lua on resume, not through the trampoline).
 *
 * Around the call it keeps a frame on b->frames: clock, excluded time and
 * the run's charge meter (b->charged plus the work the thread has not yet
 * reported to its count hook). On return it charges what the call's wall
 * time is worth past what was charged meanwhile. A call that does not
 * return - an error, or a yield - is unwound by a longjmp the trampoline
 * never sees, so Lua HULL PATCH 0007 calls meter_throw before every throw:
 * a yield ends every call in progress on the yielding thread (they are the
 * whole C stack of that coroutine, and the parked time that follows is not
 * the run's), and an error ends the calls on the throwing thread that
 * started inside the error handler it is thrown to. Each is charged up to
 * that moment. */

typedef union { void *p; lua_CFunction f; } HlFnBits;

static uint64_t meter_reading(HlLuaBudget *b, lua_State *L)
{
    return b->charged + (uint64_t)lua_hlpending(L);
}

/* Charge the frame @p fr for its call up to now; never raises. */
static void meter_settle(HlLuaBudget *b, lua_State *L,
                         const HlLuaMeterFrame *fr)
{
    int64_t charged = (int64_t)(meter_reading(b, L) - fr->m0);
    uint64_t owed = hl_meter_settle(&fr->mark, charged, &b->meter);
    if (owed)
        (void)budget_charge(b, owed > (uint64_t)INT64_MAX ? INT64_MAX
                                                          : (int64_t)owed);
}

static void meter_throw(lua_State *L, int status, void *ud)
{
    HlLuaBudget *b = (HlLuaBudget *)ud;
    const void *ej = lua_hlerrorjmp(L);
    while (b->nframes > 0) {
        HlLuaMeterFrame *fr = &b->frames[b->nframes - 1];
        if (fr->L != L) break;
        if (status != LUA_YIELD && ej != NULL && fr->ej != ej) break;
        b->nframes--;
        meter_settle(b, L, fr);
    }
}

/* 1 when the run is already over (its budget, or its deadline). */
static int meter_over(HlLuaBudget *b)
{
    if (!b->tripped && hl_run_watch_stopped(b->watch)) {
        b->tripped = 1;
        b->timed_out = 1;
    }
    return b->tripped;
}

static int meter_call(lua_State *L, int nup)
{
    HlFnBits fn;
    fn.p = lua_touserdata(L, lua_upvalueindex(nup + 1));
    HlLuaBudget *b =
        (HlLuaBudget *)lua_touserdata(L, lua_upvalueindex(nup + 2));
    if (meter_over(b))
        return hl_lua_budget_raise(L);
    int idx = b->nframes;
    if (b->limit <= 0 || idx >= HL_LUA_METER_DEPTH)
        return fn.f(L);              /* nothing to charge, or too deep */
    HlLuaMeterFrame *fr = &b->frames[idx];
    fr->L  = L;
    fr->ej = lua_hlerrorjmp(L);
    fr->m0 = meter_reading(b, L);
    hl_meter_begin(&fr->mark, b->meter.mode != 0);
    b->nframes = idx + 1;
    int n = fn.f(L);
    if (b->nframes <= idx)           /* already settled (cannot happen) */
        return n;
    b->nframes = idx;                /* frames above were unwound */
    meter_settle(b, L, fr);
    /* Over now: the call's effects stand, and the run ends here. */
    if (b->tripped)
        return hl_lua_budget_raise(L);
    return n;
}

static int meter_tramp0(lua_State *L) { return meter_call(L, 0); }
static int meter_tramp1(lua_State *L) { return meter_call(L, 1); }
static int meter_tramp2(lua_State *L) { return meter_call(L, 2); }

static const lua_CFunction meter_tramps[] = {
    meter_tramp0, meter_tramp1, meter_tramp2,
};

/* Push @p f closed over the @p nup values on top of the stack (consumed). */
static void meter_push(lua_State *L, HlLuaBudget *b, lua_CFunction f, int nup)
{
    if (!b || b->meter_off || nup < 0 || nup > 2) {
        lua_pushcclosure(L, f, nup);
        return;
    }
    HlFnBits fn;
    fn.f = f;
    lua_pushlightuserdata(L, fn.p);
    lua_pushlightuserdata(L, b);
    lua_pushcclosure(L, meter_tramps[nup], nup + 2);
}

int hl_lua_is_dispatched(lua_State *L, int idx)
{
    lua_CFunction f = lua_tocfunction(L, idx);
    return f == meter_tramp0 || f == meter_tramp1 || f == meter_tramp2;
}

void hl_lua_pushcclosure(lua_State *L, lua_CFunction fn, int n)
{
    meter_push(L, budget_of(L), fn, n);
}

void hl_lua_setfuncs(lua_State *L, const luaL_Reg *l, int nup)
{
    HlLuaBudget *b = budget_of(L);
    luaL_checkstack(L, nup + 2, "too many upvalues");
    for (; l->name != NULL; l++) {
        if (l->func == NULL) {
            lua_pushboolean(L, 0);
        } else {
            for (int i = 0; i < nup; i++)
                lua_pushvalue(L, -nup);
            int raw = strcmp(l->name, "__gc") == 0 ||
                      strcmp(l->name, "__close") == 0;
            meter_push(L, raw ? NULL : b, l->func, nup);
        }
        lua_setfield(L, -(nup + 2), l->name);
    }
    lua_pop(L, nup);
}

void hl_lua_budget_install(lua_State *L, HlLuaBudget *b)
{
    lua_pushlightuserdata(L, b);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &hl_lua_budget_key);
    lua_pushliteral(L, "instruction limit exceeded");
    lua_rawsetp(L, LUA_REGISTRYINDEX, &hl_lua_budget_err_key);
    lua_pushliteral(L, HL_RUN_TIME_LIMIT_MSG);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &hl_lua_budget_time_key);
    /* Lua HULL PATCH 0005: the watchdog's flag reaches every thread's hook. */
    lua_hlsetstop(L, b->watch ? &b->watch->stop : NULL);
    /* Lua HULL PATCH 0006: the stop flag and a tripped budget also end a
     * long ephemeron convergence inside the collector (conservatively). */
    lua_hlsettripped(L, &b->tripped);
    /* Lua HULL PATCH 0007: the dispatcher hears of every throw (an error or
     * a yield) that unwinds binding calls. */
    b->nframes = 0;
    lua_hlsetthrowhook(L, meter_throw, b);
    lua_pushcfunction(L, budget_pcall);
    lua_setglobal(L, "pcall");
    lua_pushcfunction(L, budget_xpcall);
    lua_setglobal(L, "xpcall");
}
