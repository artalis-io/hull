/*
 * protected.h - Lua pushes that cannot raise, for C code that must not be
 * unwound through.
 *
 * Any Lua allocation can raise LUA_ERRMEM, and a raise is a longjmp. Two
 * kinds of C code cannot take one:
 *
 *   - a callback invoked from inside a C library mid-operation (a database
 *     row callback runs inside the Postgres / MySQL read loop or between
 *     sqlite3_step calls; a UDF runs inside sqlite3_step). A longjmp out of
 *     it leaves the library mid-operation: a wire connection one reply
 *     behind, so the next query reads this one's rows; a statement never
 *     reset.
 *   - a binding holding a C buffer it must free (a malloc'd result from a
 *     capability). A raise while copying it into Lua skips the free, and the
 *     buffer is outside the Lua heap limit, so each failure leaks it.
 *
 * Both do their Lua work inside lua_pcall through these helpers and act on
 * the outcome once they are back in safe code. Pushing a light C function or
 * a light userdata does not allocate, so setting up the call cannot raise.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_RUNTIME_LUA_PROTECTED_H
#define HL_RUNTIME_LUA_PROTECTED_H

#include <stddef.h>
#include <stdint.h>

#include "hull/cap/types.h"   /* HlColumn */
#include "lua.h"
#include "lauxlib.h"
#include <stdio.h>

typedef struct {
    const char *p;
    size_t      n;
} HlLuaBytes;

static inline int hl_lua_pushlstring_k(lua_State *L)
{
    const HlLuaBytes *b = (const HlLuaBytes *)lua_touserdata(L, 1);
    lua_pushlstring(L, b->p, b->n);
    return 1;
}

/* Push a copy of [p, p + n). 0 when pushed; -1 when it could not be (out of
 * memory), with nothing pushed - the caller frees what it owns, then raises. */
static inline int hl_lua_pushlstring_safe(lua_State *L, const void *p, size_t n)
{
    if (!lua_checkstack(L, 2))
        return -1;
    HlLuaBytes b = { (const char *)p, n };
    lua_pushcfunction(L, hl_lua_pushlstring_k);
    lua_pushlightuserdata(L, &b);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

static inline int hl_lua_newslot_k(lua_State *L)
{
    const char *mt = (const char *)lua_touserdata(L, 1);
    void **slot = (void **)lua_newuserdatauv(L, sizeof(void *), 0);
    *slot = NULL;
    luaL_setmetatable(L, mt);
    return 1;
}

/* Push a new pointer-slot userdata with metatable @p mt and store @p obj in
 * it. 0 when pushed; -1 when it could not be (out of memory), with nothing
 * pushed and @p obj NOT stored - the caller frees it, then raises. Made the
 * plain way, the C object was created first and a failure making its
 * userdata raised past it (a WASM / GPU output buffer, a decoded image). */
static inline int hl_lua_push_slot_safe(lua_State *L, const char *mt, void *obj)
{
    if (!lua_checkstack(L, 2))
        return -1;
    lua_pushcfunction(L, hl_lua_newslot_k);
    lua_pushlightuserdata(L, (void *)(uintptr_t)mt);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return -1;
    }
    *(void **)lua_touserdata(L, -1) = obj;
    return 0;
}

typedef struct {
    const HlColumn *cols;
    int             ncols;
    lua_Integer     row;
} HlLuaRow;

/* (args: row, result table) */
static inline int hl_lua_append_row_k(lua_State *L)
{
    const HlLuaRow *r = (const HlLuaRow *)lua_touserdata(L, 1);
    luaL_checkstack(L, 2, "db row");
    lua_createtable(L, 0, r->ncols > 0 ? r->ncols : 0);
    for (int i = 0; i < r->ncols; i++) {
        const HlValue *v = &r->cols[i].value;
        switch (v->type) {
        case HL_TYPE_INT:    lua_pushinteger(L, (lua_Integer)v->i); break;
        case HL_TYPE_DOUBLE: lua_pushnumber(L, (lua_Number)v->d);   break;
        case HL_TYPE_TEXT:
        case HL_TYPE_BLOB:   lua_pushlstring(L, v->s, v->len);     break;
        case HL_TYPE_BOOL:   lua_pushboolean(L, v->b);              break;
        case HL_TYPE_NIL:
        default:             lua_pushnil(L);                        break;
        }
        lua_setfield(L, -2, r->cols[i].name ? r->cols[i].name : "?");
    }
    lua_rawseti(L, 2, r->row);
    return 0;
}

/* Build one result row as a table and store it at result[row]. For a database
 * row callback: it runs inside the backend's read loop, so it must not raise.
 * 0 on success; -1 when building failed (out of memory) - the callback stops
 * the query (a non-zero return makes every backend drain to the end of the
 * reply) and the binding raises once hl_db_query has returned. */
static inline int hl_lua_append_row(lua_State *L, int result_idx, lua_Integer row,
                                    const HlColumn *cols, int ncols)
{
    if (!lua_checkstack(L, 3))
        return -1;
    result_idx = lua_absindex(L, result_idx);
    HlLuaRow r = { cols, ncols, row };
    lua_pushcfunction(L, hl_lua_append_row_k);
    lua_pushlightuserdata(L, &r);
    lua_pushvalue(L, result_idx);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

/* (args: lightuserdata HlLuaStrItem, table) */
typedef struct {
    const void  *s;
    size_t       len;
    lua_Integer  idx;
} HlLuaStrItem;

static inline int hl_lua_append_lstring_k(lua_State *L)
{
    const HlLuaStrItem *it = (const HlLuaStrItem *)lua_touserdata(L, 1);
    lua_pushlstring(L, (const char *)it->s, it->len);
    lua_rawseti(L, 2, it->idx);
    return 0;
}

/* Store a copy of s[0..len) at tbl[idx] - both the string and the table
 * growth protected, for a callback that runs inside a C loop (a backend's
 * scan) and so must not raise. 0, or -1 when out of memory. */
static inline int hl_lua_append_lstring(lua_State *L, int tbl, lua_Integer idx,
                                        const void *s, size_t len)
{
    if (!lua_checkstack(L, 3))
        return -1;
    tbl = lua_absindex(L, tbl);
    HlLuaStrItem it = { s, len, idx };
    lua_pushcfunction(L, hl_lua_append_lstring_k);
    lua_pushlightuserdata(L, &it);
    lua_pushvalue(L, tbl);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

int hl_lua_budget_tripped(lua_State *L);   /* budget.c */
int hl_lua_budget_raise(lua_State *L);

/* Raise "<prefix><msg>" with msg COPIED first. luaL_error formats lazily,
 * after luaL_where has allocated - a GC step that can run an app finalizer
 * which closes the connection msg points into (hl_db_errmsg /
 * hl_cap_kv_error), so the format then read freed memory. */
static inline int hl_lua_raise_copy(lua_State *L, const char *prefix,
                                    const char *msg)
{
    /* The statement was interrupted because the run is over its instruction
     * budget (cap/db_budget.h): raise the limit, not "interrupted" as an
     * ordinary SQL error (audit 9 H4). */
    if (hl_lua_budget_tripped(L))
        return hl_lua_budget_raise(L);
    char buf[512];
    snprintf(buf, sizeof buf, "%s%s", prefix ? prefix : "",
             msg ? msg : "(unknown error)");
    return luaL_error(L, "%s", buf);
}

#endif /* HL_RUNTIME_LUA_PROTECTED_H */
