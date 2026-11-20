/*
 * plain_value.c - Lua values to and from the build plugin channel's value
 * encoding (see plain_value.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "plain_value.h"

#include "lauxlib.h"

#include <stdint.h>
#include <stdlib.h>

/* The bytes box's metatable, keyed in the registry by this address. */
static const char BYTES_KEY = 0;

/* The empty array's marker: a light userdata of this address. */
static char EMPTY_ARRAY;

void hl_lua_pv_push_empty_array(lua_State *L)
{
    lua_pushlightuserdata(L, &EMPTY_ARRAY);
}

static int is_bytes_box(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TUSERDATA || !lua_getmetatable(L, idx))
        return 0;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &BYTES_KEY);
    int same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return same;
}

void hl_lua_pv_push_bytes(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    luaL_checktype(L, idx, LUA_TSTRING);
    lua_newuserdatauv(L, 0, 1);
    lua_pushvalue(L, idx);
    lua_setiuservalue(L, -2, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &BYTES_KEY);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 2);
        lua_pushliteral(L, "hull.plain_value.bytes");
        lua_setfield(L, -2, "__name");
        lua_pushliteral(L, "locked");
        lua_setfield(L, -2, "__metatable");
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &BYTES_KEY);
    }
    lua_setmetatable(L, -2);
}

static int fail(HlPvWriter *w, int err)
{
    if (!w->err) w->err = err;
    return w->err;
}

static int encode(lua_State *L, int idx, HlPvWriter *w, int depth);

/* A table's shape: an array of 1..n, a map of string keys, or neither. */
static int table_shape(lua_State *L, int idx, lua_Integer *count, int *is_map)
{
    lua_Integer n = 0, max = 0;
    int ints = 0, strs = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        n++;
        if (lua_type(L, -1) == LUA_TSTRING) strs = 1;
        else if (lua_isinteger(L, -1)) {
            lua_Integer k = lua_tointeger(L, -1);
            if (k < 1) { lua_pop(L, 1); return -1; }
            if (k > max) max = k;
            ints = 1;
        } else { lua_pop(L, 1); return -1; }
    }
    if (ints && strs) return -1;
    if (ints && max != n) return -1;        /* a hole: not an array */
    if (n > UINT32_MAX) return -1;
    *count = n;
    *is_map = !ints;
    return 0;
}

static int encode_table(lua_State *L, int idx, HlPvWriter *w, int depth)
{
    if (depth >= HL_PV_MAX_DEPTH) return fail(w, HL_PV_E_DEPTH);
    if (lua_getmetatable(L, idx)) {
        lua_pop(L, 1);
        return fail(w, HL_PV_E_TYPE);
    }
    if (!lua_checkstack(L, 4)) return fail(w, HL_PV_E_NOMEM);
    lua_Integer n;
    int is_map;
    if (table_shape(L, idx, &n, &is_map) != 0) return fail(w, HL_PV_E_TYPE);
    if (!is_map) {
        hl_pv_put_array(w, (uint32_t)n);
        for (lua_Integer i = 1; i <= n && !w->err; i++) {
            lua_rawgeti(L, idx, i);
            encode(L, lua_gettop(L), w, depth + 1);
            lua_pop(L, 1);
        }
        return w->err;
    }
    hl_pv_put_map(w, (uint32_t)n);
    size_t start = w->len;
    size_t *ends = malloc((n ? (size_t)n : 1) * sizeof *ends);
    if (!ends) return fail(w, HL_PV_E_NOMEM);
    lua_Integer i = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (!w->err) {
            size_t kl;
            const char *k = lua_tolstring(L, -2, &kl);   /* a string key */
            hl_pv_put_key(w, k, kl);
            encode(L, lua_gettop(L), w, depth + 1);
            ends[i++] = w->len;
        }
        lua_pop(L, 1);
    }
    if (!w->err) hl_pv_sort_map(w, start, ends, (uint32_t)n);
    free(ends);
    return w->err;
}

static int encode(lua_State *L, int idx, HlPvWriter *w, int depth)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        hl_pv_put_nil(w);
        break;
    case LUA_TBOOLEAN:
        hl_pv_put_bool(w, lua_toboolean(L, idx));
        break;
    case LUA_TNUMBER:
        if (!lua_isinteger(L, idx)) return fail(w, HL_PV_E_TYPE);
        hl_pv_put_int(w, (int64_t)lua_tointeger(L, idx));
        break;
    case LUA_TSTRING: {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        hl_pv_put_text(w, s, n);
        break;
    }
    case LUA_TTABLE:
        return encode_table(L, idx, w, depth);
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(L, idx) != &EMPTY_ARRAY) return fail(w, HL_PV_E_TYPE);
        hl_pv_put_array(w, 0);
        break;
    case LUA_TUSERDATA:
        if (!is_bytes_box(L, idx)) return fail(w, HL_PV_E_TYPE);
        lua_getiuservalue(L, idx, 1);
        {
            size_t n;
            const char *s = lua_tolstring(L, -1, &n);
            hl_pv_put_bytes(w, s, n);
        }
        lua_pop(L, 1);
        break;
    default:
        return fail(w, HL_PV_E_TYPE);
    }
    return w->err;
}

int hl_lua_pv_encode(lua_State *L, int idx, HlPvWriter *w)
{
    idx = lua_absindex(L, idx);
    /* A nil inside an array cannot be written from Lua (the array would
     * have a hole), so arrays never hold one. */
    return encode(L, idx, w, w->depth);
}

static void decode(lua_State *L, HlPvReader *r)
{
    HlPvItem it;
    (void)hl_pv_next(r, &it);              /* checked: cannot fail */
    luaL_checkstack(L, 3, "plain value");
    switch (it.type) {
    case HL_PV_NIL:   lua_pushnil(L); break;
    case HL_PV_FALSE: lua_pushboolean(L, 0); break;
    case HL_PV_TRUE:  lua_pushboolean(L, 1); break;
    case HL_PV_INT:   lua_pushinteger(L, (lua_Integer)it.i); break;
    case HL_PV_TEXT:
    case HL_PV_BYTES: lua_pushlstring(L, (const char *)it.s, it.n); break;
    case HL_PV_ARRAY:
        lua_createtable(L, it.n > INT32_MAX ? INT32_MAX : (int)it.n, 0);
        for (size_t i = 0; i < it.n; i++) {
            decode(L, r);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        break;
    case HL_PV_MAP:
        lua_createtable(L, 0, it.n > INT32_MAX ? INT32_MAX : (int)it.n);
        for (size_t i = 0; i < it.n; i++) {
            const uint8_t *k;
            size_t kn;
            (void)hl_pv_next_key(r, &k, &kn);
            lua_pushlstring(L, (const char *)k, kn);
            decode(L, r);
            lua_rawset(L, -3);
        }
        break;
    }
}

int hl_lua_pv_decode(lua_State *L, const void *buf, size_t len)
{
    int rc = hl_pv_check(buf, len);
    if (rc != HL_PV_OK) return rc;
    HlPvReader r;
    hl_pv_reader_init(&r, buf, len);
    decode(L, &r);
    return HL_PV_OK;
}

static int pv_decode_k(lua_State *L)
{
    const void *buf = lua_touserdata(L, 1);
    size_t len = (size_t)lua_tointeger(L, 2);
    lua_settop(L, 0);
    int rc = hl_lua_pv_decode(L, buf, len);
    lua_pushinteger(L, rc);
    lua_insert(L, 1);
    return lua_gettop(L);   /* the code, then the value when it is 0 */
}

int hl_lua_pv_decode_protected(lua_State *L, const void *buf, size_t len)
{
    /* Nothing below allocates before the call, so nothing raises outside it. */
    if (!lua_checkstack(L, 4)) return HL_PV_E_NOMEM;
    lua_pushcfunction(L, pv_decode_k);
    lua_pushlightuserdata(L, (void *)(uintptr_t)buf);
    lua_pushinteger(L, (lua_Integer)len);
    if (lua_pcall(L, 2, 2, 0) != LUA_OK) return HL_LUA_PV_RAISED;   /* the error pushed */
    int rc = (int)lua_tointeger(L, -2);
    if (rc != HL_PV_OK) {
        lua_pop(L, 2);
        return rc;
    }
    lua_remove(L, -2);
    return HL_PV_OK;
}
