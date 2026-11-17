/*
 * mod_buildctx_fs.h - hull.buildctx._fs, the one native piece of a BuildAction's
 * declared workspace inputs (docs/buildcontext_design.md §4, §13.1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_LUA_MOD_BUILDCTX_FS_H
#define HL_LUA_MOD_BUILDCTX_FS_H

#include "lua.h"

int luaopen_hull_buildctx_fs(lua_State *L);

#endif /* HL_LUA_MOD_BUILDCTX_FS_H */
