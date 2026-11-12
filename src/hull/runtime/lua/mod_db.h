/* mod_db.h - internal seam between mod_db.c (base runtime) and the composed
 * SQLite UDF bridge mod_db_udf.c, for udf on a db.open() connection.
 *
 * The bridge's udf functions on a db.open() connection carry the owner box
 * (a userdata) as upvalue 1. The UDF functions it registers are kept in the
 * box's user value rather than the registry, so they live exactly as long as
 * the connection object (a UDF that captures its own connection does not pin
 * it); these helpers are the only way the bridge reaches them.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_RUNTIME_LUA_MOD_DB_H
#define HULL_RUNTIME_LUA_MOD_DB_H

#ifdef HL_ENABLE_DB

#include "lua.h"
#include "hull/cap/db_backend.h"

/* The handle of the db.open() owner box at @p idx (NULL once closed), its
 * box in *box; NULL and *box = NULL when @p idx is not an owner box. */
HlDbHandle *hl_lua_db_owned_handle(lua_State *L, int idx, void **box);

/* Keep the function at @p fn_idx in the owner box at @p box_idx; returns its
 * key. Raises (out of memory). */
lua_Integer hl_lua_db_udf_hold(lua_State *L, int box_idx, int fn_idx);

/* Push the function kept under @p key in @p box (1), or push nothing (0)
 * when the box is gone. Allocates nothing. */
int hl_lua_db_udf_push(lua_State *L, void *box, lua_Integer key);

/* Drop the function kept under @p key; nothing when the box is gone (it is
 * being collected, its holder with it). Raises nothing, allocates nothing. */
void hl_lua_db_udf_release(lua_State *L, void *box, lua_Integer key);

/* Attach `udf` to the db.open() connection table at the top of the stack,
 * bound to the owner box at @p box_idx. Weak no-op in mod_db.c; strong
 * override in mod_db_udf.c. */
void hl_lua_db_attach_owned_udf(lua_State *L, int box_idx);

#endif /* HL_ENABLE_DB */
#endif /* HULL_RUNTIME_LUA_MOD_DB_H */
