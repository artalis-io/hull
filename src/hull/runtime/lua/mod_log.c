/* mod_log.c - hull.log module: structured logging
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */
#include "log.h"
#include <stdint.h>
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
    /* The call (the caller lookup above), then every argument is a record
     * of its own (audit 41: the bytes alone were charged, so log.info("")
     * - a timestamp, an escape pass and an unbuffered stderr write - was
     * free, and a loop of it ran ~40 s per budget). */
    lua_hlwork(L, HL_WORK_LOG_CALL_UNITS, 0);
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (!s) {
            lua_pop(L, 1);
            continue;
        }
        /* Charged before the write (audit 24): log_log formats and writes
         * the whole message in this one call. */
        uint64_t u = hl_work_log_record_units(len);
        lua_hlwork(L, u > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)u, 0);
        /* CR / LF go out as the two characters \r / \n (audit 24, as the
         * JS runtime): a message holding a newline wrote a second log line
         * of the app's choosing - a forged "[hull:lua]" record. */
        if (memchr(s, '\n', len) || memchr(s, '\r', len)) {
            luaL_Buffer b;
            luaL_buffinit(L, &b);
            for (size_t k = 0; k < len; k++) {
                if (s[k] == '\n')
                    luaL_addlstring(&b, "\\n", 2);
                else if (s[k] == '\r')
                    luaL_addlstring(&b, "\\r", 2);
                else
                    luaL_addchar(&b, s[k]);
            }
            luaL_pushresult(&b);
            s = lua_tostring(L, -1);
            log_log(level, src, line, "%s %s", tag, s);
            lua_pop(L, 2);   /* the escaped copy and luaL_tolstring's */
            continue;
        }
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
