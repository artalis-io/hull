/* mod_exec.h - hull.exec (Lua) and its hull test stubs
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_LUA_MOD_EXEC_H
#define HULL_LUA_MOD_EXEC_H

#include "lua.h"

int  luaopen_hull_exec(lua_State *L);
/* test.exec_stub(name, result | fn | nil) - hull test only. */
int  hl_lua_exec_stub(lua_State *L);
/* (Re)create the stub table: hull test's registration and each file's
 * reset. Its presence is what tells exec.run it runs under hull test. */
void hl_lua_exec_stubs_reset(lua_State *L);

#endif /* HULL_LUA_MOD_EXEC_H */
