/* mod_time.c - hull.time module: timestamps, date formatting
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/time.h"

/* ════════════════════════════════════════════════════════════════════
 * hull.time module
 *
 * time.now()      → Unix timestamp (seconds)
 * time.now_ms()   → milliseconds since epoch
 * time.clock()    → monotonic ms
 * time.date()     → "YYYY-MM-DD"
 * time.datetime() → "YYYY-MM-DDTHH:MM:SSZ"
 * ════════════════════════════════════════════════════════════════════ */

static int lua_time_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_cap_time_now());
    return 1;
}

static int lua_time_now_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_cap_time_now_ms());
    return 1;
}

static int lua_time_clock(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_cap_time_clock());
    return 1;
}

static int lua_time_date(lua_State *L)
{
    char buf[16];
    if (hl_cap_time_date(buf, sizeof(buf)) != 0)
        return luaL_error(L, "time.date() failed");
    lua_pushstring(L, buf);
    return 1;
}

static int lua_time_datetime(lua_State *L)
{
    char buf[32];
    if (hl_cap_time_datetime(buf, sizeof(buf)) != 0)
        return luaL_error(L, "time.datetime() failed");
    lua_pushstring(L, buf);
    return 1;
}

static const luaL_Reg time_funcs[] = {
    {"now",      lua_time_now},
    {"now_ms",   lua_time_now_ms},
    {"clock",    lua_time_clock},
    {"date",     lua_time_date},
    {"datetime", lua_time_datetime},
    {NULL, NULL}
};

int luaopen_hull_time(lua_State *L)
{
    luaL_newlib(L, time_funcs);
    return 1;
}
