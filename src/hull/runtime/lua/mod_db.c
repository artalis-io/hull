/* mod_db.c — hull.db module: query, exec, batch, async, udf
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/db.h"
#include "hull/async.h"
#include "hull/worker_db.h"

#ifdef HL_ENABLE_WASM
#include "hull/cap/db_udf.h"
#include "hull/cap/wasm.h"
#endif

#include <keel/server.h>

#include <sh_arena.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

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
    if (!lua_checkstack(qc->L, ncols + 2))
        return -1;
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

/* Check if the immediate Lua caller is a stdlib module (chunk name starts
 * with "hull.").  User modules start with "./" — so a simple prefix check
 * is sufficient.  Returns 1 for stdlib, 0 for user code. */
static int lua_is_stdlib_caller(lua_State *L)
{
    lua_Debug ar;
    /* level 0 = this C function, level 1 = Lua caller */
    if (lua_getstack(L, 1, &ar) == 0)
        return 0;
    if (lua_getinfo(L, "S", &ar) == 0)
        return 0;
    return ar.source && strncmp(ar.source, "hull.", 5) == 0;
}

/* db.query implementation */
static int lua_db_query_impl(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.stmt_cache)
        return luaL_error(L, "database not available");

    const char *sql = luaL_checkstring(L, 1);

    if (!lua_is_stdlib_caller(L) && hl_cap_db_check_namespace(sql) != 0)
        return luaL_error(L, "access denied: _hull_* tables are reserved");

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

    int rc = hl_cap_db_query(lua->base.stmt_cache, sql, params, nparams,
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

/* db.exec implementation */
static int lua_db_exec_impl(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.stmt_cache)
        return luaL_error(L, "database not available");

    const char *sql = luaL_checkstring(L, 1);

    if (!lua_is_stdlib_caller(L) && hl_cap_db_check_namespace(sql) != 0)
        return luaL_error(L, "access denied: _hull_* tables are reserved");

    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    int rc = hl_cap_db_exec(lua->base.stmt_cache, sql, params, nparams);

    lua_free_hl_values(L, params, nparams);

    if (rc < 0)
        return luaL_error(L, "exec failed: %s", sqlite3_errmsg(lua->base.db));

    lua_pushinteger(L, rc);
    return 1;
}

static int lua_db_query(lua_State *L) { return lua_db_query_impl(L); }
static int lua_db_exec(lua_State *L) { return lua_db_exec_impl(L); }

/* db.last_id() */
static int lua_db_last_id(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.db)
        return luaL_error(L, "database not available");

    lua_pushinteger(L, (lua_Integer)hl_cap_db_last_id(lua->base.db));
    return 1;
}

/* db.batch(fn) — execute fn() inside a transaction (BEGIN IMMEDIATE..COMMIT) */
static int lua_db_batch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.db)
        return luaL_error(L, "database not available");

    luaL_checktype(L, 1, LUA_TFUNCTION);

    if (hl_cap_db_begin(lua->base.db) != 0)
        return luaL_error(L, "BEGIN failed: %s", sqlite3_errmsg(lua->base.db));

    lua_pushvalue(L, 1); /* push the function */
    int rc = lua_pcall(L, 0, 0, 0);

    if (rc != LUA_OK) {
        hl_cap_db_rollback(lua->base.db);
        return lua_error(L); /* re-raise the error */
    }

    if (hl_cap_db_commit(lua->base.db) != 0) {
        hl_cap_db_rollback(lua->base.db);
        return luaL_error(L, "COMMIT failed: %s", sqlite3_errmsg(lua->base.db));
    }

    return 0;
}

static const luaL_Reg db_funcs[] = {
    {"query",   lua_db_query},
    {"exec",    lua_db_exec},
    {"last_id", lua_db_last_id},
    {"batch",   lua_db_batch},
    {NULL, NULL}
};

/* ── db.async.query / db.async.exec ─────────────────────────────────── */

/* push_result callback: convert HlWorkerDbOp result to Lua table */
static void lua_push_worker_db_result(lua_State *L, void *driver)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)driver;

    if (op->error) {
        lua_newtable(L);
        lua_pushstring(L, op->error_msg);
        lua_setfield(L, -2, "error");
        return;
    }

    if (op->kind == HL_WORK_DB_EXEC) {
        lua_newtable(L);
        lua_pushinteger(L, op->exec_changes);
        lua_setfield(L, -2, "changes");
        lua_pushinteger(L, (lua_Integer)op->last_id);
        lua_setfield(L, -2, "last_id");
        return;
    }

    /* HL_WORK_DB_QUERY — array of row tables */
    HlDbResult *r = &op->result;
    lua_createtable(L, r->nrows, 0);

    for (int row = 0; row < r->nrows; row++) {
        lua_createtable(L, 0, r->ncols);
        HlDbValue *vals = &r->values[row * r->ncols];
        for (int col = 0; col < r->ncols; col++) {
            switch (vals[col].type) {
            case HL_TYPE_INT:
                lua_pushinteger(L, (lua_Integer)vals[col].i);
                break;
            case HL_TYPE_DOUBLE:
                lua_pushnumber(L, vals[col].d);
                break;
            case HL_TYPE_TEXT:
            case HL_TYPE_BLOB:
                lua_pushlstring(L, vals[col].s, vals[col].len);
                break;
            case HL_TYPE_NIL:
            default:
                lua_pushnil(L);
                break;
            }
            lua_setfield(L, -2, r->col_names[col]);
        }
        lua_rawseti(L, -2, row + 1);
    }
}

/* Common implementation for db.async.query and db.async.exec */
static int lua_db_async_common(lua_State *L, HlWorkerDbKind kind)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "db.async not available (no thread pool)");
    if (!lua->server || !lua->active_conn)
        return luaL_error(L, "db.async can only be called from a request handler");

    const char *sql = luaL_checkstring(L, 1);

    if (!lua_is_stdlib_caller(L) && hl_cap_db_check_namespace(sql) != 0)
        return luaL_error(L, "access denied: _hull_* tables are reserved");

    /* Parse params */
    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    /* Allocate op */
    HlWorkerDbOp *op = calloc(1, sizeof(HlWorkerDbOp));
    if (!op) {
        lua_free_hl_values(L, params, nparams);
        return luaL_error(L, "db.async: out of memory");
    }

    op->kind = kind;
    op->server = lua->server;
    op->alloc = lua->base.alloc;
    op->sql = strdup(sql);
    if (!op->sql) {
        lua_free_hl_values(L, params, nparams);
        free(op);
        return luaL_error(L, "db.async: out of memory");
    }

    /* Deep-copy params (they need to outlive this stack frame) */
    if (nparams > 0) {
        op->params = hl_deep_copy_params(params, nparams);
        op->nparams = nparams;
        if (!op->params) {
            lua_free_hl_values(L, params, nparams);
            free(op->sql);
            free(op);
            return luaL_error(L, "db.async: out of memory");
        }
    }
    lua_free_hl_values(L, params, nparams);

    /* Create async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.alloc);
    if (!ctx) {
        hl_worker_db_op_free(op);
        free(op);
        return luaL_error(L, "db.async: out of memory");
    }

    /* Create Lua continuation */
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_worker_db_result);
    if (!cont) {
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "db.async: out of memory");
    }
    ctx->cont = cont;
    ctx->driver = op;
    ctx->free_driver = hl_worker_db_op_free_all;
    ctx->op.on_cancel = hl_worker_db_async_cancel;

    op->async_ctx = ctx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_worker_db_submit(lua->base.thread_pool, op) != 0) {
        ctx->cont->destroy(ctx->cont);
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "db.async: thread pool full");
    }

    /* Suspend the connection */
    if (kl_async_suspend(lua->server, lua->active_conn, &ctx->op) < 0) {
        op->cancelled = 1;
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
        return luaL_error(L, "db.async: failed to suspend connection");
    }

    return lua_yieldk(L, 0, 0, NULL);
}

static int lua_db_async_query(lua_State *L)
{
    return lua_db_async_common(L, HL_WORK_DB_QUERY);
}

static int lua_db_async_exec(lua_State *L)
{
    return lua_db_async_common(L, HL_WORK_DB_EXEC);
}

static const luaL_Reg db_async_funcs[] = {
    {"query", lua_db_async_query},
    {"exec",  lua_db_async_exec},
    {NULL, NULL}
};

/* ── db.udf — user-defined SQL functions ─────────────────────────────── */

/* Context for Lua scalar UDF trampoline */
typedef struct {
    lua_State *L;
    int func_ref;    /* LUA_REGISTRYINDEX ref to the Lua function */
} LuaScalarUdfCtx;

/* Context for Lua aggregate UDF trampoline */
typedef struct {
    lua_State *L;
    int step_ref;
    int finalize_ref;
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
    if (udf->L && udf->func_ref != 0)
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
    if (udf->L) {
        if (udf->step_ref != 0)
            luaL_unref(udf->L, LUA_REGISTRYINDEX, udf->step_ref);
        if (udf->finalize_ref != 0)
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
    if (!lua || !lua->base.db)
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
            lua->base.db, lua->base.wasm_cache, &opts,
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
        lua_pushvalue(L, 2);
        udf_ctx->func_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        int encoding = SQLITE_UTF8;
        if (deterministic) encoding |= SQLITE_DETERMINISTIC;

        int rc = sqlite3_create_function_v2(
            lua->base.db, sql_name, nargs, encoding, udf_ctx,
            lua_scalar_udf_func, NULL, NULL,
            lua_scalar_udf_destroy);
        if (rc != SQLITE_OK) {
            lua_scalar_udf_destroy(udf_ctx);
            return luaL_error(L, "db.udf.register: %s",
                              sqlite3_errmsg(lua->base.db));
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
        /* finalize is on top of stack, step below it */
        udf_ctx->finalize_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        udf_ctx->step_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        int encoding = SQLITE_UTF8;
        if (deterministic) encoding |= SQLITE_DETERMINISTIC;

        int rc = sqlite3_create_function_v2(
            lua->base.db, sql_name, nargs, encoding, udf_ctx,
            NULL, lua_agg_step_func, lua_agg_finalize_func,
            lua_agg_udf_destroy);
        if (rc != SQLITE_OK) {
            lua_agg_udf_destroy(udf_ctx);
            return luaL_error(L, "db.udf.register: %s",
                              sqlite3_errmsg(lua->base.db));
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
    if (!lua || !lua->base.db)
        return luaL_error(L, "database not available");

    const char *sql_name = luaL_checkstring(L, 1);

    /* SQLite calls the xDestroy callback for the old registration */
    int rc = sqlite3_create_function_v2(
        lua->base.db, sql_name, -1, SQLITE_UTF8,
        NULL, NULL, NULL, NULL, NULL);

    if (rc != SQLITE_OK)
        return luaL_error(L, "db.udf.unregister: %s",
                          sqlite3_errmsg(lua->base.db));

    return 0;
}

static const luaL_Reg db_udf_funcs[] = {
    {"register",   lua_db_udf_register},
    {"unregister", lua_db_udf_unregister},
    {NULL, NULL}
};

/* ── Module opener ───────────────────────────────────────────────────── */

int luaopen_hull_db(lua_State *L)
{
    luaL_newlib(L, db_funcs);

    /* db.async sub-table */
    luaL_newlib(L, db_async_funcs);
    lua_setfield(L, -2, "async");

    /* db.udf sub-table */
    luaL_newlib(L, db_udf_funcs);
    lua_setfield(L, -2, "udf");

    return 1;
}
