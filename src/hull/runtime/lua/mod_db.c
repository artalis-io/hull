/* mod_db.c — hull.db module: query, exec, batch, async, udf
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "mod_buffer.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/db_dynamic.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "hull/worker_db.h"

#include <keel/server.h>

#include <sh_arena.h>
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

/* The default connection, resolved from the registry (there is no separate
 * default-handle field). NULL under --no-db. */
static HlDbHandle *default_db(HlLua *lua)
{
    return lua ? hl_db_registry_default(lua->base.db_registry) : NULL;
}

/* Owner box for a caller-owned dynamic connection (db.open). A registry-backed
 * connection object binds its borrowed handle directly as a lightuserdata
 * upvalue; a db.open() object instead binds this box, so close()/__gc can null
 * the pointer and every subsequent method fails closed. The box outlives the
 * stack via the method closures that hold it as an upvalue. */
#define HL_LUA_DB_OWNED_MT "hull.db.owned"
typedef struct {
    HlDbHandle *h;     /* owned; NULL after close (idempotent) */
    char       *dsn;   /* owned; the validated DSN this opened from, for
                        * conn.async (worker pool is DSN-keyed). Freed in __gc,
                        * not close(), so a post-close async fails on the NULL
                        * handle guard before the dsn is ever read. */
} HlLuaOwnedConn;

/* Release the handle exactly once; NULL-guarded so close() + __gc are safe in
 * either order. Does not touch dsn (see the struct comment). */
static void owned_conn_release(HlLuaOwnedConn *o)
{
    if (o && o->h) {
        hl_db_dynamic_close(o->h);
        o->h = NULL;
    }
}

/* Resolve the connection this call operates on: a borrowed handle bound as a
 * lightuserdata upvalue (db.connect()/db.default()), an owner box bound the same
 * way (db.open(), NULL once closed), else the default connection.
 * Internal-table (_hull_*) access is gated by a caller check at each call
 * site, not by a separate handle. */
static HlDbHandle *db_call_handle(lua_State *L)
{
    int uv = lua_upvalueindex(1);
    if (lua_islightuserdata(L, uv))
        return (HlDbHandle *)lua_touserdata(L, uv);
    HlLuaOwnedConn *o = luaL_testudata(L, uv, HL_LUA_DB_OWNED_MT);
    if (o)
        return o->h;   /* NULL after close() → methods error "not available" */
    HlLua *lua = get_hl_lua(L);
    if (!lua) return NULL;
    return default_db(lua);
}

/* Resolve the DSN this call's connection opened from, symmetric with
 * db_call_handle: an owner box (db.open) carries its own validated DSN; a
 * borrowed lightuserdata handle (db.connect/default) is matched in the
 * registry. Used only by conn.async to tell the worker pool which database to
 * open its per-thread connection against. NULL = unknown → the worker falls
 * back to its default DSN. */
static const char *db_call_dsn(lua_State *L)
{
    int uv = lua_upvalueindex(1);
    HlLua *lua = get_hl_lua(L);
    if (!lua) return NULL;
    if (lua_islightuserdata(L, uv))
        return hl_db_registry_dsn_for(lua->base.db_registry,
                                      (HlDbHandle *)lua_touserdata(L, uv));
    HlLuaOwnedConn *o = luaL_testudata(L, uv, HL_LUA_DB_OWNED_MT);
    if (o)
        return o->dsn;
    return hl_db_registry_dsn_for(lua->base.db_registry, default_db(lua));
}

/* db.query implementation */
static int lua_db_query_impl(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not available");

    const char *sql = luaL_checkstring(L, 1);

    int is_stdlib = lua_is_stdlib_caller(L);
    HlDbHandle *h = db_call_handle(L);

    if (!is_stdlib && hl_cap_db_check_namespace(sql) != 0)
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

    int rc = hl_db_query(h, sql, params, nparams,
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
        return luaL_error(L, "query failed: %s", hl_db_errmsg(h));
    }

    return 1; /* result table already on stack */
}

/* db.exec implementation */
static int lua_db_exec_impl(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not available");

    const char *sql = luaL_checkstring(L, 1);

    int is_stdlib = lua_is_stdlib_caller(L);
    HlDbHandle *h = db_call_handle(L);

    if (!is_stdlib && hl_cap_db_check_namespace(sql) != 0)
        return luaL_error(L, "access denied: _hull_* tables are reserved");

    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    int rc = hl_db_exec(h, sql, params, nparams);

    lua_free_hl_values(L, params, nparams);

    if (rc < 0)
        return luaL_error(L, "exec failed: %s", hl_db_errmsg(h));

    lua_pushinteger(L, rc);
    return 1;
}

static int lua_db_query(lua_State *L) { return lua_db_query_impl(L); }
static int lua_db_exec(lua_State *L) { return lua_db_exec_impl(L); }

/* db.last_id() */
static int lua_db_last_id(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not available");

    lua_pushinteger(L, (lua_Integer)hl_db_last_id(
        db_call_handle(L)));
    return 1;
}

/* db.batch(fn) — execute fn() inside a transaction (BEGIN IMMEDIATE..COMMIT) */
static int lua_db_batch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not available");

    luaL_checktype(L, 1, LUA_TFUNCTION);

    HlDbHandle *h = db_call_handle(L);

    if (hl_db_begin(h) != 0)
        return luaL_error(L, "BEGIN failed: %s", hl_db_errmsg(h));

    lua_pushvalue(L, 1); /* push the function */
    int rc = lua_pcall(L, 0, 0, 0);

    if (rc != LUA_OK) {
        hl_db_rollback(h);
        return lua_error(L); /* re-raise the error */
    }

    if (hl_db_commit(h) != 0) {
        hl_db_rollback(h);
        return luaL_error(L, "COMMIT failed: %s", hl_db_errmsg(h));
    }

    return 0;
}

/* ── Dialect-aware helpers (backed by HlDbBackend methods) ─────────
 *
 * These let stdlib modules stay DB-backend-agnostic. The actual SQL
 * for each call is constructed inside the backend (db_sqlite.c et al.)
 * — these are thin marshalling layers that turn Lua tables into the
 * HlValue / C-string arrays the backend method expects. */

/* Marshal a Lua array of strings into a C string array allocated on
 * the scratch arena. Returns NULL on bad input (puts an error on the
 * Lua stack via lua_pushstring + raises). */
static const char **lua_strings_from_array(lua_State *L, int idx, int *out_n,
                                            HlLua *lua)
{
    if (lua_type(L, idx) != LUA_TTABLE) {
        luaL_error(L, "expected array of strings at arg %d", idx);
        return NULL;
    }
    int n = (int)luaL_len(L, idx);
    if (n < 0) {
        luaL_error(L, "negative array length at arg %d", idx);
        return NULL;
    }
    const char **arr = sh_arena_alloc(lua->scratch, (size_t)n * sizeof(*arr));
    if (!arr) {
        luaL_error(L, "out of memory");
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, idx, i + 1);
        if (lua_type(L, -1) != LUA_TSTRING) {
            luaL_error(L, "non-string element at index %d", i + 1);
            return NULL;
        }
        arr[i] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    *out_n = n;
    return arr;
}

/* Marshal a Lua array of values into an HlValue[] on the scratch arena.
 * Mirrors the conversion already done in lua_db_query / lua_db_exec
 * but extracted so insert_if_absent / upsert can use it. */
static HlValue *lua_values_from_array(lua_State *L, int idx, int *out_n,
                                       HlLua *lua)
{
    if (lua_type(L, idx) != LUA_TTABLE) {
        luaL_error(L, "expected values array at arg %d", idx);
        return NULL;
    }
    int n = (int)luaL_len(L, idx);
    HlValue *vals = sh_arena_alloc(lua->scratch, (size_t)n * sizeof(*vals));
    if (!vals) {
        luaL_error(L, "out of memory");
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, idx, i + 1);
        switch (lua_type(L, -1)) {
            case LUA_TNIL:
                vals[i].type = HL_TYPE_NIL;
                break;
            case LUA_TBOOLEAN:
                vals[i].type = HL_TYPE_BOOL;
                vals[i].b    = lua_toboolean(L, -1);
                break;
            case LUA_TNUMBER:
                if (lua_isinteger(L, -1)) {
                    vals[i].type = HL_TYPE_INT;
                    vals[i].i    = lua_tointeger(L, -1);
                } else {
                    vals[i].type = HL_TYPE_DOUBLE;
                    vals[i].d    = lua_tonumber(L, -1);
                }
                break;
            case LUA_TSTRING: {
                size_t slen;
                const char *s = lua_tolstring(L, -1, &slen);
                vals[i].type = HL_TYPE_TEXT;
                vals[i].s    = s;
                vals[i].len  = slen;
                break;
            }
            default:
                luaL_error(L, "unsupported value type at index %d", i + 1);
                return NULL;
        }
        lua_pop(L, 1);
    }
    *out_n = n;
    return vals;
}

/* db.insert_if_absent(table, conflict_cols, columns, values)
 *   conflict_cols: array of column names (the unique-constraint
 *                  target). May be empty/nil to mean "ignore on
 *                  any constraint violation".
 *   columns:       array of column names to insert.
 *   values:        array of values, same length as columns.
 *
 * Backend-dispatched: SQLite emits `INSERT OR IGNORE`, Postgres
 * emits `INSERT ... ON CONFLICT(...) DO NOTHING`. */
static int lua_db_insert_if_absent(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not configured");

    const char *table = luaL_checkstring(L, 1);
    int n_conflict = 0;
    const char **conflict_cols = NULL;
    if (!lua_isnil(L, 2)) {
        conflict_cols = lua_strings_from_array(L, 2, &n_conflict, lua);
    }
    int n_cols = 0;
    const char **cols = lua_strings_from_array(L, 3, &n_cols, lua);
    int n_vals = 0;
    HlValue *vals = lua_values_from_array(L, 4, &n_vals, lua);
    if (n_cols != n_vals)
        return luaL_error(L, "columns/values length mismatch (%d vs %d)",
                          n_cols, n_vals);

    HlDbHandle *h = db_call_handle(L);

    int rc = hl_db_insert_if_absent(h, table, conflict_cols, n_conflict,
                                     cols, vals, n_cols);
    if (rc < 0) {
        return luaL_error(L, "db.insert_if_absent: %s", hl_db_errmsg(h));
    }
    lua_pushinteger(L, rc);
    return 1;
}

/* db.upsert(table, conflict_cols, columns, values)
 *   Backend-dispatched: SQLite emits `INSERT ... ON CONFLICT(...) DO
 *   UPDATE SET col=excluded.col, ...` (the spec'd portable form,
 *   equivalent to the old INSERT OR REPLACE pattern). */
static int lua_db_upsert(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not configured");

    const char *table = luaL_checkstring(L, 1);
    int n_conflict = 0;
    const char **conflict_cols = lua_strings_from_array(L, 2, &n_conflict, lua);
    if (n_conflict < 1)
        return luaL_error(L, "db.upsert: conflict_cols must be non-empty");
    int n_cols = 0;
    const char **cols = lua_strings_from_array(L, 3, &n_cols, lua);
    int n_vals = 0;
    HlValue *vals = lua_values_from_array(L, 4, &n_vals, lua);
    if (n_cols != n_vals)
        return luaL_error(L, "columns/values length mismatch (%d vs %d)",
                          n_cols, n_vals);

    HlDbHandle *h = db_call_handle(L);

    int rc = hl_db_upsert(h, table, conflict_cols, n_conflict,
                           cols, vals, n_cols);
    if (rc < 0) {
        return luaL_error(L, "db.upsert: %s", hl_db_errmsg(h));
    }
    lua_pushinteger(L, rc);
    return 1;
}

/* Row-callback adapter for db.table_columns: pushes each column name
 * onto a Lua-side array stored at the top of the stack. */
typedef struct {
    lua_State *L;
    int        i;
} LuaColForwardCtx;

static void lua_db_table_columns_cb(void *cb_ctx, const char *col_name)
{
    LuaColForwardCtx *fwd = (LuaColForwardCtx *)cb_ctx;
    lua_pushstring(fwd->L, col_name);
    lua_rawseti(fwd->L, -2, ++fwd->i);
}

/* db.table_columns(table) -> { "col1", "col2", ... }
 *
 * Backend-dispatched: SQLite uses PRAGMA table_info, Postgres
 * uses information_schema. */
static int lua_db_table_columns(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not configured");

    const char *table = luaL_checkstring(L, 1);
    HlDbHandle *h = db_call_handle(L);

    lua_newtable(L);  /* result */
    LuaColForwardCtx fwd = { L, 0 };
    int rc = hl_db_table_columns(h, table, lua_db_table_columns_cb, &fwd);
    if (rc < 0) {
        return luaL_error(L, "db.table_columns: %s", hl_db_errmsg(h));
    }
    return 1;
}

/* db.quote_identifier(name) -> dialect-quoted identifier string.
 *
 * Wraps a table / column name in the backend's identifier-quote char (doubling
 * any internal occurrence) so a reserved word or special char is safe. Lets a
 * stdlib module interpolate an app-supplied identifier into SQL portably
 * (SQLite / Postgres quote with '"', a future MySQL with '`'). */
static int lua_db_quote_identifier(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    char buf[512];
    int n = hl_db_quote_ident(db_call_handle(L), name, buf, sizeof buf);
    if (n < 0)
        return luaL_error(L, "db.quote_identifier: name too long");
    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

/* The connection-object method table (db_conn_methods, defined near
 * push_conn_object) carries these same functions; the top-level module no
 * longer registers them, so there is no separate db_funcs array. */

/* ── db.async.query / db.async.exec ─────────────────────────────────── */

/* push_result callback: convert HlWorkerDbOp result to Lua value.
 * On error, raises a Lua error (longjmps out of the resume continuation,
 * which propagates as a regular pcall failure in user code). Successful
 * exec returns { changes, last_id }; query returns rows array directly. */
static void lua_push_worker_db_result(lua_State *L, void *driver)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)driver;

    if (op->error) {
        /* luaL_error longjmps — caller's resume completes with an error */
        luaL_error(L, "db.async: %s", op->error_msg);
        return; /* unreachable */
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
            case HL_TYPE_BOOL:
                /* Postgres bool columns arrive as HL_TYPE_BOOL (flag in .i);
                 * SQLite never emits this type. */
                lua_pushboolean(L, vals[col].i != 0);
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
    if (!lua->base.async_ctx)
        return luaL_error(L, "db.async requires an active event loop");
    /* Require a live bound connection: a closed db.open() handle resolves to
     * NULL here, so async-after-close fails closed instead of silently
     * targeting the default database. */
    if (!db_call_handle(L))
        return luaL_error(L, "database not available");

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

    /* Target the database this async call is bound to: db.connect(name).async
     * hits that named connection's DSN; db.default().async (and the bare
     * module async) resolve to the default. A seeded connection whose DSN the
     * registry never saw yields NULL, so the worker falls back to its default
     * connection. The worker opens its own per-thread connection per DSN. */
    {
        const char *dsn = db_call_dsn(L);
        if (dsn) {
            op->dsn = strdup(dsn);
            if (!op->dsn) {
                hl_worker_db_op_free(op);
                free(op);
                return luaL_error(L, "db.async: out of memory");
            }
        }
    }

    /* Create async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.net_ctx, lua->base.alloc);
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
    ctx->detached = (lua->active_conn == NULL);

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

    /* Suspend the FD (attached only). */
    if (!ctx->detached &&
        hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)lua->active_conn, (HlSuspendOp *)&ctx->op) < 0) {
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
/* The SQLite UDF bindings live in the composed per-runtime bridge
 * (mod_db_udf.c → libhull_feature-sqlite-lua.a) so THIS base runtime archive
 * carries no sqlite3_* references (Phase C.2, docs/sqlite_feature.md). The
 * bridge provides a strong hl_lua_db_attach_udf; this weak default (no udf
 * sub-object) stands in when no SQLite backend is composed. */
__attribute__((weak)) void hl_lua_db_attach_udf(lua_State *L, HlDbHandle *h)
{
    (void)L; (void)h;
}

/* ── Module opener ───────────────────────────────────────────────────── */

/* ── Connection objects: db.connect(name) / db.default() ──────────── */

/* The sync method surface bound onto a connection object. Each is the same
 * C function the top-level bridge uses; db_call_handle picks the bound
 * handle (upvalue 1) over the runtime default. async / udf stay on the
 * top-level module for now (the worker layer is single-DSN). */
static const luaL_Reg db_conn_methods[] = {
    {"query",            lua_db_query},
    {"exec",             lua_db_exec},
    {"last_id",          lua_db_last_id},
    {"batch",            lua_db_batch},
    {"insert_if_absent", lua_db_insert_if_absent},
    {"upsert",           lua_db_upsert},
    {"table_columns",    lua_db_table_columns},
    {"quote_identifier", lua_db_quote_identifier},
    {NULL, NULL}
};

/* Push a sub-table whose functions each carry @p h as upvalue 1, so a call on
 * conn.async.* / conn.udf.* resolves the bound connection via db_call_handle
 * exactly like the sync methods do. */
static void push_bound_subtable(lua_State *L, const luaL_Reg *funcs,
                                HlDbHandle *h, const char *field)
{
    lua_newtable(L);
    for (const luaL_Reg *m = funcs; m->name; m++) {
        lua_pushlightuserdata(L, h);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
    lua_setfield(L, -2, field);
}

/* Push (and set as the field "dialect" on the table at the top of the stack) a
 * read-only snapshot of the backend's SQL dialect descriptor. The single home
 * for identifier quoting, placeholder style, upsert grammar, RETURNING support,
 * and identity DDL that the query / schema builders read. */
static void push_dialect_table(lua_State *L, const HlDbBackend *be)
{
    lua_createtable(L, 0, 7);
    char qs[2] = { (be && be->dialect.identifier_quote)
                   ? be->dialect.identifier_quote : '"', '\0' };
    lua_pushstring(L, qs);
    lua_setfield(L, -2, "identifier_quote");
    lua_pushstring(L, (be && be->dialect.placeholder) ? be->dialect.placeholder : "?");
    lua_setfield(L, -2, "placeholder");
    lua_pushstring(L, (be && be->dialect.upsert_style)
                       ? be->dialect.upsert_style : "on_conflict");
    lua_setfield(L, -2, "upsert_style");
    lua_pushboolean(L, be && be->dialect.supports_returning);
    lua_setfield(L, -2, "supports_returning");
    lua_pushboolean(L, be && be->dialect.supports_index_if_not_exists);
    lua_setfield(L, -2, "supports_index_if_not_exists");
    lua_pushboolean(L, be && be->dialect.supports_skip_locked);
    lua_setfield(L, -2, "supports_skip_locked");
    lua_pushstring(L, (be && be->dialect.identity_column)
                       ? be->dialect.identity_column : "INTEGER PRIMARY KEY");
    lua_setfield(L, -2, "identity_column");
    if (be && be->dialect.identity_sequence) {
        lua_pushstring(L, be->dialect.identity_sequence);
        lua_setfield(L, -2, "identity_sequence");
    }
    lua_setfield(L, -2, "dialect");
}

/* Push a fresh connection-object table whose methods carry @p h as upvalue 1. */
static int push_conn_object(lua_State *L, HlDbHandle *h)
{
    lua_createtable(L, 0, 11);
    for (const luaL_Reg *m = db_conn_methods; m->name; m++) {
        lua_pushlightuserdata(L, h);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
    /* async targets this connection's database via the worker pool's per-DSN
     * connections. udf is attached only when the backend supports it (§2.5), so
     * a Postgres connection has no `udf` sub-object at all rather than one that
     * fails at call time. */
    push_bound_subtable(L, db_async_funcs, h, "async");
    const HlDbBackend *be = h ? h->backend : NULL;
    /* The udf sub-object itself is attached by the composed SQLite bridge
     * (strong hl_lua_db_attach_udf); a base with no SQLite backend gets the
     * weak no-op and thus no udf. */
    if (be && be->supports_udf)
        hl_lua_db_attach_udf(L, h);
    lua_pushstring(L, be ? be->name : "none");
    lua_setfield(L, -2, "backend_name");
    lua_pushstring(L, (be && be->dialect.identity_column)
                       ? be->dialect.identity_column : "INTEGER PRIMARY KEY");
    lua_setfield(L, -2, "autoincrement_id_ddl");
    push_dialect_table(L, be);
    return 1;
}

/* __gc for the owner box: release the handle if close() didn't, then free the
 * DSN (the box's final owner of it). */
static int lua_owned_conn_gc(lua_State *L)
{
    HlLuaOwnedConn *o = luaL_testudata(L, 1, HL_LUA_DB_OWNED_MT);
    owned_conn_release(o);
    if (o) { free(o->dsn); o->dsn = NULL; }
    return 0;
}

/* conn.close() → release a db.open() handle now (idempotent; __gc is the
 * backstop). The owner box rides as upvalue 1. */
static int lua_owned_conn_close(lua_State *L)
{
    owned_conn_release(luaL_testudata(L, lua_upvalueindex(1),
                                      HL_LUA_DB_OWNED_MT));
    return 0;
}

/* Push a caller-owned connection object wrapping @p h (from hl_db_dynamic_open),
 * opened from @p dsn. Unlike push_conn_object, this object owns its handle: it
 * adds a close() method and a __gc finalizer that both release it via
 * hl_db_dynamic_close. It carries sync methods + async (bound to the owner box,
 * so conn.async targets @p dsn through the worker pool). udf is intentionally
 * absent: worker-side udf re-registration is keyed off the registry a dynamic
 * handle is not in (tracked follow-up, §2.2). */
static int push_owned_conn_object(lua_State *L, HlDbHandle *h, const char *dsn)
{
    HlLuaOwnedConn *o = lua_newuserdatauv(L, sizeof *o, 0);
    o->h = h;
    o->dsn = NULL;
    luaL_setmetatable(L, HL_LUA_DB_OWNED_MT);   /* installs __gc */
    int owner = lua_gettop(L);
    if (dsn) {
        o->dsn = strdup(dsn);
        /* On OOM the box (on the stack) is GC'd, whose __gc closes h + frees the
         * NULL dsn, so no leak. */
        if (!o->dsn)
            return luaL_error(L, "db.open: out of memory");
    }

    lua_createtable(L, 0, 10);
    for (const luaL_Reg *m = db_conn_methods; m->name; m++) {
        lua_pushvalue(L, owner);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
    lua_pushvalue(L, owner);
    lua_pushcclosure(L, lua_owned_conn_close, 1);
    lua_setfield(L, -2, "close");

    /* async bound to the owner box: conn.async resolves the same live handle +
     * DSN as the sync methods (db_call_handle / db_call_dsn). No udf (see the
     * function comment). */
    lua_newtable(L);
    for (const luaL_Reg *m = db_async_funcs; m->name; m++) {
        lua_pushvalue(L, owner);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
    lua_setfield(L, -2, "async");

    const HlDbBackend *be = h ? h->backend : NULL;
    lua_pushstring(L, be ? be->name : "none");
    lua_setfield(L, -2, "backend_name");
    lua_pushstring(L, (be && be->dialect.identity_column)
                       ? be->dialect.identity_column : "INTEGER PRIMARY KEY");
    lua_setfield(L, -2, "autoincrement_id_ddl");
    push_dialect_table(L, be);

    /* Drop the owner box from under the returned table; the method closures
     * keep it alive (and GC-reachable) as long as the table is. */
    lua_remove(L, owner);
    return 1;
}

/* db.open(dsn) → caller-owned connection object for a runtime-computed DSN,
 * validated against manifest.databases.dynamic (host/scheme allowlist, fs gate
 * for file backends). The app owns the result: call conn.close(), or let GC
 * finalize it. */
static int lua_db_open(lua_State *L)
{
    const char *dsn = luaL_checkstring(L, 1);
    HlLua *lua = get_hl_lua(L);
    if (!lua)
        return luaL_error(L, "no runtime");
    if (!lua->base.db_registry)
        return luaL_error(L, "no database registry available");
    const HlManifestDbDynamic *policy =
        hl_db_registry_dynamic_policy(lua->base.db_registry);
    const char *err = NULL;
    HlDbHandle *h = hl_db_dynamic_open(dsn, policy, lua->base.fs_cfg, &err);
    if (!h)
        return luaL_error(L, "%s", err ? err : "db.open: denied");
    return push_owned_conn_object(L, h, dsn);
}

/* db.default() → connection object for the "default" connection. Returns an
 * object even when no DB is configured; its methods error lazily on use, so
 * requiring a db-using module in a no-db context does not fail at load (it
 * matches require("hull.db")'s historical laziness). */
static int lua_db_default(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    return push_conn_object(L, lua ? default_db(lua) : NULL);
}

/* db.connect(name) → connection object for a manifest-declared database. */
static int lua_db_connect(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    HlLua *lua = get_hl_lua(L);
    if (!lua)
        return luaL_error(L, "no runtime");
    if (!lua->base.db_registry)
        return luaL_error(L, "no database registry available");
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(lua->base.db_registry, name, &err);
    if (!h)
        return luaL_error(L, "db.connect('%s'): %s", name,
                          err ? err : "unknown database");
    return push_conn_object(L, h);
}

int luaopen_hull_db(lua_State *L)
{
    /* The DB module exposes only connection acquisition: every query goes
     * through a connection object from db.connect(name) or db.default(). The
     * historical top-level db.query / exec / async / udf / backend_name / ...
     * bridge is gone; those live on the connection object (db.default()). */
    /* Metatable for caller-owned (db.open) connection handles: __gc releases
     * the handle if close() didn't. Registered once per Lua state. */
    luaL_newmetatable(L, HL_LUA_DB_OWNED_MT);
    lua_pushcfunction(L, lua_owned_conn_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, lua_db_connect);
    lua_setfield(L, -2, "connect");
    lua_pushcfunction(L, lua_db_default);
    lua_setfield(L, -2, "default");
    lua_pushcfunction(L, lua_db_open);
    lua_setfield(L, -2, "open");
    return 1;
}

#endif /* HL_ENABLE_DB */
