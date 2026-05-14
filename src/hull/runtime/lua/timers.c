/*
 * timers.c — Lua background timer support
 *
 * Implements `app.every()` / `app.daily()` timer callbacks. Hosts the
 * trampoline that fires from Keel's timer min-heap, drives the handler
 * coroutine, supports async yield via continuation, and reschedules.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/alloc.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include <sh_arena.h>

#include "log.h"

#include <assert.h>
#include <limits.h>
#include <string.h>
#include <time.h>

int64_t hl_compute_daily_delay_ms(int hour, int minute, int use_local)
{
    time_t now = time(NULL);
    struct tm now_tm;
    if (use_local)
        localtime_r(&now, &now_tm);
    else
        gmtime_r(&now, &now_tm);

    /* Compute seconds since midnight for current time */
    int64_t now_secs = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;
    int64_t target_secs = hour * 3600 + minute * 60;

    int64_t delta = target_secs - now_secs;
    if (delta <= 0)
        delta += 86400; /* next day */

    return delta * 1000;
}

int hl_lua_track_timer(HlLua *lua, void *timer)
{
    if (lua->timer_count >= lua->timer_cap) {
        size_t new_cap = lua->timer_cap ? lua->timer_cap * 2 : 4;
        if (new_cap < lua->timer_cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1;
        size_t old_sz = lua->timer_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(lua->base.alloc,
                                           lua->timers, old_sz, new_sz);
        if (!new_arr)
            return -1;
        lua->timers = new_arr;
        lua->timer_cap = new_cap;
    }
    lua->timers[lua->timer_count++] = timer;
    return 0;
}

void hl_lua_timer_reschedule(HlLuaTimer *t)
{
    HlLua *lua = t->lua;
    int64_t delay_ms;

    if (t->daily)
        delay_ms = hl_compute_daily_delay_ms(t->hour, t->minute, t->localtime);
    else
        delay_ms = t->interval_ms;

    t->timer_id = kl_timer_add(&lua->server->ev, (uint64_t)delay_ms,
                                hl_lua_timer_trampoline, t);
    if (t->timer_id < 0)
        log_error("[hull:timer] failed to reschedule timer (handler_id=%d)",
                  t->handler_id);
}

void hl_lua_timer_trampoline(void *user_data)
{
    HlLuaTimer *t = (HlLuaTimer *)user_data;
    HlLua *lua = t->lua;

    /* Skip if previous invocation still in flight (async) */
    if (t->in_flight) {
        t->timer_id = kl_timer_add(&lua->server->ev, 1000,
                                    hl_lua_timer_trampoline, t);
        return;
    }

    t->in_flight = 1;

    /* Reset scratch arena + guard stale txn */
    sh_arena_reset(lua->scratch);
    hl_db_guard_stale_txn(lua->base.db_handle);

    assert(lua->dispatch_depth == 0 && "timer fired during active dispatch");
    lua->dispatch_depth++;

    /* Clear per-request state (no connection) */
    lua->active_conn = NULL;
    lua->active_req = NULL;
    lua->active_thread_ref = LUA_NOREF;
    lua->active_co = NULL;
    lua->active_timer = t;

    /* Create coroutine for this timer invocation */
    lua_State *co = lua_newthread(lua->L);
    int thread_ref = luaL_ref(lua->L, LUA_REGISTRYINDEX);
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;

    /* Look up handler */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_timers");
    lua_rawgeti(lua->L, -1, t->handler_id);
    lua_xmove(lua->L, co, 1);
    lua_pop(lua->L, 1); /* pop __hull_timers */

    /* Re-arm instruction limit for this callback */
    if (lua->max_instructions > 0) {
        lua_sethook(co, NULL, 0, 0); /* clear first */
        lua_sethook(co, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));
    }

    int nres = 0;
    int status = lua_resume(co, lua->L, 0, &nres);

    if (status == LUA_OK) {
        /* Synchronous completion */
        int cancelled = 0;
        if (nres > 0 && lua_isboolean(co, -1) && !lua_toboolean(co, -1))
            cancelled = 1;

        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_timer = NULL;
        lua->dispatch_depth--;
        t->in_flight = 0;

        if (!cancelled)
            hl_lua_timer_reschedule(t);
    } else if (status == LUA_YIELD) {
        /* Handler yielded (async op in flight).
         * The continuation was created by the yielding function and
         * already has timer_ctx wired (via lua->active_timer).
         * dispatch_depth stays at 1 — decremented on async resume.
         * When async completes, hl_lua_async_resume will clear
         * in_flight and reschedule. Nothing to do here. */
    } else {
        /* Error — log, reschedule anyway */
        const char *err = lua_tostring(co, -1);
        log_error("[hull:timer] %s", err ? err : "unknown error");
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_timer = NULL;
        lua->dispatch_depth--;
        t->in_flight = 0;
        hl_lua_timer_reschedule(t);
    }

    lua->active_timer = NULL;
}
