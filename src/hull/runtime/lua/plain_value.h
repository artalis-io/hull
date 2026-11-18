/*
 * plain_value.h - Lua values to and from the build plugin channel's value
 * encoding (include/hull/shared/plain_value.h).
 *
 * A Lua string encodes as text (it must be UTF-8); bytes travel in a box
 * made by hl_lua_pv_push_bytes. Decoding gives both as a plain string.
 * Integers only (a float is refused), tables with no metatable: 1..n keys
 * are an array, string keys a map, an empty table an empty map (an empty
 * array is the marker hl_lua_pv_push_empty_array pushes); a nil value in a
 * map is simply absent.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_LUA_PLAIN_VALUE_H
#define HL_LUA_PLAIN_VALUE_H

#include "hull/shared/plain_value.h"
#include "lua.h"

/* Encode the value at @p idx into @p w. Runs no Lua code and allocates
 * nothing in the VM (raw reads only, so no metamethod or finalizer runs).
 * HL_PV_OK or an HL_PV_E_* code (also left in w->err). */
int hl_lua_pv_encode(lua_State *L, int idx, HlPvWriter *w);

/* Check and decode @p buf, pushing the value. HL_PV_OK (one value pushed),
 * or an HL_PV_E_* code (nothing pushed). May raise a memory error. */
int hl_lua_pv_decode(lua_State *L, const void *buf, size_t len);

/* Push a bytes box holding the string at @p idx. */
void hl_lua_pv_push_bytes(lua_State *L, int idx);

/* Push the marker that encodes as an empty array. */
void hl_lua_pv_push_empty_array(lua_State *L);

#endif /* HL_LUA_PLAIN_VALUE_H */
