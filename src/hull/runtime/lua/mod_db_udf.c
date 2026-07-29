/* mod_db_udf.c — hull.db UDF bindings (SQLite user-defined functions).
 *
 * The per-runtime SQLite UDF bridge, split out of mod_db.c so the base runtime
 * archive (libhull_feature-lua.a) carries ZERO sqlite3_* references (Phase C.2,
 * docs/sqlite_feature.md). This TU is whole-archived into an app only when the
 * SQLite backend is reachable (libhull_feature-sqlite-lua.a), providing the
 * strong hl_lua_db_attach_udf override that mod_db.c's weak default replaces.
 * UDFs register into the SQLite VM via sqlite3_create_function and marshal
 * sqlite3_value/context, so this is the sole per-runtime sqlite3_* consumer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB
#ifdef HL_ENABLE_SQLITE

#include "mod_buffer.h"            /* get_hl_lua, HlLua */
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"    /* hl_db_sqlite_raw */

#ifdef HL_ENABLE_WASM
#include "hull/cap/db_udf.h"
#include "hull/cap/wasm.h"
#endif

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* Minimal handle resolver: the udf sub-table binds the connection handle as
 * lightuserdata upvalue 1 (see hl_lua_db_attach_udf below), so a udf call
 * resolves it directly. udf is never attached to an owner box (db.open), so the
 * owned-conn path in mod_db.c's db_call_handle is not needed here. */
static HlDbHandle *db_call_handle(lua_State *L)
{
    int uv = lua_upvalueindex(1);
    return lua_islightuserdata(L, uv)
        ? (HlDbHandle *)lua_touserdata(L, uv) : NULL;
}

/* Context for Lua scalar UDF trampoline */
typedef struct {
    lua_State *L;
    int func_ref;    /* LUA_REGISTRYINDEX ref to the Lua function */
    int *alive;      /* points to HlLua.udf_runtime_alive */
} LuaScalarUdfCtx;

/* Context for Lua aggregate UDF trampoline */
typedef struct {
    lua_State *L;
    int step_ref;
    int finalize_ref;
    int *alive;      /* points to HlLua.udf_runtime_alive */
} LuaAggUdfCtx;

/* Per-group state for Lua aggregates (via sqlite3_aggregate_context) */
typedef struct {
    int ctx_table_ref;  /* 0 = not initialized */
} LuaAggGroupState;

/* Push sqlite3_value as a Lua value */
static void lua_push_sqlite_value(lua_State *L, sqlite3_value *val)
{
    switch (sqlite3_value_type(val)) {
    case SQLITE_INTEGER:
        lua_pushinteger(L, (lua_Integer)sqlite3_value_int64(val));
        break;
    case SQLITE_FLOAT:
        lua_pushnumber(L, (lua_Number)sqlite3_value_double(val));
        break;
    case SQLITE_TEXT:
        lua_pushlstring(L, (const char *)sqlite3_value_text(val),
                        (size_t)sqlite3_value_bytes(val));
        break;
    case SQLITE_BLOB:
        lua_pushlstring(L, (const char *)sqlite3_value_blob(val),
                        (size_t)sqlite3_value_bytes(val));
        break;
    default: /* SQLITE_NULL */
        lua_pushnil(L);
        break;
    }
}

/* Convert Lua stack top to sqlite3_result */
static void lua_to_sqlite_result(lua_State *L, sqlite3_context *ctx)
{
    switch (lua_type(L, -1)) {
    case LUA_TNUMBER:
        if (lua_isinteger(L, -1))
            sqlite3_result_int64(ctx, (sqlite3_int64)lua_tointeger(L, -1));
        else
            sqlite3_result_double(ctx, (double)lua_tonumber(L, -1));
        break;
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        sqlite3_result_text(ctx, s, (int)len, SQLITE_TRANSIENT);
        break;
    }
    case LUA_TBOOLEAN:
        sqlite3_result_int(ctx, lua_toboolean(L, -1));
        break;
    case LUA_TNIL:
    default:
        sqlite3_result_null(ctx);
        break;
    }
}

/* Scalar Lua UDF callback */
static void lua_scalar_udf_func(sqlite3_context *ctx, int argc,
                                 sqlite3_value **argv)
{
    LuaScalarUdfCtx *udf = (LuaScalarUdfCtx *)sqlite3_user_data(ctx);
    lua_State *L = udf->L;

    lua_rawgeti(L, LUA_REGISTRYINDEX, udf->func_ref);

    for (int i = 0; i < argc; i++)
        lua_push_sqlite_value(L, argv[i]);

    if (lua_pcall(L, argc, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        sqlite3_result_error(ctx, err ? err : "Lua UDF error", -1);
        lua_pop(L, 1);
        return;
    }

    lua_to_sqlite_result(L, ctx);
    lua_pop(L, 1);
}

/* Destroy callback for scalar Lua UDF */
static void lua_scalar_udf_destroy(void *data)
{
    LuaScalarUdfCtx *udf = (LuaScalarUdfCtx *)data;
    if (!udf) return;
    /* Free the registry ref only if the Lua state is still alive
     * (explicit unregister). During sqlite3_close the state is dead. */
    if (udf->alive && *udf->alive && udf->L)
        luaL_unref(udf->L, LUA_REGISTRYINDEX, udf->func_ref);
    free(udf);
}

/* Aggregate Lua UDF step callback */
static void lua_agg_step_func(sqlite3_context *ctx, int argc,
                               sqlite3_value **argv)
{
    LuaAggUdfCtx *udf = (LuaAggUdfCtx *)sqlite3_user_data(ctx);
    lua_State *L = udf->L;
    LuaAggGroupState *gs = (LuaAggGroupState *)sqlite3_aggregate_context(
        ctx, (int)sizeof(LuaAggGroupState));
    if (!gs) {
        sqlite3_result_error_nomem(ctx);
        return;
    }

    /* Create context table on first call for this group */
    if (gs->ctx_table_ref == 0) {
        lua_newtable(L);
        gs->ctx_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    /* Call step(ctx_table, arg1, arg2, ...) */
    lua_rawgeti(L, LUA_REGISTRYINDEX, udf->step_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, gs->ctx_table_ref);
    for (int i = 0; i < argc; i++)
        lua_push_sqlite_value(L, argv[i]);

    if (lua_pcall(L, argc + 1, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        sqlite3_result_error(ctx, err ? err : "Lua UDF step error", -1);
        lua_pop(L, 1);
    }
}

/* Aggregate Lua UDF finalize callback */
static void lua_agg_finalize_func(sqlite3_context *ctx)
{
    LuaAggUdfCtx *udf = (LuaAggUdfCtx *)sqlite3_user_data(ctx);
    lua_State *L = udf->L;
    LuaAggGroupState *gs = (LuaAggGroupState *)sqlite3_aggregate_context(ctx, 0);

    if (!gs || gs->ctx_table_ref == 0) {
        sqlite3_result_null(ctx);
        return;
    }

    /* Call finalize(ctx_table) -> result */
    lua_rawgeti(L, LUA_REGISTRYINDEX, udf->finalize_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, gs->ctx_table_ref);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        sqlite3_result_error(ctx, err ? err : "Lua UDF finalize error", -1);
        lua_pop(L, 1);
    } else {
        lua_to_sqlite_result(L, ctx);
        lua_pop(L, 1);
    }

    /* Clean up group state */
    luaL_unref(L, LUA_REGISTRYINDEX, gs->ctx_table_ref);
    gs->ctx_table_ref = 0;
}

/* Destroy callback for aggregate Lua UDF */
static void lua_agg_udf_destroy(void *data)
{
    LuaAggUdfCtx *udf = (LuaAggUdfCtx *)data;
    if (!udf) return;
    /* Free registry refs only if the Lua state is still alive */
    if (udf->alive && *udf->alive && udf->L) {
        luaL_unref(udf->L, LUA_REGISTRYINDEX, udf->step_ref);
        luaL_unref(udf->L, LUA_REGISTRYINDEX, udf->finalize_ref);
    }
    free(udf);
}

/* Parse UDF options from Lua table at stack index `opts_idx`.
 * Fills deterministic, nargs, gas. Returns 0. */
static void lua_parse_udf_opts(lua_State *L, int opts_idx,
                                int *deterministic, int *nargs,
                                int *aggregate, int64_t *gas,
                                uint32_t *heap, uint32_t *stack_sz)
{
    *deterministic = 0;
    *nargs = -1; /* default: variadic for Lua UDFs */
    *aggregate = 0;
    *gas = 0;
    *heap = 0;
    *stack_sz = 0;

    if (!lua_istable(L, opts_idx))
        return;

    lua_getfield(L, opts_idx, "deterministic");
    if (lua_isboolean(L, -1))
        *deterministic = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "args");
    if (lua_isinteger(L, -1))
        *nargs = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "aggregate");
    if (lua_isboolean(L, -1))
        *aggregate = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "gas");
    if (lua_isinteger(L, -1))
        *gas = (int64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "heap");
    if (lua_isinteger(L, -1))
        *heap = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "stack");
    if (lua_isinteger(L, -1))
        *stack_sz = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
}

/*
 * db.udf.register(name, impl, opts?)
 *
 * impl types:
 *   function       -> Lua scalar UDF
 *   table{step,finalize} -> Lua aggregate UDF
 *   string         -> WASM UDF (module name)
 */
static int lua_db_udf_register(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    HlDbHandle *conn = db_call_handle(L);
    sqlite3 *raw_db = hl_db_sqlite_raw(conn);
    if (!lua || !raw_db)
        return luaL_error(L, "database not available");

    const char *sql_name = luaL_checkstring(L, 1);

    /* Validate hull_ prefix */
    if (strncmp(sql_name, "hull_", 5) != 0)
        return luaL_error(L, "UDF name must start with 'hull_'");

    int deterministic = 0, nargs = -1, aggregate = 0;
    int64_t gas = 0;
    uint32_t heap = 0, stack_sz = 0;

    if (lua_isstring(L, 2)) {
        /* ── WASM UDF ──────────────────────────────────────────── */
#ifdef HL_ENABLE_WASM
        const char *module_name = lua_tostring(L, 2);

        if (!lua->base.wasm_cache)
            return luaL_error(L, "WASM compute not available");

        lua_parse_udf_opts(L, 3, &deterministic, &nargs, &aggregate,
                           &gas, &heap, &stack_sz);

        HlDbUdfOpts opts = {
            .sql_name      = sql_name,
            .module_name   = module_name,
            .nargs         = nargs != -1 ? nargs : 1,
            .gas_per_call  = gas,
            .deterministic = deterministic,
            .is_aggregate  = aggregate,
            .heap_size     = heap,
            .stack_size    = stack_sz,
        };

        const char *err_msg = NULL;
        int rc = hl_cap_db_udf_register_wasm(
            conn, lua->base.wasm_cache, &opts,
            lua->base.app_vfs, lua->app_dir,
            lua->base.alloc, &err_msg);
        if (rc != 0)
            return luaL_error(L, "db.udf.register: %s",
                              err_msg ? err_msg : "registration failed");
#else
        return luaL_error(L, "WASM UDF support not compiled in");
#endif
    } else if (lua_isfunction(L, 2)) {
        /* ── Lua scalar UDF ────────────────────────────────────── */
        lua_parse_udf_opts(L, 3, &deterministic, &nargs, &aggregate,
                           &gas, &heap, &stack_sz);

        LuaScalarUdfCtx *udf_ctx = calloc(1, sizeof(*udf_ctx));
        if (!udf_ctx)
            return luaL_error(L, "db.udf.register: out of memory");

        udf_ctx->L = L;
        udf_ctx->alive = &lua->udf_runtime_alive;
        lua_pushvalue(L, 2);
        udf_ctx->func_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        int encoding = SQLITE_UTF8;
        if (deterministic) encoding |= SQLITE_DETERMINISTIC;

        int rc = sqlite3_create_function_v2(
            raw_db, sql_name, nargs, encoding, udf_ctx,
            lua_scalar_udf_func, NULL, NULL,
            lua_scalar_udf_destroy);
        if (rc != SQLITE_OK) {
            lua_scalar_udf_destroy(udf_ctx);
            return luaL_error(L, "db.udf.register: %s",
                              sqlite3_errmsg(raw_db));
        }
    } else if (lua_istable(L, 2)) {
        /* ── Lua aggregate UDF (table with step + finalize) ──── */
        lua_getfield(L, 2, "step");
        lua_getfield(L, 2, "finalize");
        int has_step = lua_isfunction(L, -2);
        int has_final = lua_isfunction(L, -1);

        if (!has_step || !has_final) {
            lua_pop(L, 2);
            return luaL_error(L,
                "db.udf.register: aggregate requires step and finalize functions");
        }

        lua_parse_udf_opts(L, 3, &deterministic, &nargs, &aggregate,
                           &gas, &heap, &stack_sz);

        LuaAggUdfCtx *udf_ctx = calloc(1, sizeof(*udf_ctx));
        if (!udf_ctx) {
            lua_pop(L, 2);
            return luaL_error(L, "db.udf.register: out of memory");
        }

        udf_ctx->L = L;
        udf_ctx->alive = &lua->udf_runtime_alive;
        /* finalize is on top of stack, step below it */
        udf_ctx->finalize_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        udf_ctx->step_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        int encoding = SQLITE_UTF8;
        if (deterministic) encoding |= SQLITE_DETERMINISTIC;

        int rc = sqlite3_create_function_v2(
            raw_db, sql_name, nargs, encoding, udf_ctx,
            NULL, lua_agg_step_func, lua_agg_finalize_func,
            lua_agg_udf_destroy);
        if (rc != SQLITE_OK) {
            lua_agg_udf_destroy(udf_ctx);
            return luaL_error(L, "db.udf.register: %s",
                              sqlite3_errmsg(raw_db));
        }
    } else {
        return luaL_error(L,
            "db.udf.register: impl must be a function, table, or string");
    }

    return 0;
}

/* db.udf.unregister(name) */
static int lua_db_udf_unregister(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    HlDbHandle *conn = db_call_handle(L);
    if (!lua || !conn)
        return luaL_error(L, "database not available");

    const char *sql_name = luaL_checkstring(L, 1);

#ifdef HL_ENABLE_WASM
    if (hl_cap_db_udf_unregister(conn, sql_name) != 0)
        return luaL_error(L, "db.udf.unregister: failed");
#else
    /* Fall back to direct sqlite3 call when WASM (and the UDF cap helper)
     * is compiled out — Lua/JS callback UDFs still work without WASM. */
    sqlite3 *raw_db = hl_db_sqlite_raw(conn);
    if (!raw_db)
        return luaL_error(L, "database not available");
    int rc = sqlite3_create_function_v2(
        raw_db, sql_name, -1, SQLITE_UTF8,
        NULL, NULL, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return luaL_error(L, "db.udf.unregister: %s",
                          sqlite3_errmsg(raw_db));
#endif

    return 0;
}

static const luaL_Reg db_udf_funcs[] = {
    {"register",   lua_db_udf_register},
    {"unregister", lua_db_udf_unregister},
    {NULL, NULL}
};


/* Attach a udf sub-table to the connection object at the top of the stack, each
 * function carrying @p h as lightuserdata upvalue 1. Strong override of the
 * weak no-op in mod_db.c: present only when this bridge is composed into the
 * app, so a base runtime with no SQLite backend simply has no db.udf. */
void hl_lua_db_attach_udf(lua_State *L, HlDbHandle *h)
{
    lua_newtable(L);
    for (const luaL_Reg *m = db_udf_funcs; m->name; m++) {
        lua_pushlightuserdata(L, h);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
    lua_setfield(L, -2, "udf");
}

#endif /* HL_ENABLE_SQLITE */
#endif /* HL_ENABLE_DB */
