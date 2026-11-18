/*
 * mod_buildctx.h - the native pieces of a BuildAction (docs/buildcontext_design.md
 * §13.1, §13.2): hull.buildctx._fs (declared workspace inputs) and
 * hull.buildctx._store (the sealed result store), and hull.buildctx._plugin
 * (a build plugin's process and channel, §9.1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_LUA_MOD_BUILDCTX_H
#define HL_LUA_MOD_BUILDCTX_H

#include "lua.h"

int luaopen_hull_buildctx_fs(lua_State *L);
int luaopen_hull_buildctx_store(lua_State *L);
int luaopen_hull_buildctx_plugin(lua_State *L);

#endif /* HL_LUA_MOD_BUILDCTX_H */
