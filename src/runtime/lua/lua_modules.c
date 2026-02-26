/*
 * lua_modules.c — hull.* built-in module implementations for Lua 5.4
 *
 * Each module is registered as a Lua library via luaL_newlib().
 * All capability calls go through hl_cap_* — no direct SQLite,
 * filesystem, or network access from this file.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/lua_runtime.h"
#include "hull/hull_cap.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "handler");
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

static const luaL_Reg app_funcs[] = {
    {"get",    lua_app_get},
    {"post",   lua_app_post},
    {"put",    lua_app_put},
    {"del",    lua_app_del},
    {"patch",  lua_app_patch},
    {"use",    lua_app_use},
    {"config", lua_app_config},
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

    HlValue *params = calloc((size_t)len, sizeof(HlValue));
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
    /* Pop the values we left on the stack in lua_to_hl_values */
    if (count > 0)
        lua_pop(L, count);
    free(params);
}

/* db.query(sql, params?) */
static int lua_db_query(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->db)
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

    int rc = hl_cap_db_query(lua->db, sql, params, nparams,
                                lua_query_row_cb, &qc);

    lua_free_hl_values(L, params, nparams);

    if (rc != 0) {
        lua_pop(L, 1); /* pop result table */
        return luaL_error(L, "query failed: %s", sqlite3_errmsg(lua->db));
    }

    return 1; /* result table already on stack */
}

/* db.exec(sql, params?) */
static int lua_db_exec(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->db)
        return luaL_error(L, "database not available");

    const char *sql = luaL_checkstring(L, 1);

    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    int rc = hl_cap_db_exec(lua->db, sql, params, nparams);

    lua_free_hl_values(L, params, nparams);

    if (rc < 0)
        return luaL_error(L, "exec failed: %s", sqlite3_errmsg(lua->db));

    lua_pushinteger(L, rc);
    return 1;
}

/* db.last_id() */
static int lua_db_last_id(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->db)
        return luaL_error(L, "database not available");

    lua_pushinteger(L, (lua_Integer)hl_cap_db_last_id(lua->db));
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
    if (!lua || !lua->env_cfg)
        return luaL_error(L, "env not configured");

    const char *name = luaL_checkstring(L, 1);
    const char *val = hl_cap_env_get(lua->env_cfg, name);

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
    if (n <= 0 || n > 65536)
        return luaL_error(L, "random bytes must be 1-65536");

    uint8_t *buf = malloc((size_t)n);
    if (!buf)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_random(buf, (size_t)n) != 0) {
        free(buf);
        return luaL_error(L, "random failed");
    }

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    free(buf);
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

    /* PBKDF2-HMAC-SHA256, 100k iterations, 32-byte output */
    uint8_t hash[32];
    int iterations = 100000;
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

/* crypto.verify_password(password, hash_string) → boolean */
static int lua_crypto_verify_password(lua_State *L)
{
    size_t pw_len;
    const char *pw = luaL_checklstring(L, 1, &pw_len);
    const char *stored = luaL_checkstring(L, 2);

    /* Parse "pbkdf2:iterations:salt_hex:hash_hex" */
    int iterations = 0;
    char salt_hex[33] = {0};
    char hash_hex[65] = {0};

    if (sscanf(stored, "pbkdf2:%d:%32[0-9a-f]:%64[0-9a-f]",
               &iterations, salt_hex, hash_hex) != 3) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Decode hex salt */
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) {
        unsigned int byte;
        sscanf(salt_hex + i * 2, "%2x", &byte);
        salt[i] = (uint8_t)byte;
    }

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                                iterations, computed, sizeof(computed)) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Decode stored hash and compare (constant-time) */
    uint8_t stored_hash[32];
    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        sscanf(hash_hex + i * 2, "%2x", &byte);
        stored_hash[i] = (uint8_t)byte;
    }

    /* Constant-time comparison */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed[i] ^ stored_hash[i];

    lua_pushboolean(L, diff == 0);
    return 1;
}

static const luaL_Reg crypto_funcs[] = {
    {"sha256",          lua_crypto_sha256},
    {"random",          lua_crypto_random},
    {"hash_password",   lua_crypto_hash_password},
    {"verify_password", lua_crypto_verify_password},
    {NULL, NULL}
};

static int luaopen_hull_crypto(lua_State *L)
{
    luaL_newlib(L, crypto_funcs);
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

static int lua_log_level(lua_State *L, const char *level)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        const char *s = luaL_tolstring(L, i, NULL);
        if (s)
            fprintf(stderr, "[%s] %s\n", level, s);
        lua_pop(L, 1); /* pop the string from luaL_tolstring */
    }
    return 0;
}

static int lua_log_info(lua_State *L)  { return lua_log_level(L, "INFO"); }
static int lua_log_warn(lua_State *L)  { return lua_log_level(L, "WARN"); }
static int lua_log_error(lua_State *L) { return lua_log_level(L, "ERROR"); }
static int lua_log_debug(lua_State *L) { return lua_log_level(L, "DEBUG"); }

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
    if (lua->db) {
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

    return 0;
}
