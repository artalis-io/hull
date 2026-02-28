/*
 * lua_modules.c — hull.* built-in module implementations for Lua 5.4
 *
 * Each module is registered as a Lua library via luaL_newlib().
 * All capability calls go through hl_cap_* — no direct SQLite,
 * filesystem, or network access from this file.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "hull/limits.h"
#include "hull/cap/db.h"
#include "hull/cap/time.h"
#include "hull/cap/env.h"
#include "hull/cap/http.h"
#include "hull/cap/crypto.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <sh_arena.h>

#include "log.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Embedded stdlib (auto-generated registry of all stdlib .lua files) */

#include "stdlib_lua_registry.h"

/* App entries: default empty in app_entries_default.c, overridden by
 * generated app_registry.o when building with APP_DIR or hull build. */
extern const HlStdlibEntry hl_app_lua_entries[];

/* ── Helper: retrieve HlLua from registry ─────────────────────────── */

static HlLua *get_hl_lua(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua;
}

/* ════════════════════════════════════════════════════════════════════
 * hull.app module
 *
 * Provides route registration: app.get(), app.post(), app.use(), etc.
 * Routes are stored in the Lua registry:
 *   registry["__hull_routes"]     = { [1]=fn, [2]=fn, ... }
 *   registry["__hull_route_defs"] = { [1]={method,pattern,handler_id}, ... }
 * ════════════════════════════════════════════════════════════════════ */

/* Helper: register a route with given method string */
static int lua_app_route(lua_State *L, const char *method)
{
    const char *pattern = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* Ensure __hull_routes table exists in registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    }

    /* Ensure __hull_route_defs table exists in registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    }

    /* Get current length of routes (next index = len + 1, 1-based) */
    lua_Integer idx = (lua_Integer)luaL_len(L, -2) + 1;

    /* Store handler function in __hull_routes[idx] */
    /* Stack: routes_table, defs_table */
    lua_pushvalue(L, 2); /* push handler function */
    lua_rawseti(L, -3, idx); /* routes[idx] = handler */

    /* Store route definition in __hull_route_defs[idx] */
    lua_newtable(L);
    lua_pushstring(L, method);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, pattern);
    lua_setfield(L, -2, "pattern");
    lua_pushinteger(L, idx);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, idx); /* defs[idx] = def */

    lua_pop(L, 2); /* pop routes_table, defs_table */
    return 0;
}

static int lua_app_get(lua_State *L)    { return lua_app_route(L, "GET"); }
static int lua_app_post(lua_State *L)   { return lua_app_route(L, "POST"); }
static int lua_app_put(lua_State *L)    { return lua_app_route(L, "PUT"); }
static int lua_app_del(lua_State *L)    { return lua_app_route(L, "DELETE"); }
static int lua_app_patch(lua_State *L)  { return lua_app_route(L, "PATCH"); }

/* app.use(method, pattern, handler) — middleware registration */
static int lua_app_use(lua_State *L)
{
    const char *method = luaL_checkstring(L, 1);
    const char *pattern = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    /* Store handler in __hull_routes (same array as route handlers) */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    }

    lua_Integer handler_id = (lua_Integer)luaL_len(L, -1) + 1;
    lua_pushvalue(L, 3);
    lua_rawseti(L, -2, handler_id);
    lua_pop(L, 1); /* pop routes table */

    /* Store middleware entry with handler_id */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    }

    lua_Integer idx = (lua_Integer)luaL_len(L, -1) + 1;

    lua_newtable(L);
    lua_pushstring(L, method);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, pattern);
    lua_setfield(L, -2, "pattern");
    lua_pushinteger(L, handler_id);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, idx);

    lua_pop(L, 1); /* pop middleware table */
    return 0;
}

/* app.config(tbl) — application configuration */
static int lua_app_config(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_config");
    return 0;
}

/* app.manifest(tbl) — declare application capabilities */
static int lua_app_manifest(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    return 0;
}

/* app.get_manifest() — retrieve manifest table (for build tools) */
static int lua_app_get_manifest(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    if (lua_isnil(L, -1))
        return 1; /* returns nil */
    return 1;
}

static const luaL_Reg app_funcs[] = {
    {"get",          lua_app_get},
    {"post",         lua_app_post},
    {"put",          lua_app_put},
    {"del",          lua_app_del},
    {"patch",        lua_app_patch},
    {"use",          lua_app_use},
    {"config",       lua_app_config},
    {"manifest",     lua_app_manifest},
    {"get_manifest", lua_app_get_manifest},
    {NULL, NULL}
};

static int luaopen_hull_app(lua_State *L)
{
    luaL_newlib(L, app_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * hull.db module
 *
 * db.query(sql, params?) → array of row tables
 * db.exec(sql, params?)  → number of rows affected
 * db.last_id()           → last insert rowid
 * ════════════════════════════════════════════════════════════════════ */

/* Callback context for building Lua result table from hl_cap_db_query */
typedef struct {
    lua_State *L;
    int        table_idx; /* absolute stack index of result table */
    int        row_count;
} LuaQueryCtx;

static int lua_query_row_cb(void *opaque, HlColumn *cols, int ncols)
{
    LuaQueryCtx *qc = (LuaQueryCtx *)opaque;
    qc->row_count++;

    lua_newtable(qc->L);
    for (int i = 0; i < ncols; i++) {
        switch (cols[i].value.type) {
        case HL_TYPE_INT:
            lua_pushinteger(qc->L, (lua_Integer)cols[i].value.i);
            break;
        case HL_TYPE_DOUBLE:
            lua_pushnumber(qc->L, (lua_Number)cols[i].value.d);
            break;
        case HL_TYPE_TEXT:
            lua_pushlstring(qc->L, cols[i].value.s, cols[i].value.len);
            break;
        case HL_TYPE_BLOB:
            lua_pushlstring(qc->L, cols[i].value.s, cols[i].value.len);
            break;
        case HL_TYPE_BOOL:
            lua_pushboolean(qc->L, cols[i].value.b);
            break;
        case HL_TYPE_NIL:
        default:
            lua_pushnil(qc->L);
            break;
        }
        lua_setfield(qc->L, -2, cols[i].name);
    }

    lua_rawseti(qc->L, qc->table_idx, qc->row_count);
    return 0;
}

/* Marshal Lua table values to HlValue array for parameter binding */
static int lua_to_hl_values(lua_State *L, int idx,
                               HlValue **out_params, int *out_count)
{
    *out_params = NULL;
    *out_count = 0;

    if (lua_isnoneornil(L, idx))
        return 0;

    luaL_checktype(L, idx, LUA_TTABLE);
    int len = (int)luaL_len(L, idx);
    if (len <= 0)
        return 0;

    /* Overflow guard */
    if ((size_t)len > SIZE_MAX / sizeof(HlValue))
        return -1;

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return -1;

    HlValue *params = sh_arena_calloc(lua->scratch, (size_t)len, sizeof(HlValue));
    if (!params)
        return -1;

    for (int i = 0; i < len; i++) {
        lua_rawgeti(L, idx, i + 1); /* Lua tables are 1-based */
        int t = lua_type(L, -1);

        switch (t) {
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1)) {
                params[i].type = HL_TYPE_INT;
                params[i].i = (int64_t)lua_tointeger(L, -1);
            } else {
                params[i].type = HL_TYPE_DOUBLE;
                params[i].d = (double)lua_tonumber(L, -1);
            }
            break;
        case LUA_TSTRING: {
            size_t slen;
            const char *s = lua_tolstring(L, -1, &slen);
            params[i].type = HL_TYPE_TEXT;
            params[i].s = s; /* valid while on Lua stack */
            params[i].len = slen;
            break;
        }
        case LUA_TBOOLEAN:
            params[i].type = HL_TYPE_BOOL;
            params[i].b = lua_toboolean(L, -1);
            break;
        case LUA_TNIL:
        default:
            params[i].type = HL_TYPE_NIL;
            break;
        }
        /* Leave values on stack — they keep strings alive */
    }

    *out_params = params;
    *out_count = len;
    return 0;
}

static void lua_free_hl_values(lua_State *L, HlValue *params, int count)
{
    if (!params)
        return;
    /* Pop the values we left on the stack in lua_to_hl_values.
     * No free() — params live in the per-request scratch arena. */
    if (count > 0)
        lua_pop(L, count);
}

/* db.query(sql, params?) */
static int lua_db_query(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.db)
        return luaL_error(L, "database not available");

    const char *sql = luaL_checkstring(L, 1);

    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    /* Create result table */
    lua_newtable(L);
    int table_idx = lua_gettop(L);

    LuaQueryCtx qc = {
        .L = L,
        .table_idx = table_idx,
        .row_count = 0,
    };

    int rc = hl_cap_db_query(lua->base.db, sql, params, nparams,
                                lua_query_row_cb, &qc, lua->base.alloc);

    /*
     * lua_to_hl_values left nparams values on the stack (to keep string
     * pointers alive during the query).  The result table sits on top of
     * them.  Rotate it below the param values so lua_free_hl_values pops
     * the right things.
     *
     * Before rotate: [... param_1 .. param_n result_table]
     * After rotate:  [... result_table param_1 .. param_n]
     */
    if (nparams > 0)
        lua_rotate(L, table_idx - nparams, 1);

    lua_free_hl_values(L, params, nparams);

    if (rc != 0) {
        lua_pop(L, 1); /* pop result table */
        return luaL_error(L, "query failed: %s", sqlite3_errmsg(lua->base.db));
    }

    return 1; /* result table already on stack */
}

/* db.exec(sql, params?) */
static int lua_db_exec(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.db)
        return luaL_error(L, "database not available");

    const char *sql = luaL_checkstring(L, 1);

    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    int rc = hl_cap_db_exec(lua->base.db, sql, params, nparams);

    lua_free_hl_values(L, params, nparams);

    if (rc < 0)
        return luaL_error(L, "exec failed: %s", sqlite3_errmsg(lua->base.db));

    lua_pushinteger(L, rc);
    return 1;
}

/* db.last_id() */
static int lua_db_last_id(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.db)
        return luaL_error(L, "database not available");

    lua_pushinteger(L, (lua_Integer)hl_cap_db_last_id(lua->base.db));
    return 1;
}

static const luaL_Reg db_funcs[] = {
    {"query",   lua_db_query},
    {"exec",    lua_db_exec},
    {"last_id", lua_db_last_id},
    {NULL, NULL}
};

static int luaopen_hull_db(lua_State *L)
{
    luaL_newlib(L, db_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * hull.time module
 *
 * time.now()      → Unix timestamp (seconds)
 * time.now_ms()   → milliseconds since epoch
 * time.clock()    → monotonic ms
 * time.date()     → "YYYY-MM-DD"
 * time.datetime() → "YYYY-MM-DDTHH:MM:SSZ"
 * ════════════════════════════════════════════════════════════════════ */

static int lua_time_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_cap_time_now());
    return 1;
}

static int lua_time_now_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_cap_time_now_ms());
    return 1;
}

static int lua_time_clock(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_cap_time_clock());
    return 1;
}

static int lua_time_date(lua_State *L)
{
    char buf[16];
    if (hl_cap_time_date(buf, sizeof(buf)) != 0)
        return luaL_error(L, "time.date() failed");
    lua_pushstring(L, buf);
    return 1;
}

static int lua_time_datetime(lua_State *L)
{
    char buf[32];
    if (hl_cap_time_datetime(buf, sizeof(buf)) != 0)
        return luaL_error(L, "time.datetime() failed");
    lua_pushstring(L, buf);
    return 1;
}

static const luaL_Reg time_funcs[] = {
    {"now",      lua_time_now},
    {"now_ms",   lua_time_now_ms},
    {"clock",    lua_time_clock},
    {"date",     lua_time_date},
    {"datetime", lua_time_datetime},
    {NULL, NULL}
};

static int luaopen_hull_time(lua_State *L)
{
    luaL_newlib(L, time_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * hull.env module
 *
 * env.get(name) → string or nil
 * ════════════════════════════════════════════════════════════════════ */

static int lua_env_get(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.env_cfg)
        return luaL_error(L, "env not configured");

    const char *name = luaL_checkstring(L, 1);
    const char *val = hl_cap_env_get(lua->base.env_cfg, name);

    if (val)
        lua_pushstring(L, val);
    else
        lua_pushnil(L);
    return 1;
}

static const luaL_Reg env_funcs[] = {
    {"get", lua_env_get},
    {NULL, NULL}
};

static int luaopen_hull_env(lua_State *L)
{
    luaL_newlib(L, env_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * hull.crypto module
 *
 * crypto.sha256(data)                → hex string
 * crypto.random(n)                   → string of n random bytes
 * crypto.hash_password(password)     → hash string
 * crypto.verify_password(pw, hash)   → boolean
 * crypto.ed25519_keypair()           → pubkey_hex, secret_key_hex
 * crypto.ed25519_sign(data, sk_hex)  → signature_hex
 * crypto.ed25519_verify(data, sig_hex, pk_hex) → boolean
 * ════════════════════════════════════════════════════════════════════ */

static int lua_crypto_sha256(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);

    uint8_t hash[32];
    if (hl_cap_crypto_sha256(data, len, hash) != 0)
        return luaL_error(L, "sha256 failed");

    /* Convert to hex string */
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = '\0';

    lua_pushstring(L, hex);
    return 1;
}

static int lua_crypto_random(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n <= 0 || n > HL_RANDOM_MAX_BYTES)
        return luaL_error(L, "random bytes must be 1-%d", HL_RANDOM_MAX_BYTES);

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *buf = sh_arena_alloc(lua->scratch, (size_t)n);
    if (!buf)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_random(buf, (size_t)n) != 0)
        return luaL_error(L, "random failed");

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

/* crypto.hash_password(password) → "pbkdf2:iterations:salt_hex:hash_hex" */
static int lua_crypto_hash_password(lua_State *L)
{
    size_t pw_len;
    const char *pw = luaL_checklstring(L, 1, &pw_len);

    /* Generate 16-byte salt */
    uint8_t salt[16];
    if (hl_cap_crypto_random(salt, sizeof(salt)) != 0)
        return luaL_error(L, "random failed");

    /* PBKDF2-HMAC-SHA256, 32-byte output */
    uint8_t hash[32];
    int iterations = HL_PBKDF2_ITERATIONS;
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                                iterations, hash, sizeof(hash)) != 0)
        return luaL_error(L, "pbkdf2 failed");

    /* Format: "pbkdf2:100000:salt_hex:hash_hex" */
    char salt_hex[33], hash_hex[65];
    for (int i = 0; i < 16; i++)
        snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);

    char result[128];
    snprintf(result, sizeof(result), "pbkdf2:%d:%s:%s",
             iterations, salt_hex, hash_hex);

    lua_pushstring(L, result);
    return 1;
}

/* ── Hex nibble helper (no sscanf — Cosmopolitan compat) ──────────── */

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* crypto.verify_password(password, hash_string) → boolean */
static int lua_crypto_verify_password(lua_State *L)
{
    size_t pw_len;
    const char *pw = luaL_checklstring(L, 1, &pw_len);
    const char *stored = luaL_checkstring(L, 2);

    /* Parse "pbkdf2:iterations:salt_hex:hash_hex" manually (no scansets
     * — Cosmopolitan libc doesn't support sscanf %[...] scansets). */
    if (strncmp(stored, "pbkdf2:", 7) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const char *p = stored + 7;

    /* Parse iterations */
    char *end = NULL;
    long iterations = strtol(p, &end, 10);
    if (!end || *end != ':' || iterations <= 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    p = end + 1;

    /* Read 32-char salt hex */
    if (strlen(p) < 32 + 1 + 64 || p[32] != ':') {
        lua_pushboolean(L, 0);
        return 1;
    }
    char salt_hex[33];
    memcpy(salt_hex, p, 32);
    salt_hex[32] = '\0';
    p += 33;

    /* Read 64-char hash hex */
    if (strlen(p) < 64) {
        lua_pushboolean(L, 0);
        return 1;
    }
    char hash_hex[65];
    memcpy(hash_hex, p, 64);
    hash_hex[64] = '\0';

    /* Decode hex salt (manual — sscanf %x broken on Cosmopolitan) */
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble((unsigned char)salt_hex[i * 2]);
        int lo = hex_nibble((unsigned char)salt_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { lua_pushboolean(L, 0); return 1; }
        salt[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                                (int)iterations, computed, sizeof(computed)) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Decode stored hash and compare (constant-time) */
    uint8_t stored_hash[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble((unsigned char)hash_hex[i * 2]);
        int lo = hex_nibble((unsigned char)hash_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { lua_pushboolean(L, 0); return 1; }
        stored_hash[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Constant-time comparison */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed[i] ^ stored_hash[i];

    lua_pushboolean(L, diff == 0);
    return 1;
}

/* ── Hex decode helper (no sscanf — Cosmopolitan compat) ──────────── */

static int hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2)
        return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── Ed25519 bindings ──────────────────────────────────────────────── */

/* crypto.ed25519_keypair() → pubkey_hex, secret_key_hex */
static int lua_crypto_ed25519_keypair(lua_State *L)
{
    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0)
        return luaL_error(L, "ed25519 keypair generation failed");

    char pk_hex[65], sk_hex[129];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 64; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[128] = '\0';

    lua_pushstring(L, pk_hex);
    lua_pushstring(L, sk_hex);
    return 2;
}

/* crypto.ed25519_sign(data, secret_key_hex) → signature_hex */
static int lua_crypto_ed25519_sign(lua_State *L)
{
    size_t data_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    size_t sk_hex_len;
    const char *sk_hex = luaL_checklstring(L, 2, &sk_hex_len);

    if (sk_hex_len != 128)
        return luaL_error(L, "secret key must be 128 hex chars (64 bytes)");

    uint8_t sk[64];
    if (hex_decode(sk_hex, sk_hex_len, sk, 64) != 0)
        return luaL_error(L, "invalid hex in secret key");

    uint8_t sig[64];
    if (hl_cap_crypto_ed25519_sign((const uint8_t *)data, data_len, sk, sig) != 0)
        return luaL_error(L, "ed25519 sign failed");

    char sig_hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(sig_hex + i * 2, 3, "%02x", sig[i]);
    sig_hex[128] = '\0';

    lua_pushstring(L, sig_hex);
    return 1;
}

/* crypto.ed25519_verify(data, signature_hex, pubkey_hex) → boolean */
static int lua_crypto_ed25519_verify(lua_State *L)
{
    size_t data_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    size_t sig_hex_len;
    const char *sig_hex = luaL_checklstring(L, 2, &sig_hex_len);
    size_t pk_hex_len;
    const char *pk_hex = luaL_checklstring(L, 3, &pk_hex_len);

    if (sig_hex_len != 128)
        return luaL_error(L, "signature must be 128 hex chars (64 bytes)");
    if (pk_hex_len != 64)
        return luaL_error(L, "public key must be 64 hex chars (32 bytes)");

    uint8_t sig[64], pk[32];
    if (hex_decode(sig_hex, sig_hex_len, sig, 64) != 0)
        return luaL_error(L, "invalid hex in signature");
    if (hex_decode(pk_hex, pk_hex_len, pk, 32) != 0)
        return luaL_error(L, "invalid hex in public key");

    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)data, data_len, sig, pk);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── SHA-512 ───────────────────────────────────────────────────────── */

/* crypto.sha512(data) → hex string (128 chars) */
static int lua_crypto_sha512(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);

    uint8_t hash[64];
    if (hl_cap_crypto_sha512(data, len, hash) != 0)
        return luaL_error(L, "sha512 failed");

    char hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[128] = '\0';

    lua_pushstring(L, hex);
    return 1;
}

/* ── HMAC-SHA512/256 authentication ────────────────────────────────── */

/* crypto.auth(msg, key_hex) → tag_hex (64 chars) */
static int lua_crypto_auth(lua_State *L)
{
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 1, &msg_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 2, &key_hex_len);

    if (key_hex_len != 64)
        return luaL_error(L, "auth key must be 64 hex chars (32 bytes)");

    uint8_t key[32];
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in auth key");

    uint8_t tag[32];
    if (hl_cap_crypto_auth(msg, msg_len, key, tag) != 0)
        return luaL_error(L, "auth failed");

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", tag[i]);
    hex[64] = '\0';

    lua_pushstring(L, hex);
    return 1;
}

/* crypto.auth_verify(tag_hex, msg, key_hex) → boolean */
static int lua_crypto_auth_verify(lua_State *L)
{
    size_t tag_hex_len;
    const char *tag_hex = luaL_checklstring(L, 1, &tag_hex_len);
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 2, &msg_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 3, &key_hex_len);

    if (tag_hex_len != 64)
        return luaL_error(L, "tag must be 64 hex chars (32 bytes)");
    if (key_hex_len != 64)
        return luaL_error(L, "auth key must be 64 hex chars (32 bytes)");

    uint8_t tag[32], key[32];
    if (hex_decode(tag_hex, tag_hex_len, tag, 32) != 0)
        return luaL_error(L, "invalid hex in tag");
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in key");

    int rc = hl_cap_crypto_auth_verify(tag, msg, msg_len, key);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── Secret-key authenticated encryption (XSalsa20+Poly1305) ──────── */

/* crypto.secretbox(msg, nonce_hex, key_hex) → ciphertext_hex */
static int lua_crypto_secretbox(lua_State *L)
{
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 1, &msg_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 3, &key_hex_len);

    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (key_hex_len != 64)
        return luaL_error(L, "key must be 64 hex chars (32 bytes)");

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in key");

    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_secretbox(ct, msg, msg_len, nonce, key) != 0)
        return luaL_error(L, "secretbox failed");

    /* Convert to hex */
    size_t hex_len = ct_len * 2 + 1;
    char *hex = sh_arena_alloc(lua->scratch, hex_len);
    if (!hex)
        return luaL_error(L, "out of memory");

    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);

    lua_pushstring(L, hex);
    return 1;
}

/* crypto.secretbox_open(ct_hex, nonce_hex, key_hex) → string or nil */
static int lua_crypto_secretbox_open(lua_State *L)
{
    size_t ct_hex_len;
    const char *ct_hex = luaL_checklstring(L, 1, &ct_hex_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 3, &key_hex_len);

    if (ct_hex_len % 2 != 0)
        return luaL_error(L, "ciphertext hex must have even length");
    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (key_hex_len != 64)
        return luaL_error(L, "key must be 64 hex chars (32 bytes)");

    size_t ct_len = ct_hex_len / 2;
    if (ct_len < HL_SECRETBOX_MACBYTES) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in key");

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct)
        return luaL_error(L, "out of memory");
    if (hex_decode(ct_hex, ct_hex_len, ct, ct_len) != 0)
        return luaL_error(L, "invalid hex in ciphertext");

    size_t msg_len = ct_len - HL_SECRETBOX_MACBYTES;
    uint8_t *msg = sh_arena_alloc(lua->scratch, msg_len + 1);
    if (!msg)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_secretbox_open(msg, ct, ct_len, nonce, key) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *)msg, msg_len);
    return 1;
}

/* ── Public-key authenticated encryption (Curve25519+XSalsa20+Poly1305) */

/* crypto.box(msg, nonce_hex, pk_hex, sk_hex) → ciphertext_hex */
static int lua_crypto_box(lua_State *L)
{
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 1, &msg_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t pk_hex_len;
    const char *pk_hex = luaL_checklstring(L, 3, &pk_hex_len);
    size_t sk_hex_len;
    const char *sk_hex = luaL_checklstring(L, 4, &sk_hex_len);

    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (pk_hex_len != 64)
        return luaL_error(L, "public key must be 64 hex chars (32 bytes)");
    if (sk_hex_len != 64)
        return luaL_error(L, "secret key must be 64 hex chars (32 bytes)");

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(pk_hex, pk_hex_len, pk, 32) != 0)
        return luaL_error(L, "invalid hex in public key");
    if (hex_decode(sk_hex, sk_hex_len, sk, 32) != 0)
        return luaL_error(L, "invalid hex in secret key");

    size_t ct_len = msg_len + HL_BOX_MACBYTES;
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_box(ct, msg, msg_len, nonce, pk, sk) != 0)
        return luaL_error(L, "box failed");

    size_t hex_len = ct_len * 2 + 1;
    char *hex = sh_arena_alloc(lua->scratch, hex_len);
    if (!hex)
        return luaL_error(L, "out of memory");

    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);

    lua_pushstring(L, hex);
    return 1;
}

/* crypto.box_open(ct_hex, nonce_hex, pk_hex, sk_hex) → string or nil */
static int lua_crypto_box_open(lua_State *L)
{
    size_t ct_hex_len;
    const char *ct_hex = luaL_checklstring(L, 1, &ct_hex_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t pk_hex_len;
    const char *pk_hex = luaL_checklstring(L, 3, &pk_hex_len);
    size_t sk_hex_len;
    const char *sk_hex = luaL_checklstring(L, 4, &sk_hex_len);

    if (ct_hex_len % 2 != 0)
        return luaL_error(L, "ciphertext hex must have even length");
    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (pk_hex_len != 64)
        return luaL_error(L, "public key must be 64 hex chars (32 bytes)");
    if (sk_hex_len != 64)
        return luaL_error(L, "secret key must be 64 hex chars (32 bytes)");

    size_t ct_len = ct_hex_len / 2;
    if (ct_len < HL_BOX_MACBYTES) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(pk_hex, pk_hex_len, pk, 32) != 0)
        return luaL_error(L, "invalid hex in public key");
    if (hex_decode(sk_hex, sk_hex_len, sk, 32) != 0)
        return luaL_error(L, "invalid hex in secret key");

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct)
        return luaL_error(L, "out of memory");
    if (hex_decode(ct_hex, ct_hex_len, ct, ct_len) != 0)
        return luaL_error(L, "invalid hex in ciphertext");

    size_t msg_len = ct_len - HL_BOX_MACBYTES;
    uint8_t *msg = sh_arena_alloc(lua->scratch, msg_len + 1);
    if (!msg)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_box_open(msg, ct, ct_len, nonce, pk, sk) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *)msg, msg_len);
    return 1;
}

/* crypto.box_keypair() → pk_hex, sk_hex */
static int lua_crypto_box_keypair(lua_State *L)
{
    uint8_t pk[32], sk[32];
    if (hl_cap_crypto_box_keypair(pk, sk) != 0)
        return luaL_error(L, "box keypair generation failed");

    char pk_hex[65], sk_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 32; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[64] = '\0';

    lua_pushstring(L, pk_hex);
    lua_pushstring(L, sk_hex);
    return 2;
}

static const luaL_Reg crypto_funcs[] = {
    {"sha256",            lua_crypto_sha256},
    {"sha512",            lua_crypto_sha512},
    {"random",            lua_crypto_random},
    {"hash_password",     lua_crypto_hash_password},
    {"verify_password",   lua_crypto_verify_password},
    {"ed25519_keypair",   lua_crypto_ed25519_keypair},
    {"ed25519_sign",      lua_crypto_ed25519_sign},
    {"ed25519_verify",    lua_crypto_ed25519_verify},
    {"auth",              lua_crypto_auth},
    {"auth_verify",       lua_crypto_auth_verify},
    {"secretbox",         lua_crypto_secretbox},
    {"secretbox_open",    lua_crypto_secretbox_open},
    {"box",               lua_crypto_box},
    {"box_open",          lua_crypto_box_open},
    {"box_keypair",       lua_crypto_box_keypair},
    {NULL, NULL}
};

static int luaopen_hull_crypto(lua_State *L)
{
    luaL_newlib(L, crypto_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * hull.http module
 *
 * http.request(method, url, opts?) → { status, body, headers }
 * http.get(url, opts?)             → { status, body, headers }
 * http.post(url, body, opts?)      → { status, body, headers }
 * http.put(url, body, opts?)       → { status, body, headers }
 * http.patch(url, body, opts?)     → { status, body, headers }
 * http.delete(url, opts?)          → { status, body, headers }
 * ════════════════════════════════════════════════════════════════════ */

/* Parse optional headers table at stack index `idx` into HlHttpHeader array.
 * Returns 0 on success. Caller must free the returned array. */
static int lua_parse_http_headers(lua_State *L, int idx,
                                     HlHttpHeader **out_headers, int *out_count,
                                     SHArena *scratch)
{
    *out_headers = NULL;
    *out_count = 0;

    if (lua_isnoneornil(L, idx))
        return 0;

    if (!lua_istable(L, idx))
        return -1;

    /* Count entries */
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        count++;
        lua_pop(L, 1); /* pop value, keep key */
    }
    if (count == 0)
        return 0;

    HlHttpHeader *hdrs = sh_arena_calloc(scratch, (size_t)count, sizeof(HlHttpHeader));
    if (!hdrs)
        return -1;

    int i = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
            hdrs[i].name = lua_tostring(L, -2);
            hdrs[i].value = lua_tostring(L, -1);
            i++;
        }
        lua_pop(L, 1); /* pop value, keep key */
    }

    *out_headers = hdrs;
    *out_count = i;
    return 0;
}

/* Push HTTP response as Lua table: { status, body, headers } */
static void lua_push_http_response(lua_State *L, HlHttpResponse *resp)
{
    lua_newtable(L);

    lua_pushinteger(L, resp->status);
    lua_setfield(L, -2, "status");

    if (resp->body && resp->body_len > 0)
        lua_pushlstring(L, resp->body, resp->body_len);
    else
        lua_pushstring(L, "");
    lua_setfield(L, -2, "body");

    /* Headers as { ["name"] = "value" } table */
    lua_newtable(L);
    for (int i = 0; i < resp->num_headers; i++) {
        lua_pushstring(L, resp->headers[i].value);
        /* Lowercase the header name for consistent access */
        size_t nlen = strlen(resp->headers[i].name);
        char *lower = sh_arena_alloc(
            ((HlLua *)get_hl_lua(L))->scratch, nlen + 1);
        if (lower) {
            for (size_t j = 0; j < nlen; j++)
                lower[j] = (char)((resp->headers[i].name[j] >= 'A' &&
                                    resp->headers[i].name[j] <= 'Z')
                    ? resp->headers[i].name[j] + 32
                    : resp->headers[i].name[j]);
            lower[nlen] = '\0';
            lua_setfield(L, -2, lower);
        } else {
            lua_setfield(L, -2, resp->headers[i].name);
        }
    }
    lua_setfield(L, -2, "headers");
}

/* http.request(method, url, opts?) */
static int lua_http_request(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *method = luaL_checkstring(L, 1);
    const char *url = luaL_checkstring(L, 2);

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse optional opts table at position 3 */
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "body");
        if (lua_isstring(L, -1))
            body = lua_tolstring(L, -1, &body_len);
        lua_pop(L, 1);

        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            if (lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                        lua->scratch) != 0) {
                lua_pop(L, 1);
                return luaL_error(L, "invalid headers table");
            }
        }
        lua_pop(L, 1);
    }

    HlHttpResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, method, url,
                                    headers, num_headers, body, body_len, &resp);
    if (rc != 0)
        return luaL_error(L, "http request failed");

    lua_push_http_response(L, &resp);
    hl_cap_http_free(&resp);
    return 1;
}

/* http.get(url, opts?) */
static int lua_http_get(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                    lua->scratch);
        }
        lua_pop(L, 1);
    }

    HlHttpResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, "GET", url,
                                    headers, num_headers, NULL, 0, &resp);
    if (rc != 0)
        return luaL_error(L, "http.get failed");

    lua_push_http_response(L, &resp);
    hl_cap_http_free(&resp);
    return 1;
}

/* Helper for POST/PUT/PATCH: (url, body, opts?) */
static int lua_http_body_method(lua_State *L, const char *method)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    size_t body_len = 0;
    const char *body = NULL;
    if (lua_isstring(L, 2))
        body = lua_tolstring(L, 2, &body_len);

    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                    lua->scratch);
        }
        lua_pop(L, 1);
    }

    HlHttpResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, method, url,
                                    headers, num_headers, body, body_len, &resp);
    if (rc != 0)
        return luaL_error(L, "http.%s failed", method);

    lua_push_http_response(L, &resp);
    hl_cap_http_free(&resp);
    return 1;
}

static int lua_http_post(lua_State *L)   { return lua_http_body_method(L, "POST"); }
static int lua_http_put(lua_State *L)    { return lua_http_body_method(L, "PUT"); }
static int lua_http_patch(lua_State *L)  { return lua_http_body_method(L, "PATCH"); }

/* http.delete(url, opts?) — same signature as http.get */
static int lua_http_delete(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                    lua->scratch);
        }
        lua_pop(L, 1);
    }

    HlHttpResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, "DELETE", url,
                                    headers, num_headers, NULL, 0, &resp);
    if (rc != 0)
        return luaL_error(L, "http.delete failed");

    lua_push_http_response(L, &resp);
    hl_cap_http_free(&resp);
    return 1;
}

static const luaL_Reg http_funcs[] = {
    {"request", lua_http_request},
    {"get",     lua_http_get},
    {"post",    lua_http_post},
    {"put",     lua_http_put},
    {"patch",   lua_http_patch},
    {"delete",  lua_http_delete},
    {NULL, NULL}
};

static int luaopen_hull_http(lua_State *L)
{
    luaL_newlib(L, http_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * hull.log module
 *
 * log.info(msg)
 * log.warn(msg)
 * log.error(msg)
 * log.debug(msg)
 * ════════════════════════════════════════════════════════════════════ */

static int lua_log_level(lua_State *L, int level)
{
    /* Extract Lua caller's source location */
    lua_Debug ar;
    const char *src = "lua";
    int line = 0;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
        src = ar.short_src;
        line = ar.currentline;
    }

    /* Detect stdlib vs app: embedded modules have "hull." or "vendor." source */
    const char *tag = "[app]";
    if (strncmp(src, "hull.", 5) == 0 || strncmp(src, "vendor.", 7) == 0)
        tag = "[hull:lua]";

    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        const char *s = luaL_tolstring(L, i, NULL);
        if (s)
            log_log(level, src, line, "%s %s", tag, s);
        lua_pop(L, 1);
    }
    return 0;
}

static int lua_log_info(lua_State *L)  { return lua_log_level(L, LOG_INFO); }
static int lua_log_warn(lua_State *L)  { return lua_log_level(L, LOG_WARN); }
static int lua_log_error(lua_State *L) { return lua_log_level(L, LOG_ERROR); }
static int lua_log_debug(lua_State *L) { return lua_log_level(L, LOG_DEBUG); }

static const luaL_Reg log_funcs[] = {
    {"info",  lua_log_info},
    {"warn",  lua_log_warn},
    {"error", lua_log_error},
    {"debug", lua_log_debug},
    {NULL, NULL}
};

static int luaopen_hull_log(lua_State *L)
{
    luaL_newlib(L, log_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * Custom require() — module loader with embedded + filesystem fallback
 *
 * Replaces Lua's package.require with a minimal custom version.
 * Search order:
 *   1. Cache (registry "__hull_loaded")
 *   2. Embedded modules (registry "__hull_modules")
 *   3. Filesystem (dev mode — relative requires from app_dir)
 *   4. Error
 *
 * Module namespaces:
 *   hull.*   — Hull stdlib wrappers (e.g. require('hull.json'))
 *   vendor.* — Vendored third-party libs (e.g. require('vendor.json'))
 *   ./path   — Relative to requiring module (filesystem or embedded app)
 *   ../path  — Relative to requiring module (parent traversal)
 * ════════════════════════════════════════════════════════════════════ */

/* ── Path normalization helper ────────────────────────────────────── */

/*
 * Normalize a path in-place by collapsing `.` and `..` segments.
 * Input:  "routes/../utils/./helper"
 * Output: "utils/helper"
 * Returns 0 on success, -1 if `..` escapes past root.
 */
static int normalize_path(char *path)
{
    /* Split into segments, process left-to-right */
    char *segments[128];
    int depth = 0;
    int absolute = (path[0] == '/');

    char *p = path;
    while (*p) {
        /* Skip slashes */
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        /* Find end of segment */
        char *seg = p;
        while (*p && *p != '/')
            p++;
        if (*p == '/') {
            *p = '\0';
            p++;
        }

        if (strcmp(seg, ".") == 0) {
            continue; /* skip */
        } else if (strcmp(seg, "..") == 0) {
            if (depth > 0)
                depth--;
            else
                return -1; /* escapes past root */
        } else {
            if (depth >= 128)
                return -1;
            segments[depth++] = seg;
        }
    }

    /* Rebuild path */
    char *out = path;
    if (absolute)
        *out++ = '/';
    for (int i = 0; i < depth; i++) {
        if (i > 0)
            *out++ = '/';
        size_t len = strlen(segments[i]);
        memmove(out, segments[i], len);
        out += len;
    }
    *out = '\0';

    return 0;
}

/* ── Resolve relative module path ─────────────────────────────────── */

/*
 * Resolve a relative require path (starting with ./ or ../) against
 * the caller's module path and app_dir.
 *
 * Returns 0 on success with `out` filled with the filesystem path.
 * Returns -1 on error (path too long, escapes app_dir, etc.).
 */
static int resolve_module_path(lua_State *L, const char *name,
                               const char *app_dir,
                               char *out, size_t out_size)
{
    /* Get the caller's module path from registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
    const char *caller = lua_tostring(L, -1);

    char caller_dir[HL_MODULE_PATH_MAX];
    if (caller) {
        /* Extract directory from caller path */
        const char *last_slash = strrchr(caller, '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - caller);
            if (dir_len >= sizeof(caller_dir)) {
                lua_pop(L, 1);
                return -1;
            }
            memcpy(caller_dir, caller, dir_len);
            caller_dir[dir_len] = '\0';
        } else {
            /* No slash — caller is in the root */
            caller_dir[0] = '.';
            caller_dir[1] = '\0';
        }
    } else {
        /* No caller context — use app_dir as base */
        if (strlen(app_dir) >= sizeof(caller_dir)) {
            lua_pop(L, 1);
            return -1;
        }
        strncpy(caller_dir, app_dir, sizeof(caller_dir) - 1);
        caller_dir[sizeof(caller_dir) - 1] = '\0';
    }
    lua_pop(L, 1); /* pop __hull_current_module */

    /* Build joined path: caller_dir / name [.lua] */
    const char *ext = "";
    size_t name_len = strlen(name);
    if (name_len < 4 || strcmp(name + name_len - 4, ".lua") != 0)
        ext = ".lua";

    char joined[HL_MODULE_PATH_MAX];
    int n = snprintf(joined, sizeof(joined), "%s/%s%s",
                     caller_dir, name, ext);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return -1;

    /* Normalize (collapse . and .. segments) */
    if (normalize_path(joined) != 0)
        return -1;

    /* Security: verify the resolved path starts with app_dir.
     * Build canonical: app_dir prefix must match. */
    size_t app_dir_len = strlen(app_dir);
    /* Strip trailing slash from app_dir for comparison */
    while (app_dir_len > 0 && app_dir[app_dir_len - 1] == '/')
        app_dir_len--;

    /* For "." app_dir, any path without leading .. is valid
     * (normalize_path already rejects escaping past root) */
    if (!(app_dir_len == 1 && app_dir[0] == '.')) {
        if (strncmp(joined, app_dir, app_dir_len) != 0 ||
            (joined[app_dir_len] != '/' && joined[app_dir_len] != '\0'))
            return -1; /* escapes above app_dir */
    }

    if (strlen(joined) >= out_size)
        return -1;
    memcpy(out, joined, strlen(joined) + 1);

    return 0;
}

/* ── Execute and cache a loaded module chunk ──────────────────────── */

/*
 * Execute a loaded chunk, save/restore __hull_current_module context,
 * cache the result, and leave the module value on the stack.
 * `module_path` is the canonical path used for context and cache key.
 * Returns 1 (number of Lua return values) on success.
 * On error, calls lua_error (does not return).
 */
static int execute_and_cache_module(lua_State *L, const char *module_path)
{
    /* Save current module context */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
    /* Stack: ... chunk, saved_module */

    /* Set new module context */
    lua_pushstring(L, module_path);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_current_module");

    /* Execute chunk (it's below saved_module on the stack) */
    lua_pushvalue(L, -2); /* copy chunk to top */
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        /* Restore context before propagating error */
        lua_pushvalue(L, -2); /* push saved_module (now at -3) */
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
        lua_remove(L, -2); /* remove saved_module */
        lua_remove(L, -2); /* remove original chunk */
        return lua_error(L);
    }
    /* Stack: ... chunk, saved_module, result */

    /* Restore previous module context */
    lua_pushvalue(L, -2); /* push saved_module */
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
    lua_remove(L, -2); /* remove saved_module */
    /* Stack: ... chunk, result */

    /* If chunk returned nil, store true as sentinel */
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }

    /* Cache the result in __hull_loaded */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
    lua_pushvalue(L, -2);  /* push module result */
    lua_setfield(L, -2, module_path);
    lua_pop(L, 1); /* pop __hull_loaded */

    /* Remove original chunk, leaving just the result */
    lua_remove(L, -2);
    return 1;
}

/* ── Main require() implementation ────────────────────────────────── */

static int hl_lua_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    /* 1. Check cache (registry "__hull_loaded") */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove __hull_loaded table */
        return 1;          /* return cached module */
    }
    lua_pop(L, 2); /* pop nil + __hull_loaded */

    /* 2. Look up in embedded modules table (registry "__hull_modules") */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_modules");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove __hull_modules table */
        return execute_and_cache_module(L, name);
    }
    lua_pop(L, 2); /* pop nil + __hull_modules */

    /* 3. Filesystem fallback (dev mode — relative requires) */
    HlLua *lua = get_hl_lua(L);
    if (lua && lua->app_dir &&
        (name[0] == '.' || strchr(name, '/') != NULL)) {

        char path[HL_MODULE_PATH_MAX];
        if (resolve_module_path(L, name, lua->app_dir,
                                path, sizeof(path)) == 0) {

            /* Check cache by resolved canonical path */
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
            lua_getfield(L, -1, path);
            if (!lua_isnil(L, -1)) {
                lua_remove(L, -2); /* remove __hull_loaded */
                return 1;
            }
            lua_pop(L, 2); /* pop nil + __hull_loaded */

            /* Read file from disk */
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                if (size < 0 || size > HL_MODULE_MAX_SIZE) {
                    fclose(f);
                    return luaL_error(L, "module too large: %s", path);
                }
                if (fseek(f, 0, SEEK_SET) != 0) {
                    fclose(f);
                    return luaL_error(L, "seek failed: %s", path);
                }

                /* Save arena position — buffer is only needed until
                 * luaL_loadbuffer copies it into Lua bytecode. */
                size_t arena_saved = lua->scratch->used;

                char *buf = sh_arena_alloc(lua->scratch, (size_t)size);
                if (!buf) {
                    fclose(f);
                    return luaL_error(L, "out of memory loading: %s", path);
                }

                size_t nread = fread(buf, 1, (size_t)size, f);
                int read_err = ferror(f);
                fclose(f);

                if (read_err || nread != (size_t)size) {
                    lua->scratch->used = arena_saved;
                    return luaL_error(L, "read error: %s", path);
                }

                /* Compile the chunk — copies data into Lua bytecode */
                int load_ok = luaL_loadbuffer(L, buf, nread, path) == LUA_OK;

                /* Reclaim file buffer — Lua owns the bytecode now */
                lua->scratch->used = arena_saved;

                if (!load_ok)
                    return lua_error(L); /* propagate compile error */

                return execute_and_cache_module(L, path);
            }
        }
    }

    return luaL_error(L, "module not found: %s", name);
}

int hl_lua_register_stdlib(HlLua *lua)
{
    if (!lua || !lua->L)
        return -1;

    lua_State *L = lua->L;

    /* Create __hull_loaded cache table */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_loaded");

    /* Create __hull_modules table and populate with compiled chunks.
     * Iterates the auto-generated hl_stdlib_lua_entries[] table —
     * adding a new .lua file to stdlib/ requires no C code changes. */
    lua_newtable(L);

    for (const HlStdlibEntry *e = hl_stdlib_lua_entries; e->name; e++) {
        if (luaL_loadbuffer(L, (const char *)e->data, e->len, e->name) != LUA_OK) {
            log_error("[hull:c] failed to load stdlib module '%s': %s",
                      e->name, lua_tostring(L, -1));
            lua_pop(L, 2); /* pop error + modules table */
            return -1;
        }
        lua_setfield(L, -2, e->name);
    }

    /* Load embedded app modules (if any — sentinel: first entry has name==NULL) */
    for (const HlStdlibEntry *e = hl_app_lua_entries; e->name; e++) {
        if (luaL_loadbuffer(L, (const char *)e->data, e->len, e->name) != LUA_OK) {
            log_error("[hull:c] failed to load app module '%s': %s",
                      e->name, lua_tostring(L, -1));
            lua_pop(L, 2); /* pop error + modules table */
            return -1;
        }
        lua_setfield(L, -2, e->name);
    }

    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_modules");

    /* Register require as a global function */
    lua_pushcfunction(L, hl_lua_require);
    lua_setglobal(L, "require");

    /* Pre-load json as a global: call require('hull.json') internally */
    lua_getglobal(L, "require");
    lua_pushstring(L, "hull.json");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        log_error("[hull:c] failed to pre-load json: %s",
                  lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    lua_setglobal(L, "json");

    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Module registry — called by hl_lua_init() to register all
 * hull.* built-in modules.
 * ════════════════════════════════════════════════════════════════════ */

int hl_lua_register_modules(HlLua *lua)
{
    if (!lua || !lua->L)
        return -1;

    lua_State *L = lua->L;

    /* Register hull.app as a global */
    luaL_requiref(L, "hull.app", luaopen_hull_app, 0);
    lua_setglobal(L, "app");

    /* Register hull.db (only if database is available) */
    if (lua->base.db) {
        luaL_requiref(L, "hull.db", luaopen_hull_db, 0);
        lua_setglobal(L, "db");
    }

    /* Register hull.time */
    luaL_requiref(L, "hull.time", luaopen_hull_time, 0);
    lua_setglobal(L, "time");

    /* Register hull.env */
    luaL_requiref(L, "hull.env", luaopen_hull_env, 0);
    lua_setglobal(L, "env");

    /* Register hull.crypto */
    luaL_requiref(L, "hull.crypto", luaopen_hull_crypto, 0);
    lua_setglobal(L, "crypto");

    /* Register hull.log */
    luaL_requiref(L, "hull.log", luaopen_hull_log, 0);
    lua_setglobal(L, "log");

    /* Register hull.http — always available; per-function checks enforce
     * that http_cfg is set (wired from manifest after load_app). */
    luaL_requiref(L, "hull.http", luaopen_hull_http, 0);
    lua_setglobal(L, "http");

    return 0;
}
