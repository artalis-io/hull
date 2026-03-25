/* mod_log.c — hull.log module: structured logging
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

#include "log.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.log module
 *
 * log.info(msg)
 * log.warn(msg)
 * log.error(msg)
 * log.debug(msg)
 * ════════════════════════════════════════════════════════════════════ */

static int lua_log_level(lua_State *L, int level)
{
    /* Extract Lua caller's source location */
    lua_Debug ar;
    const char *src = "lua";
    int line = 0;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
        src = ar.short_src;
        line = ar.currentline;
    }

    /* Detect stdlib vs app: embedded modules have "hull." or "vendor." source */
    const char *tag = "[app]";
    if (strncmp(src, "hull.", 5) == 0 || strncmp(src, "vendor.", 7) == 0)
        tag = "[hull:lua]";

    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        const char *s = luaL_tolstring(L, i, NULL);
        if (s)
            log_log(level, src, line, "%s %s", tag, s);
        lua_pop(L, 1);
    }
    return 0;
}

static int lua_log_info(lua_State *L)  { return lua_log_level(L, LOG_INFO); }
static int lua_log_warn(lua_State *L)  { return lua_log_level(L, LOG_WARN); }
static int lua_log_error(lua_State *L) { return lua_log_level(L, LOG_ERROR); }
static int lua_log_debug(lua_State *L) { return lua_log_level(L, LOG_DEBUG); }

static const luaL_Reg log_funcs[] = {
    {"info",  lua_log_info},
    {"warn",  lua_log_warn},
    {"error", lua_log_error},
    {"debug", lua_log_debug},
    {NULL, NULL}
};

int luaopen_hull_log(lua_State *L)
{
    luaL_newlib(L, log_funcs);
    return 1;
}
