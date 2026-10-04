/*
 * manifest_json.c - the declared manifest as JSON, produced in C
 *
 * What `hull build` signs and what --verify-sig compares at run time is the
 * manifest app.manifest() stored (registry "__hull_manifest"). Both used to be
 * produced by the runtime's Lua json.encode - the very table app code gets
 * from require("hull.json") (or "vendor.json") and can rewrite: an app that
 * replaced encode had a benign manifest signed and checked while the policy
 * the runtime actually enforces was read from the real table. This encoder
 * reads the stored table with raw accessors only (lua_rawget / lua_rawlen /
 * lua_next) and calls no Lua code, so nothing the app does can change its
 * output.
 *
 * Shape matches stdlib/lua/vendor/json.lua (the build used it until now):
 * a table that is empty or has [1] is an array (keys 1..n, no holes), any
 * other table an object with string keys; strings, numbers and booleans
 * encode as themselves. Anything else - functions, userdata, mixed or sparse
 * keys, keys with a NUL, nesting deeper than 32 - is refused (-1), as
 * json.encode raised on it. The comparison (hl_sig_check_runtime_policy) is
 * structural, so key order does not matter; keys are sorted anyway.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "lua.h"
#include "lauxlib.h"

#include <sh_json.h>

#include <stdlib.h>
#include <string.h>

#define MJ_MAX_DEPTH 32

typedef struct {
    const char *s;
    size_t      len;
} MjKey;

static int mj_key_cmp(const void *a, const void *b)
{
    const MjKey *x = (const MjKey *)a, *y = (const MjKey *)b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->s, y->s, n);
    if (c) return c;
    return (x->len > y->len) - (x->len < y->len);
}

static int mj_value(lua_State *L, int idx, ShJsonWriter *w, int depth);

/* The table at idx: array or object, per json.lua's rule. */
static int mj_table(lua_State *L, int idx, ShJsonWriter *w, int depth)
{
    if (depth > MJ_MAX_DEPTH || !lua_checkstack(L, 4))
        return -1;
    idx = lua_absindex(L, idx);

    lua_rawgeti(L, idx, 1);
    int has_first = !lua_isnil(L, -1);
    lua_pop(L, 1);
    lua_pushnil(L);
    int empty = (lua_next(L, idx) == 0);
    if (!empty) lua_pop(L, 2);

    if (has_first || empty) {
        /* Array: every key a number, no holes (count == rawlen). */
        lua_Integer n = 0;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            lua_pop(L, 1);
            if (lua_type(L, -1) != LUA_TNUMBER) { lua_pop(L, 1); return -1; }
            n++;
        }
        if ((lua_Unsigned)n != lua_rawlen(L, idx)) return -1;
        sh_json_write_array_start(w);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, idx, i);
            int rc = mj_value(L, -1, w, depth + 1);
            lua_pop(L, 1);
            if (rc != 0) return -1;
        }
        sh_json_write_array_end(w);
        return 0;
    }

    /* Object: string keys, sorted. The key strings stay alive in the table
     * (it is not modified), so their pointers hold while we write. */
    size_t cap = 8, cnt = 0;
    MjKey *keys = malloc(cap * sizeof *keys);
    if (!keys) return -1;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);
        size_t kl = 0;
        if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); free(keys); return -1; }
        const char *k = lua_tolstring(L, -1, &kl);
        if (memchr(k, '\0', kl)) { lua_pop(L, 1); free(keys); return -1; }
        if (cnt == cap) {
            MjKey *nk = (cap > SIZE_MAX / 2 / sizeof *keys) ? NULL
                      : realloc(keys, cap * 2 * sizeof *keys);
            if (!nk) { lua_pop(L, 1); free(keys); return -1; }
            keys = nk;
            cap *= 2;
        }
        keys[cnt].s = k;
        keys[cnt].len = kl;
        cnt++;
    }
    qsort(keys, cnt, sizeof *keys, mj_key_cmp);
    sh_json_write_object_start(w);
    int rc = 0;
    for (size_t i = 0; i < cnt && rc == 0; i++) {
        sh_json_write_key(w, keys[i].s);
        lua_pushlstring(L, keys[i].s, keys[i].len);
        lua_rawget(L, idx);
        rc = mj_value(L, -1, w, depth + 1);
        lua_pop(L, 1);
    }
    free(keys);
    if (rc != 0) return -1;
    sh_json_write_object_end(w);
    return 0;
}

static int mj_value(lua_State *L, int idx, ShJsonWriter *w, int depth)
{
    switch (lua_type(L, idx)) {
    case LUA_TSTRING: {
        size_t n = 0;
        const char *s = lua_tolstring(L, idx, &n);
        sh_json_write_string_n(w, s, n);
        return 0;
    }
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            sh_json_write_int(w, (int64_t)lua_tointeger(L, idx));
        else
            sh_json_write_double(w, (double)lua_tonumber(L, idx));
        return 0;
    case LUA_TBOOLEAN:
        sh_json_write_bool(w, lua_toboolean(L, idx) != 0);
        return 0;
    case LUA_TTABLE:
        return mj_table(L, idx, w, depth);
    default:
        return -1;
    }
}

/* Raises on failure; run under lua_pcall (args: lightuserdata ShJsonWriter) */
static int mj_encode_k(lua_State *L)
{
    ShJsonWriter *w = (ShJsonWriter *)lua_touserdata(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    if (!lua_istable(L, -1))
        return luaL_error(L, "no manifest");
    if (mj_table(L, -1, w, 0) != 0)
        return luaL_error(L, "app.manifest() is not serialisable");
    return 0;
}

int hl_lua_manifest_json(lua_State *L, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);
    int top = lua_gettop(L);
    lua_pushcfunction(L, mj_encode_k);
    lua_pushlightuserdata(L, &w);
    int rc = lua_pcall(L, 1, 0, 0);
    lua_settop(L, top);
    if (rc != LUA_OK || sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return -1;
    }
    char *copy = malloc(jb.len + 1);
    if (!copy) { sh_json_buf_free(&jb); return -1; }
    memcpy(copy, jb.buf, jb.len);
    copy[jb.len] = '\0';
    *out = copy;
    *out_len = jb.len;
    sh_json_buf_free(&jb);
    return 0;
}
