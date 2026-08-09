/*
 * mod_kv.c: Lua native binding for the app-facing KV connection cap (cap/kv.c).
 *
 * Exposes an internal module `hull.kv._native` with one entry, open(dsn) ->
 * connection userdata. The connection carries method-style ops (conn:get,
 * conn:set, ...) plus close(); the metatable's __gc releases the handle if
 * close() did not, so a dropped connection never leaks. The stdlib valkey
 * backend (hull.kv._valkey) is the sole caller; app code reaches it through
 * hull.kv / hull.cache with backend="valkey".
 *
 * Borrow-copy invariant: get() returns a value BORROWED into the connection's
 * receive buffer, valid only until the next op on the same connection. This
 * binding copies it into a runtime-owned Lua string via lua_pushlstring BEFORE
 * returning, so the script value survives any subsequent op. Regression-tested
 * end to end in tests/e2e_valkey.sh (get -> another op -> first value unchanged).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"        /* get_hl_lua, luaopen decls */
#include "hull/cap/kv.h"

#include <string.h>

#define HL_LUA_KV_MT "hull.kv.conn"

typedef struct {
    HlKvConn *c;   /* owned; NULL after close (idempotent; __gc-safe) */
} HlLuaKvConn;

/* Resolve the live connection at stack arg 1, or raise if closed. */
static HlKvConn *kv_self(lua_State *L)
{
    HlLuaKvConn *o = luaL_checkudata(L, 1, HL_LUA_KV_MT);
    if (!o->c) return (HlKvConn *)(luaL_error(L, "kv: connection is closed"), NULL);
    return o->c;
}

/* Optional integer TTL in milliseconds at `idx` (nil/absent -> 0 = no expiry). */
static int64_t kv_opt_ttl_ms(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) return 0;
    return (int64_t)luaL_checkinteger(L, idx);
}

/* conn:get(key) -> value (bytes) | nil. Copies the borrowed value. */
static int l_kv_get(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t klen; const char *key = luaL_checklstring(L, 2, &klen);
    const uint8_t *val; size_t vlen; int found = 0;
    if (hl_cap_kv_get(c, (const uint8_t *)key, klen, &val, &vlen, &found) != 0)
        return luaL_error(L, "%s", hl_cap_kv_error(c));
    if (!found) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)val, vlen);   /* COPY: borrow-copy guard */
    return 1;
}

/* conn:set(key, val, ttl_ms?) -> true. */
static int l_kv_set(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t klen, vlen;
    const char *key = luaL_checklstring(L, 2, &klen);
    const char *val = luaL_checklstring(L, 3, &vlen);
    int64_t ttl = kv_opt_ttl_ms(L, 4);
    if (hl_cap_kv_set(c, (const uint8_t *)key, klen, (const uint8_t *)val, vlen, ttl) != 0)
        return luaL_error(L, "%s", hl_cap_kv_error(c));
    lua_pushboolean(L, 1);
    return 1;
}

/* conn:del(key) -> bool (existed). */
static int l_kv_del(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t klen; const char *key = luaL_checklstring(L, 2, &klen);
    int deleted = 0;
    if (hl_cap_kv_del(c, (const uint8_t *)key, klen, &deleted) != 0)
        return luaL_error(L, "%s", hl_cap_kv_error(c));
    lua_pushboolean(L, deleted);
    return 1;
}

/* conn:has(key) -> bool. */
static int l_kv_has(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t klen; const char *key = luaL_checklstring(L, 2, &klen);
    int present = 0;
    if (hl_cap_kv_exists(c, (const uint8_t *)key, klen, &present) != 0)
        return luaL_error(L, "%s", hl_cap_kv_error(c));
    lua_pushboolean(L, present);
    return 1;
}

/* conn:incr(key, by, ttl_ms?) -> integer newval. */
static int l_kv_incr(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t klen; const char *key = luaL_checklstring(L, 2, &klen);
    int64_t by = (int64_t)luaL_checkinteger(L, 3);
    int64_t ttl = kv_opt_ttl_ms(L, 4);
    int64_t nv = 0;
    if (hl_cap_kv_incr(c, (const uint8_t *)key, klen, by, ttl, &nv) != 0)
        return luaL_error(L, "%s", hl_cap_kv_error(c));
    lua_pushinteger(L, (lua_Integer)nv);
    return 1;
}

/* conn:cas(key, expected|nil, new, ttl_ms?) -> 0 ok / 1 mismatch / 2 conflict.
 * A transport/protocol ERROR raises. The wrapper maps 0->true, else->false. */
static int l_kv_cas(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t klen, elen = 0, nlen;
    const char *key = luaL_checklstring(L, 2, &klen);
    int has_expected = !lua_isnoneornil(L, 3);
    const char *expected = has_expected ? luaL_checklstring(L, 3, &elen) : NULL;
    const char *newv = luaL_checklstring(L, 4, &nlen);
    int64_t ttl = kv_opt_ttl_ms(L, 5);
    HlKvCasResult r = hl_cap_kv_cas(c, (const uint8_t *)key, klen,
                                    (const uint8_t *)expected, elen, has_expected,
                                    (const uint8_t *)newv, nlen, ttl);
    if (r == HL_KV_CAS_ERROR) return luaL_error(L, "%s", hl_cap_kv_error(c));
    lua_pushinteger(L, (lua_Integer)r);   /* 0/1/2 */
    return 1;
}

/* conn:clear(prefix) -> integer removed. */
static int l_kv_clear(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t plen; const char *prefix = luaL_checklstring(L, 2, &plen);
    int64_t removed = 0;
    if (hl_cap_kv_clear(c, (const uint8_t *)prefix, plen, &removed) != 0)
        return luaL_error(L, "%s", hl_cap_kv_error(c));
    lua_pushinteger(L, (lua_Integer)removed);
    return 1;
}

/* scan collector: append each borrowed key (prefix already stripped by the
 * backend) as a Lua byte string into a result table. */
struct kv_scan_lua { lua_State *L; int tbl; lua_Integer n; };
static int kv_scan_lua_cb(void *ctx, const uint8_t *key, size_t klen)
{
    struct kv_scan_lua *s = (struct kv_scan_lua *)ctx;
    lua_pushlstring(s->L, (const char *)key, klen);   /* COPY inside the callback */
    lua_rawseti(s->L, s->tbl, ++s->n);
    return 0;
}

/* conn:scan(prefix, limit?) -> { key, ... } (prefix-stripped app keys). */
static int l_kv_scan(lua_State *L)
{
    HlKvConn *c = kv_self(L);
    size_t plen; const char *prefix = luaL_checklstring(L, 2, &plen);
    size_t limit = (size_t)luaL_optinteger(L, 3, 0);
    lua_newtable(L);
    struct kv_scan_lua s = { L, lua_gettop(L), 0 };
    if (hl_cap_kv_scan(c, (const uint8_t *)prefix, plen, limit, kv_scan_lua_cb, &s) != 0)
        return luaL_error(L, "%s", hl_cap_kv_error(c));
    return 1;   /* the table is on top */
}

/* conn:caps() -> integer bitmask (HL_KV_CAP_*). */
static int l_kv_caps(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_cap_kv_caps(kv_self(L)));
    return 1;
}

/* conn:backend_name() -> string. */
static int l_kv_backend_name(lua_State *L)
{
    lua_pushstring(L, hl_cap_kv_backend_name(kv_self(L)));
    return 1;
}

/* conn:close() -> release now (idempotent). */
static int l_kv_close(lua_State *L)
{
    HlLuaKvConn *o = luaL_checkudata(L, 1, HL_LUA_KV_MT);
    if (o->c) { hl_cap_kv_close(o->c); o->c = NULL; }
    return 0;
}

static int l_kv_gc(lua_State *L)
{
    HlLuaKvConn *o = luaL_checkudata(L, 1, HL_LUA_KV_MT);
    if (o->c) { hl_cap_kv_close(o->c); o->c = NULL; }
    return 0;
}

/* hull.kv._native.open(dsn [, timeout_ms]) -> connection userdata. */
static int l_kv_open(lua_State *L)
{
    const char *dsn = luaL_checkstring(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);
    HlLua *lua = get_hl_lua(L);

    char err[256];
    /* Enforce the manifest kv.dynamic allowlist (scheme + host) before dialing.
     * Fails closed with a policy message when no policy is declared. */
    if (hl_cap_kv_check_dsn(lua ? lua->base.kv_policy : NULL, dsn, err, sizeof err) != 0)
        return luaL_error(L, "%s", err);

    HlKvConn *c = NULL;
    if (hl_cap_kv_open(&c, dsn, timeout_ms, lua ? lua->base.alloc : NULL,
                       err, sizeof err) != 0)
        return luaL_error(L, "%s", err);

    HlLuaKvConn *o = lua_newuserdatauv(L, sizeof *o, 0);
    o->c = c;
    luaL_setmetatable(L, HL_LUA_KV_MT);   /* installs __gc + method __index */
    return 1;
}

static const luaL_Reg kv_methods[] = {
    { "get",          l_kv_get },
    { "set",          l_kv_set },
    { "del",          l_kv_del },
    { "has",          l_kv_has },
    { "incr",         l_kv_incr },
    { "cas",          l_kv_cas },
    { "clear",        l_kv_clear },
    { "scan",         l_kv_scan },
    { "caps",         l_kv_caps },
    { "backend_name", l_kv_backend_name },
    { "close",        l_kv_close },
    { NULL, NULL },
};

int luaopen_hull_kv_native(lua_State *L)
{
    /* Connection metatable: __index = methods table, __gc = finalizer. */
    luaL_newmetatable(L, HL_LUA_KV_MT);
    lua_pushcfunction(L, l_kv_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, kv_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);   /* pop the metatable */

    lua_newtable(L);
    lua_pushcfunction(L, l_kv_open);
    lua_setfield(L, -2, "open");
    return 1;
}
