/*
 * refusal.h - raise a coded refusal from a Lua binding (audit 35; the codes
 * are in include/hull/shared/refusal.h).
 */
#ifndef HULL_RUNTIME_LUA_REFUSAL_H
#define HULL_RUNTIME_LUA_REFUSAL_H

#include <stdarg.h>
#include <lua.h>
#include "hull/shared/refusal.h"

/* luaL_error without the position: luaL_error prefixes the CALLER's
 * "chunk:line: " when a Lua function called the binding, which would put the
 * "[hull:<code>] " prefix (HL_LUA_REFUSAL_*, the first thing in @p fmt)
 * anywhere but the start. Same formats as lua_pushfstring. */
static inline int hl_lua_refuse(lua_State *L, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    return lua_error(L);
}

#endif /* HULL_RUNTIME_LUA_REFUSAL_H */
