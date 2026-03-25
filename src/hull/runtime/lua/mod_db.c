/* mod_db.c — hull.db module: query, exec, batch, async
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/db.h"
#include "hull/async.h"
#include "hull/worker_db.h"

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

int luaopen_hull_db(lua_State *L)
{
    luaL_newlib(L, db_funcs);
    luaL_newlib(L, db_async_funcs);
    lua_setfield(L, -2, "async");
    return 1;
}
