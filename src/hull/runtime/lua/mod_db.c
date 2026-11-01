/* mod_db.c - hull.db module: query, exec, batch, async, udf
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "refusal.h"
#include "mod_buffer.h"
#include "internal.h"   /* hl_lua_source_is_stdlib */
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */
#include "protected.h"  /* rows built without raising inside the backend */
#include "hull/utils/secure_zero.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/db_dynamic.h"
#include "hull/cap/fs_resolve.h"   /* the db.open charge scope (audit 39) */
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "hull/worker_db.h"
#include "hull/manifest.h"
#include "log.h"

#include <keel/http_server.h>

#include <sh_arena.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
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
    int        failed;    /* a row could not be built (out of memory) */
} LuaQueryCtx;

/* Runs inside the backend's read loop (between sqlite3_step calls, or while a
 * Postgres / MySQL reply is still on the wire), so it must not raise: an
 * out-of-memory longjmp from here left the reply unread, and the next query
 * on that connection returned this one's rows. The row is built under
 * lua_pcall; a failure stops the query (every backend then drains to the end
 * of the reply) and the binding raises once hl_db_query has returned. */
static int lua_query_row_cb(void *opaque, HlColumn *cols, int ncols)
{
    LuaQueryCtx *qc = (LuaQueryCtx *)opaque;
    if (hl_lua_append_row(qc->L, qc->table_idx, (lua_Integer)qc->row_count + 1,
                          cols, ncols) != 0) {
        qc->failed = 1;
        return 1;
    }
    qc->row_count++;
    return 0;
}

static void lua_scratch_release(lua_State *L, const void *mark);

/* Marshal Lua table values to HlValue array for parameter binding */
static int lua_to_hl_values(lua_State *L, int idx,
                               HlValue **out_params, int *out_count)
{
    *out_params = NULL;
    *out_count = 0;

    if (lua_isnoneornil(L, idx))
        return 0;

    luaL_checktype(L, idx, LUA_TTABLE);
    /* Raw: luaL_len ran the table's __len - app code that could close the
     * connection whose handle the caller already held. */
    lua_Unsigned rl = lua_rawlen(L, idx);
    if (rl > (lua_Unsigned)INT_MAX)
        return -1;
    int len = (int)rl;
    if (len <= 0)
        return 0;

    /* Overflow guard */
    if ((size_t)len > SIZE_MAX / sizeof(HlValue))
        return -1;

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return -1;

    /* Every value stays on the stack (it keeps its string alive), so room
     * for all of them first: past the free slots it wrote out of bounds. */
    if (!lua_checkstack(L, len))
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
            params[i].type = HL_TYPE_NIL;
            break;
        default: {
            /* A table / function / userdata bound NULL, silently (audit 27):
             * refused, as upsert's values and the JS bindings refuse it. The
             * scratch goes back before the raise. */
            const char *tn = lua_typename(L, t);
            lua_scratch_release(L, params);
            return luaL_error(L, "params: unsupported value type '%s' at "
                              "index %d", tn, i + 1);
        }
        }
        /* Leave values on stack - they keep strings alive */
    }

    *out_params = params;
    *out_count = len;
    return 0;
}

/* Give the scratch arena back down to @p mark (the call's first scratch
 * allocation), without touching the stack: the raise paths use it, whose
 * stack the error discards anyway. */
static void lua_scratch_release(lua_State *L, const void *mark)
{
    HlLua *lua = get_hl_lua(L);
    if (mark && lua && lua->scratch && (const char *)mark >= lua->scratch->buffer &&
        (size_t)((const char *)mark - lua->scratch->buffer) <= lua->scratch->used)
        lua->scratch->used = (size_t)((const char *)mark - lua->scratch->buffer);
}

static void lua_free_hl_values(lua_State *L, HlValue *params, int count)
{
    if (!params)
        return;
    /* Pop the values we left on the stack in lua_to_hl_values.
     * No free() - params live in the per-request scratch arena, which gets
     * them back: it is reset only when a run starts, so a bulk import's
     * thousands of statements in one run filled it, and every later scratch
     * user in the run failed ("params must be a table"; audit 22). The
     * params are the call's first scratch allocation and nothing after
     * them is used past this point. */
    if (count > 0)
        lua_pop(L, count);
    lua_scratch_release(L, params);
}

/* Check if the immediate Lua caller is a stdlib module (chunk name starts
 * with "hull.").  User modules start with "./" - so a simple prefix check
 * is sufficient.  Returns 1 for stdlib, 0 for user code. */
static int lua_is_stdlib_caller(lua_State *L, const char *method)
{
    lua_Debug ar;
    /* level 0 = this C function, level 1 = Lua caller */
    if (lua_getstack(L, 1, &ar) == 0)
        return 0;
    if (lua_getinfo(L, "S", &ar) == 0)
        return 0;
    if (!hl_lua_source_is_stdlib(ar.source))
        return 0;

    /* A stdlib frame is not enough on its own. A stdlib helper that calls a
     * function the APP handed it - hull.map(list, fn), a loader, a callback -
     * would otherwise lend its identity to whatever that function is: passing
     * `conn.exec` itself, with the SQL as the list item, ran app SQL with
     * stdlib privilege. So the call must also NAME this method, as
     * `conn.exec(...)` or `conn:exec(...)`, the way the stdlib does. A function
     * held in a parameter or local ("local" / "upvalue") is refused, and so is
     * any other name. A tail-called C function keeps its caller's frame in Lua
     * 5.4, so `return db.exec(...)` in the stdlib still names the method. */
    lua_Debug self;
    if (lua_getstack(L, 0, &self) == 0 || lua_getinfo(L, "n", &self) == 0)
        return 0;
    if (!self.name || !self.namewhat ||
        (strcmp(self.namewhat, "method") != 0 &&
         strcmp(self.namewhat, "field") != 0))
        return 0;
    if (strcmp(self.name, method) != 0)
        return 0;
    /* hull.search's SQL reaches only its own `_hull_fts_*` index (exempt by
     * name) and app tables: the authorizer treats it as untrusted, so a read
     * the index prepares while it runs (an external-content table) cannot
     * reach another `_hull_*` table (audit 40). */
    const char *src = ar.source;
    if (*src == '@' || *src == '=') src++;
    if (strcmp(src, "hull.search") == 0)
        return HL_DB_NS_STDLIB_SCOPED;
    return HL_DB_NS_TRUSTED;
}

/* Charge SQL text (or identifiers) a call is about to scan and hand the
 * statement cache: one unit per 8 bytes plus the call floor, raising BEFORE
 * the work once the run is over its budget (audit 41). */
static void sql_charge(lua_State *L, size_t len)
{
    uint64_t u = hl_work_sql_units(len);
    lua_hlwork(L, u > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)u, 0);
}

#define HL_LUA_DB_RESERVED_MSG "access denied: _hull_* tables are reserved"

/* Refuse an app caller's identifier that names a reserved `_hull_*` table.
 * insert_if_absent / upsert / table_columns build their SQL from these names,
 * so the query/exec text check never sees them; every name is checked, since
 * a column name lands in the same statement. 0 when the caller may name
 * these identifiers, -1 when one is reserved. Does not raise:
 * insert_if_absent / upsert give their scratch arrays back first (audit 24);
 * check_app_identifiers is the raising form. */
static int app_identifiers_ok(lua_State *L, const char *method,
                              const char *table,
                              const char **a, int na,
                              const char **b, int nb)
{
    /* The names are scanned here and land in the statement the backend
     * builds (and the statement cache compares): charged first, stdlib or
     * not (audit 41). Non-raising, as the rest of this function. */
    uint64_t bytes = table ? strlen(table) : 0;
    for (int i = 0; i < na; i++) bytes += strlen(a[i]);
    for (int i = 0; i < nb; i++) bytes += strlen(b[i]);
    uint64_t u = hl_work_sql_units((size_t)bytes);
    if (hl_lua_budget_charge(L, u > (uint64_t)INT64_MAX ? INT64_MAX
                                                        : (int64_t)u))
        return -2;
    if (lua_is_stdlib_caller(L, method))
        return 0;
    int bad = hl_cap_db_check_namespace(table) != 0;
    for (int i = 0; !bad && i < na; i++)
        bad = hl_cap_db_check_namespace(a[i]) != 0;
    for (int i = 0; !bad && i < nb; i++)
        bad = hl_cap_db_check_namespace(b[i]) != 0;
    return bad ? -1 : 0;
}

static void check_app_identifiers(lua_State *L, const char *method,
                                  const char *table,
                                  const char **a, int na,
                                  const char **b, int nb)
{
    int rc = app_identifiers_ok(L, method, table, a, na, b, nb);
    if (rc == -2)
        (void)hl_lua_budget_raise(L);
    if (rc != 0)
        luaL_error(L, HL_LUA_DB_RESERVED_MSG);
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
/* Row loops in progress: hl_db_query / hl_db_table_columns calling back into
 * Lua per row. A row-table allocation can run a GC step, and a GC step can run
 * a script __gc - Lua code, inside the loop - that calls the database again.
 * That re-entrant call could evict and finalize the statement being stepped,
 * or close the connection under it, and the next step reads freed memory. So
 * while a loop runs, every connection method refuses (db_call_handle and
 * close() check this). Event-loop thread only.
 *
 * The count comes back down through a to-be-closed guard, so an error
 * unwinding out of the row callback (out of memory) cannot leave it raised. */
static int g_db_row_loops;

static int row_loop_guard_close(lua_State *L)
{
    (void)L;
    if (g_db_row_loops > 0) g_db_row_loops--;
    return 0;
}

/* Push the guard and mark it to-be-closed. Returns its stack index; closing
 * it (lua_settop below it, lua_closeslot, or an error unwinding) ends the
 * loop's count. */
static int push_row_loop_guard(lua_State *L)
{
    lua_newuserdatauv(L, 1, 0);
    if (luaL_newmetatable(L, "hull.db.rowloop")) {
        lua_pushcfunction(L, row_loop_guard_close);
        lua_setfield(L, -2, "__close");
    }
    lua_setmetatable(L, -2);
    g_db_row_loops++;
    lua_toclose(L, -1);
    return lua_gettop(L);
}

/* The handle this call's connection resolves to, with no re-entry check:
 * for the re-resolve a binding does under its own row-loop guard. */
static HlDbHandle *db_resolve_handle(lua_State *L)
{
    int uv = lua_upvalueindex(1);
    if (lua_islightuserdata(L, uv)) {
        /* A registry slot. Once the registry is closed at teardown (a __gc
         * finalizer lua_close runs) the slot has no backend (audit 17). */
        HlDbHandle *h = (HlDbHandle *)lua_touserdata(L, uv);
        return h && h->backend ? h : NULL;
    }
    HlLuaOwnedConn *o = luaL_testudata(L, uv, HL_LUA_DB_OWNED_MT);
    if (o)
        return o->h;   /* NULL after close() → methods error "not available" */
    HlLua *lua = get_hl_lua(L);
    if (!lua) return NULL;
    return default_db(lua);
}

static HlDbHandle *db_call_handle(lua_State *L)
{
    if (g_db_row_loops > 0)
        luaL_error(L, "db: called while a query's rows are being read "
                      "(from a __gc finalizer?)");
    return db_resolve_handle(L);
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

    size_t sql_len;
    const char *sql = luaL_checklstring(L, 1, &sql_len);
    /* The text check and the statement cache's compare walk the whole
     * text: charged first, stdlib or not (audit 41). */
    sql_charge(L, sql_len);

    int is_stdlib = lua_is_stdlib_caller(L, "query");
    (void)db_call_handle(L);   /* the re-entry check; the handle is resolved below */

    if (!is_stdlib && hl_cap_db_check_namespace(sql) != 0)
        return luaL_error(L, "access denied: _hull_* tables are reserved");
    lua_settop(L, 2);   /* sql, params (or nil): the slots pushed below follow */

    /* The result table and the guard come BEFORE the params are marshalled
     * (audit 27): both allocate, and a LUA_ERRMEM raised after the params
     * left the scratch arena advanced for the rest of the run. */
    lua_newtable(L);
    int table_idx = lua_gettop(L);

    LuaQueryCtx qc = {
        .L = L,
        .table_idx = table_idx,
        .row_count = 0,
    };

    (void)push_row_loop_guard(L);   /* closed by the settop below */

    HlValue *params = NULL;
    int nparams = 0;
    if (!lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    /* The handle again, now that nothing more runs before the call: the
     * params' conversion, the result table and the guard can all run app
     * code (an __index, a __gc) that closes a db.open connection. */
    HlDbHandle *h = db_resolve_handle(L);
    if (!h) {
        lua_scratch_release(L, params);   /* audit 24: raised with it held */
        return luaL_error(L, "query: the connection was closed");
    }
    /* The authorizer's `_hull_*` check follows the same rule as the text
     * check above (audit 39): a stored view / trigger reaching one is
     * refused for app SQL. Set around the call only (prepare and step). */
    int ns_prev = hl_cap_db_ns_trust(is_stdlib);
    int rc = hl_db_query(h, sql, params, nparams,
                         lua_query_row_cb, &qc, lua->base.alloc);
    hl_cap_db_ns_restore(ns_prev);
    /* Closes the guard and pops the params lua_to_hl_values left on the
     * stack (they kept the string pointers alive during the query): the
     * result table is on top. */
    lua_settop(L, table_idx);
    lua_scratch_release(L, params);

    /* A row build that failed because the run's budget or deadline tripped
     * inside it raises the trip, not a catchable memory error (audit 39). */
    if (qc.failed && hl_lua_budget_tripped(L))
        return hl_lua_budget_raise(L);
    if (qc.failed)
        return luaL_error(L, "db.query: not enough memory for the result");
    if (rc != 0) {
        lua_pop(L, 1); /* pop result table */
        return hl_lua_raise_copy(L, "query failed: ", hl_db_errmsg(h));
    }

    return 1; /* result table already on stack */
}

/* db.exec implementation */
static int lua_db_exec_impl(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not available");

    size_t sql_len;
    const char *sql = luaL_checklstring(L, 1, &sql_len);
    /* The text check and the statement cache's compare walk the whole
     * text: charged first, stdlib or not (audit 41). */
    sql_charge(L, sql_len);

    int is_stdlib = lua_is_stdlib_caller(L, "exec");
    (void)db_call_handle(L);   /* the re-entry check; the handle is resolved below */

    if (!is_stdlib && hl_cap_db_check_namespace(sql) != 0)
        return luaL_error(L, "access denied: _hull_* tables are reserved");
    lua_settop(L, 2);   /* sql, params (or nil): the slots pushed below follow */

    /* A Lua UDF runs inside the statement's step, so this is a row loop too
     * as far as re-entry is concerned. The guard allocates, so it is made
     * before the params take scratch space (audit 27, as lua_db_query_impl). */
    int guard = push_row_loop_guard(L);

    HlValue *params = NULL;
    int nparams = 0;
    if (!lua_isnil(L, 2)) {
        if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    HlDbHandle *h = db_resolve_handle(L);       /* see lua_db_query_impl */
    if (!h) {
        lua_scratch_release(L, params);   /* audit 24, as lua_db_query_impl */
        return luaL_error(L, "exec: the connection was closed");
    }
    int ns_prev = hl_cap_db_ns_trust(is_stdlib);   /* see query */
    int rc = hl_db_exec(h, sql, params, nparams);
    hl_cap_db_ns_restore(ns_prev);
    lua_settop(L, guard - 1);   /* closes the guard, pops the params */
    lua_scratch_release(L, params);

    if (rc < 0)
        return hl_lua_raise_copy(L, "exec failed: ", hl_db_errmsg(h));

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

/* conn.in_transaction() - whether this connection is inside a transaction
 * (a db.batch, or a raw BEGIN). False where the backend cannot tell. The
 * durable-workflow waits (hull.jobs ctx.sleep / ctx.wait_signal) refuse
 * inside one: they unwind by raising, which rolled back their own record. */
static int lua_db_in_transaction(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not available");
    lua_pushboolean(L, hl_db_in_txn(db_call_handle(L)));
    return 1;
}

/* db.batch(fn) - execute fn() inside a transaction (BEGIN IMMEDIATE..COMMIT) */
static int lua_db_batch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !db_call_handle(L))
        return luaL_error(L, "database not available");

    luaL_checktype(L, 1, LUA_TFUNCTION);

    HlDbHandle *h = db_call_handle(L);

    /* Nested inside another batch: a savepoint (see hl_db_batch_enter). */
    if (hl_db_batch_enter(h) != 0)
        return hl_lua_raise_copy(L, "BEGIN failed: ", hl_db_errmsg(h));

    lua_pushvalue(L, 1); /* push the function */
    int rc = lua_pcall(L, 0, 0, 0);

    /* Resolve again: fn may have closed a db.open handle, freeing h. */
    h = db_call_handle(L);
    if (rc != LUA_OK) {
        if (h) (void)hl_db_batch_leave(h, 0);
        return lua_error(L); /* re-raise the error */
    }
    if (!h)
        return luaL_error(L, "db.batch: the connection was closed inside the batch");

    if (hl_db_batch_leave(h, 1) != 0)
        return hl_lua_raise_copy(L, "COMMIT failed: ", hl_db_errmsg(h));

    return 0;
}

/* ── Dialect-aware helpers (backed by HlDbBackend methods) ─────────
 *
 * These let stdlib modules stay DB-backend-agnostic. The actual SQL
 * for each call is constructed inside the backend (db_sqlite.c et al.)
 * - these are thin marshalling layers that turn Lua tables into the
 * HlValue / C-string arrays the backend method expects. */

/* Marshal a Lua array of strings into a C string array allocated on
 * the scratch arena. Returns NULL on bad input with the reason in @p err.
 * It does not raise (audit 24): its callers give the call's scratch back
 * before they do - raised with it held, a pcall loop over a refused call
 * filled the arena for the rest of the run. Nothing here allocates on the
 * Lua heap or runs app code. */
static const char **lua_strings_from_array(lua_State *L, int idx, int *out_n,
                                            HlLua *lua, char *err, size_t errsz)
{
    if (lua_type(L, idx) != LUA_TTABLE) {
        snprintf(err, errsz, "expected array of strings at arg %d", idx);
        return NULL;
    }
    /* Raw length, no __len: a metamethod is app code, and app code
     * running here could drop the strings already taken. */
    lua_Unsigned rl = lua_rawlen(L, idx);
    if (rl > (lua_Unsigned)INT_MAX) {
        snprintf(err, errsz, "array too long at arg %d", idx);
        return NULL;
    }
    int n = (int)rl;
    const char **arr = sh_arena_alloc(lua->scratch, (size_t)n * sizeof(*arr));
    if (!arr) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, idx, i + 1);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            snprintf(err, errsz, "non-string element at index %d", i + 1);
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
                                       HlLua *lua, char *err, size_t errsz)
{
    if (lua_type(L, idx) != LUA_TTABLE) {
        snprintf(err, errsz, "expected values array at arg %d", idx);
        return NULL;
    }
    lua_Unsigned rl = lua_rawlen(L, idx);
    if (rl > (lua_Unsigned)INT_MAX) {
        snprintf(err, errsz, "values array too long at arg %d", idx);
        return NULL;
    }
    int n = (int)rl;
    HlValue *vals = sh_arena_alloc(lua->scratch, (size_t)n * sizeof(*vals));
    if (!vals) {
        snprintf(err, errsz, "out of memory");
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
                lua_pop(L, 1);
                snprintf(err, errsz, "unsupported value type at index %d", i + 1);
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
    /* The scratch arrays below are this call's: given back at its end. */
    size_t scratch_mark_at = lua->scratch ? lua->scratch->used : 0;

    /* Every refusal below gives the scratch back before it raises (audit
     * 24: raised with it held, as lua_db_async_common's were in audit 23). */
    const char *table = luaL_checkstring(L, 1);
    char err[96];
    err[0] = '\0';
    int n_conflict = 0;
    const char **conflict_cols = NULL;
    const char **cols = NULL;
    HlValue *vals = NULL;
    int n_cols = 0, n_vals = 0;
    int ok = 1;
    if (!lua_isnil(L, 2)) {
        conflict_cols = lua_strings_from_array(L, 2, &n_conflict, lua,
                                               err, sizeof err);
        ok = conflict_cols != NULL;
    }
    if (ok) {
        cols = lua_strings_from_array(L, 3, &n_cols, lua, err, sizeof err);
        ok = cols != NULL;
    }
    if (ok) {
        vals = lua_values_from_array(L, 4, &n_vals, lua, err, sizeof err);
        ok = vals != NULL;
    }
    if (ok && n_cols != n_vals) {
        snprintf(err, sizeof err, "columns/values length mismatch (%d vs %d)",
                 n_cols, n_vals);
        ok = 0;
    }
    int id_rc = ok ? app_identifiers_ok(L, "insert_if_absent", table,
                                        conflict_cols, n_conflict,
                                        cols, n_cols) : 0;
    if (id_rc != 0) {
        snprintf(err, sizeof err, "%s", HL_LUA_DB_RESERVED_MSG);
        ok = 0;
    }
    if (!ok) {
        if (lua->scratch) lua->scratch->used = scratch_mark_at;
        if (id_rc == -2)
            return hl_lua_budget_raise(L);   /* the charge tripped (audit 41) */
        return luaL_error(L, "%s", err);
    }

    HlDbHandle *h = db_call_handle(L);

    int ns_prev = hl_cap_db_ns_trust(lua_is_stdlib_caller(L, "insert_if_absent"));
    int rc = hl_db_insert_if_absent(h, table, conflict_cols, n_conflict,
                                     cols, vals, n_cols);
    hl_cap_db_ns_restore(ns_prev);
    if (lua->scratch) lua->scratch->used = scratch_mark_at;
    if (rc < 0) {
        return hl_lua_raise_copy(L, "db.insert_if_absent: ", hl_db_errmsg(h));
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
    /* The scratch arrays below are this call's: given back at its end. */
    size_t scratch_mark_at = lua->scratch ? lua->scratch->used : 0;

    /* Every refusal gives the scratch back first (audit 24; see
     * lua_db_insert_if_absent). */
    const char *table = luaL_checkstring(L, 1);
    char err[96];
    err[0] = '\0';
    int n_conflict = 0, n_cols = 0, n_vals = 0;
    const char **cols = NULL;
    HlValue *vals = NULL;
    const char **conflict_cols = lua_strings_from_array(L, 2, &n_conflict, lua,
                                                        err, sizeof err);
    int ok = conflict_cols != NULL;
    if (ok && n_conflict < 1) {
        snprintf(err, sizeof err, "db.upsert: conflict_cols must be non-empty");
        ok = 0;
    }
    if (ok) {
        cols = lua_strings_from_array(L, 3, &n_cols, lua, err, sizeof err);
        ok = cols != NULL;
    }
    if (ok) {
        vals = lua_values_from_array(L, 4, &n_vals, lua, err, sizeof err);
        ok = vals != NULL;
    }
    if (ok && n_cols != n_vals) {
        snprintf(err, sizeof err, "columns/values length mismatch (%d vs %d)",
                 n_cols, n_vals);
        ok = 0;
    }
    int id_rc = ok ? app_identifiers_ok(L, "upsert", table,
                                        conflict_cols, n_conflict,
                                        cols, n_cols) : 0;
    if (id_rc != 0) {
        snprintf(err, sizeof err, "%s", HL_LUA_DB_RESERVED_MSG);
        ok = 0;
    }
    if (!ok) {
        if (lua->scratch) lua->scratch->used = scratch_mark_at;
        if (id_rc == -2)
            return hl_lua_budget_raise(L);   /* the charge tripped (audit 41) */
        return luaL_error(L, "%s", err);
    }

    HlDbHandle *h = db_call_handle(L);

    int ns_prev = hl_cap_db_ns_trust(lua_is_stdlib_caller(L, "upsert"));
    int rc = hl_db_upsert(h, table, conflict_cols, n_conflict,
                           cols, vals, n_cols);
    hl_cap_db_ns_restore(ns_prev);
    if (lua->scratch) lua->scratch->used = scratch_mark_at;
    if (rc < 0) {
        return hl_lua_raise_copy(L, "db.upsert: ", hl_db_errmsg(h));
    }
    lua_pushinteger(L, rc);
    return 1;
}

/* Row-callback adapter for db.table_columns: pushes each column name
 * onto a Lua-side array stored at the top of the stack. */
typedef struct {
    lua_State *L;
    int        i;
    int        failed;
} LuaColForwardCtx;

/* (args: name, result) */
static int lua_db_table_columns_k(lua_State *L)
{
    const HlLuaBytes *name = (const HlLuaBytes *)lua_touserdata(L, 1);
    lua_pushstring(L, name->p);
    lua_rawseti(L, 2, (lua_Integer)lua_tointeger(L, 3));
    return 0;
}

/* Runs inside the backend's query, so it must not raise (see
 * lua_query_row_cb): each name is stored under lua_pcall, and a failure is
 * raised after hl_db_table_columns returns. */
static void lua_db_table_columns_cb(void *cb_ctx, const char *col_name)
{
    LuaColForwardCtx *fwd = (LuaColForwardCtx *)cb_ctx;
    lua_State *L = fwd->L;
    if (fwd->failed || !lua_checkstack(L, 4)) {
        fwd->failed = 1;
        return;
    }
    int result = lua_gettop(L);
    HlLuaBytes name = { col_name, 0 };
    lua_pushcfunction(L, lua_db_table_columns_k);
    lua_pushlightuserdata(L, &name);
    lua_pushvalue(L, result);
    lua_pushinteger(L, (lua_Integer)fwd->i + 1);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        lua_pop(L, 1);
        fwd->failed = 1;
        return;
    }
    fwd->i++;
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
    check_app_identifiers(L, "table_columns", table, NULL, 0, NULL, 0);
    (void)db_call_handle(L);   /* the re-entry check; the handle is resolved below */

    int guard = push_row_loop_guard(L);
    lua_newtable(L);  /* result */
    HlDbHandle *h = db_resolve_handle(L);       /* see lua_db_query_impl */
    if (!h)
        return luaL_error(L, "db.table_columns: the connection was closed");
    LuaColForwardCtx fwd = { L, 0, 0 };
    int rc = hl_db_table_columns(h, table, lua_db_table_columns_cb, &fwd);
    lua_closeslot(L, guard);    /* the result table stays on top */
    if (fwd.failed && hl_lua_budget_tripped(L))
        return hl_lua_budget_raise(L);   /* audit 39, as db.query */
    if (fwd.failed)
        return luaL_error(L, "db.table_columns: not enough memory for the result");
    if (rc < 0) {
        return hl_lua_raise_copy(L, "db.table_columns: ", hl_db_errmsg(h));
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

/* Continuation for db.async.query / .exec. Runs INSIDE lua_resume (Lua calls
 * it when the yielded coroutine is resumed), so a raise here is a proper
 * coroutine error - catchable by pcall, otherwise a 500 - rather than a
 * longjmp out of the resume that leaves the connection suspended (#321).
 *
 * The op arrives as the yield's KContext and is still alive: both resume paths
 * in shared/async.c call cont->resume BEFORE free_driver. Mirrors
 * lua_compute_async_k (#320) and the JS side's promise reject (#319).
 *
 * On success lua_push_worker_db_result has already pushed the single result
 * value; return it unchanged. */
static int lua_db_async_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status;
    if (!hl_lua_async_k_ok(L))   /* audit 33: kctx may be freed */
        return luaL_error(L, "%s: this wait was abandoned", "db.async");
    HlWorkerDbOp *op = (HlWorkerDbOp *)kctx;
    if (op && op->error)
        return luaL_error(L, "db.async: %s", op->error_msg);
    return 1;
}

/* push_result callback: convert HlWorkerDbOp result to Lua value.
 * On error it pushes a NIL PLACEHOLDER and leaves the raising to
 * lua_db_async_k - see there for why. Successful exec returns
 * { changes, last_id }; query returns rows array directly. */
static void lua_push_worker_db_result(lua_State *L, void *driver)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)driver;

    if (op->error) {
        /* Push a placeholder and let lua_db_async_k raise. Raising HERE would
         * longjmp out of hl_async_on_resume, which runs BEFORE lua_resume - the
         * error would escape the resume entirely and strand the suspended
         * connection (issue #321: the request hangs at HTTP 000 instead of
         * failing). The continuation raises from INSIDE lua_resume, where it
         * becomes a proper coroutine error a pcall can catch. */
        lua_pushnil(L);
        return;
    }

    if (op->kind == HL_WORK_DB_WAIT_NOTIFY) {
        lua_pushboolean(L, op->notified);
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

    /* HL_WORK_DB_QUERY - array of row tables */
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
    HlDbHandle *bound = db_call_handle(L);
    if (!bound)
        return luaL_error(L, "database not available");

    /* Input differs by kind: WAIT_NOTIFY takes (channel, timeout_ms); the
     * query/exec kinds take (sql, params). */
    const char *sql = NULL, *channel = NULL;
    int timeout_ms = 0;
    int ns_trusted = 0;   /* the stdlib-frame verdict, for the worker */
    HlValue *params = NULL;
    int nparams = 0;

    if (kind == HL_WORK_DB_WAIT_NOTIFY) {
        /* A backend with no LISTEN/NOTIFY fails here (audit 14): the worker
         * opened a connection only to have the wait return at once. */
        if (!bound->backend || !bound->backend->dialect.supports_notify ||
            !bound->backend->wait_notify)
            return luaL_error(L, "wait_notify: this database has no "
                              "LISTEN/NOTIFY (check conn.dialect.supports_notify)");
        size_t channel_len;
        channel = luaL_checklstring(L, 1, &channel_len);
        if (channel_len > HL_WORKER_DB_CHANNEL_MAX)
            return luaL_error(L, "wait_notify: a channel name is at most %d "
                              "bytes", HL_WORKER_DB_CHANNEL_MAX);
        /* 0..HL_WORKER_DB_NOTIFY_MAX_MS (audit 14): the wait holds a pool
         * thread, and an unclamped value held one for ~24 days. */
        timeout_ms = hl_worker_db_notify_timeout(
            (double)luaL_optinteger(L, 2, 1000));
    } else {
        size_t sql_len;
        sql = luaL_checklstring(L, 1, &sql_len);
        sql_charge(L, sql_len);   /* before the scan (audit 41) */
        ns_trusted = lua_is_stdlib_caller(L, kind == HL_WORK_DB_QUERY ? "query" : "exec");
        if (!ns_trusted && hl_cap_db_check_namespace(sql) != 0)
            return luaL_error(L, "access denied: _hull_* tables are reserved");
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
            if (lua_to_hl_values(L, 2, &params, &nparams) != 0)
                return luaL_error(L, "params must be a table");
        }
    }

    /* After the arguments (a number converted to a string allocates, and
     * an allocation can run a __gc finalizer that BEGINs), before anything
     * is armed (audit 15). */
    /* The same finalizer may have closed a db.open handle: the op would then
     * run against the closed connection's DSN, after close() (audit 16, as
     * JS's js_call_handle re-check). Resolve it again. */
    if (!db_call_handle(L)) {
        lua_free_hl_values(L, params, nparams);
        return luaL_error(L, "database not available");
    }
    if (!hl_lua_can_wait(L)) {
        lua_free_hl_values(L, params, nparams);
        hl_lua_check_can_wait(L, "db.async");   /* raises */
        return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "db.async cannot wait here");
    }

    /* The copies below live outside the heap until the op is freed: charged
     * and held on the VM's off-heap total under its heap limit before they
     * are made - a table naming one big string 32766 times was copied
     * 32766 times (audit 22). Neither runs app code. Both failures give
     * the params' scratch space back before they raise (audit 23: raised
     * with it held, a pcall loop over a refused call filled the arena for
     * the rest of the run) - so the charge is the non-raising one, and the
     * trip is raised once the params are released. */
    HlOffheapHold copy_hold = {0};
    if (kind != HL_WORK_DB_WAIT_NOTIFY) {
        size_t copy_bytes = hl_worker_db_copy_bytes(params, nparams,
                                                    strlen(sql) + 1);
        uint64_t units = hl_work_bytes_units((uint64_t)copy_bytes);
        if (hl_lua_budget_charge(L, units > (uint64_t)INT64_MAX
                                    ? INT64_MAX : (int64_t)units)) {
            lua_free_hl_values(L, params, nparams);
            return hl_lua_budget_raise(L);
        }
        if (hl_offheap_take(&copy_hold, &lua->wasm_buf_bytes, lua->mem_limit,
                            copy_bytes) != 0) {
            lua_free_hl_values(L, params, nparams);
            return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "db.async: %I bytes of SQL and params would "
                              "take the VM's off-heap copies past its heap "
                              "limit", (lua_Integer)(copy_bytes > (size_t)LUA_MAXINTEGER
                                  ? LUA_MAXINTEGER : copy_bytes));
        }
    }

    /* Allocate op */
    HlWorkerDbOp *op = calloc(1, sizeof(HlWorkerDbOp));
    if (!op) {
        hl_offheap_drop(&copy_hold);
        lua_free_hl_values(L, params, nparams);
        return luaL_error(L, "db.async: out of memory");
    }
    op->copy_hold = copy_hold;

    op->kind = kind;
    op->server = lua->server;
    op->alloc = lua->base.alloc;

    if (kind == HL_WORK_DB_WAIT_NOTIFY) {
        op->channel = strdup(channel);
        op->timeout_ms = timeout_ms;
        if (!op->channel) {
            free(op);
            return luaL_error(L, "db.async: out of memory");
        }
    } else {
        op->sql = strdup(sql);
        if (!op->sql) {
            lua_free_hl_values(L, params, nparams);
            hl_worker_db_op_free(op);
            free(op);
            return luaL_error(L, "db.async: out of memory");
        }
        /* Deep-copy params (they need to outlive this stack frame) */
        if (nparams > 0) {
            op->params = hl_deep_copy_params(params, nparams);
            op->nparams = nparams;
            if (!op->params) {
                lua_free_hl_values(L, params, nparams);
                hl_worker_db_op_free(op);
                free(op);
                return luaL_error(L, "db.async: out of memory");
            }
        }
        lua_free_hl_values(L, params, nparams);
    }

    /* Target the database this async call is bound to: db.connect(name).async
     * hits that named connection's DSN; db.default().async (and the bare
     * module async) resolve to the default. A seeded connection whose DSN the
     * registry never saw yields NULL, so the worker falls back to its default
     * connection. The worker opens its own per-thread connection per DSN. */
    {
        const char *dsn = db_call_dsn(L);
        op->max_instructions = lua->max_instructions;   /* the op's own */
        op->max_result_bytes = lua->mem_limit;
        op->ns_trusted = ns_trusted;
        {
            HlDbHandle *sh = db_resolve_handle(L);
            op->session_control = sh ? sh->session_control : 0;
        }
        hl_offheap_shared_init(&op->result_hold, &lua->async_result_bytes,
                               lua->mem_limit);
        op->no_cache = luaL_testudata(L, lua_upvalueindex(1),
                                      HL_LUA_DB_OWNED_MT) != NULL;
        if (op->no_cache) op->dyn_id = hl_db_dynamic_id(db_resolve_handle(L));
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
        /* No cancel(): it unrefs the handler coroutine's registry slot,
         * which dispatch unrefs again when this error reaches it - a
         * double luaL_unref corrupts the registry free list. */
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
        return luaL_error(L, "db.async: failed to suspend connection");
    }

    return lua_yieldk(L, 0, (lua_KContext)op, lua_db_async_k);
}

static int lua_db_async_query(lua_State *L)
{
    return lua_db_async_common(L, HL_WORK_DB_QUERY);
}

static int lua_db_async_exec(lua_State *L)
{
    return lua_db_async_common(L, HL_WORK_DB_EXEC);
}

/* conn.wait_notify(channel, timeout_ms) - yielding low-latency wakeup. Parks
 * the coroutine on the db.async worker pool while a worker blocks on the
 * backend's LISTEN/NOTIFY primitive; returns true (notified) or false
 * (timeout, or a dropped connection). Latency only: false is
 * indistinguishable from a plain sleep. Raises on a backend without
 * LISTEN/NOTIFY, so callers gate on conn.dialect.supports_notify. The
 * timeout is clamped to 0..HL_WORKER_DB_NOTIFY_MAX_MS. */
static int lua_db_wait_notify(lua_State *L)
{
    return lua_db_async_common(L, HL_WORK_DB_WAIT_NOTIFY);
}

static const luaL_Reg db_async_funcs[] = {
    {"query", lua_db_async_query},
    {"exec",  lua_db_async_exec},
    {NULL, NULL}
};

/* ── db.udf - user-defined SQL functions ─────────────────────────────── */
/* The SQLite UDF bindings live in the composed per-runtime bridge
 * (mod_db_udf.c → libhull_feature-sqlite-lua.a) so THIS base runtime archive
 * carries no sqlite3_* references (docs/sqlite_feature.md). The
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
    {"wait_notify",      lua_db_wait_notify},
    {"in_transaction",   lua_db_in_transaction},
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
    lua_createtable(L, 0, 8);
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
    lua_pushboolean(L, be && be->dialect.supports_notify);
    lua_setfield(L, -2, "supports_notify");
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
    lua_createtable(L, 0, 12);
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
    if (o) { hl_secure_free_str(o->dsn); o->dsn = NULL; }
    return 0;
}

/* conn.close() → release a db.open() handle now (idempotent; __gc is the
 * backstop). The owner box rides as upvalue 1. */
static int lua_owned_conn_close(lua_State *L)
{
    if (g_db_row_loops > 0)
        return luaL_error(L, "db: close() while a query's rows are being read "
                             "(from a __gc finalizer?)");
    owned_conn_release(luaL_testudata(L, lua_upvalueindex(1),
                                      HL_LUA_DB_OWNED_MT));
    return 0;
}

/* Push an empty owner box (h = NULL, dsn = NULL) with its __gc installed. It
 * is made BEFORE the handle is opened (audit 24): a LUA_ERRMEM from
 * lua_newuserdatauv after hl_db_dynamic_open leaked the handle - one of the
 * HL_DB_DYNAMIC_MAX slots - for the life of the process. The __gc releases a
 * NULL handle as a no-op. */
static HlLuaOwnedConn *push_owned_box(lua_State *L)
{
    HlLuaOwnedConn *o = lua_newuserdatauv(L, sizeof *o, 0);
    o->h = NULL;
    o->dsn = NULL;
    luaL_setmetatable(L, HL_LUA_DB_OWNED_MT);   /* installs __gc */
    return o;
}

/* Turn the owner box @p o at the top of the stack (holding a handle from
 * hl_db_dynamic_open, opened from @p dsn) into a caller-owned connection
 * object. Unlike push_conn_object, this object owns its handle: it adds a
 * close() method and a __gc finalizer that both release it via
 * hl_db_dynamic_close. It carries sync methods + async (bound to the owner box,
 * so conn.async targets @p dsn through the worker pool). udf is intentionally
 * absent: worker-side udf re-registration is keyed off the registry a dynamic
 * handle is not in (tracked follow-up, §2.2). */
static int push_owned_conn_object(lua_State *L, HlLuaOwnedConn *o,
                                  const char *dsn)
{
    int owner = lua_gettop(L);
    HlDbHandle *h = o->h;
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
    if (hl_db_registry_is_closed(lua->base.db_registry))   /* audit 17 */
        return luaL_error(L, "db.open: database connections are closed "
                             "(shutting down)");
    const HlManifestDbDynamic *policy =
        hl_db_registry_dynamic_policy(lua->base.db_registry);
    const char *err = NULL;
    /* The DSN actually opened (a file DSN resolves under the app directory):
     * conn.async opens its worker connection from this, so both name the same
     * database. */
    char opened[HL_DB_DYNAMIC_DSN_MAX];
    HlLuaOwnedConn *o = push_owned_box(L);
    /* Nothing allocates on the Lua heap between the open and o->h. The
     * path checks and the open's fixed work are charged to the run as they
     * go (audit 39); a trip there, or in SQLite's allocator during the open
     * (the budget bound on this thread), raises the run's limit rather
     * than "connection failed". */
    HlFsChargeScope cs = hl_fs_charge_begin(hl_lua_fs_charge, L);
    HlDbHandle *h = hl_db_dynamic_open_ex(dsn, policy, lua->base.fs_cfg,
                                          opened, sizeof opened, &err);
    int tripped = hl_fs_charge_end(cs) || hl_lua_budget_tripped(L);
    if (tripped) {
        if (h) hl_db_dynamic_close(h);
        return hl_lua_budget_raise(L);
    }
    if (!h)
        return luaL_error(L, "%s", err ? err : "db.open: denied");
    o->h = h;
    return push_owned_conn_object(L, o, opened);
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

/* hull.db._internal_conn.connection() -> (conn, final)
 *
 * The connection the stdlib keeps its _hull_* tables on (manifest
 * `databases.internal`, else the default one). Stdlib-only: the module name's
 * underscore segment keeps it out of app code's reach. `final` is true once
 * the manifest is wired; before that (a stdlib init() at app top level) the
 * DSN is read from the app's manifest table on the spot, and the caller (the
 * hull.db._internal proxy) must not cache the result. */
static int lua_db_internal_conn_impl(lua_State *L, int app_table)
{
    HlLua *lua = get_hl_lua(L);
    HlDbRegistry *reg = lua ? lua->base.db_registry : NULL;
    if (!reg) {
        push_conn_object(L, NULL);
        lua_pushboolean(L, 1);
        return 2;
    }
    int final = hl_db_registry_manifest_wired(reg);
    if (!final) {
        HlManifest m;
        memset(&m, 0, sizeof m);
        char ref_err[256] = {0};
        int ref_bad = 0;
        if (hl_manifest_extract_lua(L, &m, lua->base.alloc) == 0) {
            /* A "$VAR" here is read from the environment by the registry, so
             * it must be declared (env / secrets) BEFORE it is used: the
             * load-time hl_manifest_check_env_refs runs only after the top
             * level, and this runs during it (audit 17). */
            if (hl_manifest_check_env_refs(&m, ref_err, sizeof ref_err) != 0) {
                ref_bad = 1;
                (void)hl_db_registry_set_internal_dsn(reg, NULL);
            } else if (hl_db_registry_set_internal_dsn(reg, m.databases.internal) != 0) {
                log_warn("[hull:db] databases.internal is too long; ignored");
            }
        }
        hl_manifest_free(&m);
        if (ref_bad)
            return luaL_error(L, "hull internal database: %s", ref_err);
    }
    const char *err = NULL;
    HlDbHandle *h = app_table ? hl_db_registry_internal_or_app(reg, &err)
                              : hl_db_registry_internal(reg, &err);
    if (!h && hl_db_registry_has_internal(reg))
        return luaL_error(L, "hull internal database: %s",
                          err ? err : "cannot open databases.internal");
    push_conn_object(L, h);
    lua_pushboolean(L, final);
    return 2;
}

static int lua_db_internal_conn(lua_State *L)
{
    return lua_db_internal_conn_impl(L, 0);
}

/* hull.db._internal_conn.app_connection() -> (conn, final): where an APP
 * table the stdlib reads for the app lives (auth-flows' standard_users): a
 * declared databases.internal, else the default connection - never the
 * Hull-owned default internal database (§7.2). */
static int lua_db_internal_app_conn(lua_State *L)
{
    return lua_db_internal_conn_impl(L, 1);
}

int luaopen_hull_db_internal_conn(lua_State *L)
{
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, lua_db_internal_conn);
    lua_setfield(L, -2, "connection");
    lua_pushcfunction(L, lua_db_internal_app_conn);
    lua_setfield(L, -2, "app_connection");
    return 1;
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
