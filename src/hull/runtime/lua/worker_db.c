/*
 * lua_worker_db.c - Bridge: db.* for worker Lua VMs
 *
 * Registers sync db.query/exec/batch into per-worker Lua VMs using the
 * worker thread's own backend connection (HlDbBackend vtable), so the
 * path is transparent across SQLite and PostgreSQL. This is the ONLY
 * file that combines Lua + DB concerns for worker VMs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/runtime/lua.h"
#include "internal.h"
#include "protected.h"
#include "hull/worker_db.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ── Param marshalling (Lua table → HlValue[]) ──────────────────── */

/* Marshal a Lua array table at @p idx into a malloc'd HlValue[]. String
 * values alias the Lua stack entries (left pushed), so the array stays
 * valid until the caller pops @p *out_count values. Returns 0 / -1. */
static int worker_lua_to_hl_values(lua_State *L, int idx,
                                   HlValue **out_params, int *out_count)
{
    *out_params = NULL;
    *out_count = 0;

    if (lua_isnoneornil(L, idx))
        return 0;
    if (!lua_istable(L, idx))
        return -1;

    /* Raw, as mod_db.c reads params (audit 9 L4): luaL_len ran the table's
     * __len, app code inside this marshalling, with any result it liked. */
    lua_Unsigned rl = lua_rawlen(L, idx);
    if (rl == 0)
        return 0;
    if (rl > (lua_Unsigned)INT_MAX)
        return -1;
    int len = (int)rl;
    if ((size_t)len > SIZE_MAX / sizeof(HlValue))
        return -1;

    /* Every value stays on the stack: room for all of them first. */
    if (!lua_checkstack(L, len))
        return -1;

    HlValue *params = calloc((size_t)len, sizeof(HlValue));
    if (!params)
        return -1;

    for (int i = 0; i < len; i++) {
        lua_rawgeti(L, idx, i + 1); /* Lua tables are 1-based */
        switch (lua_type(L, -1)) {
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
            params[i].s = s; /* valid while left on the Lua stack */
            params[i].len = slen;
            break;
        }
        case LUA_TBOOLEAN:
            params[i].type = HL_TYPE_BOOL;
            params[i].b = lua_toboolean(L, -1);
            break;
        default:
            params[i].type = HL_TYPE_NIL;
            break;
        }
        /* Leave value on the stack; keeps string pointers alive. */
    }

    *out_params = params;
    *out_count = len;
    return 0;
}

/* ── Row callback (backend-agnostic) ────────────────────────────── */

typedef struct {
    lua_State *L;
    int        table_idx;
    int        row_count;
    int        failed;    /* a row could not be built (out of memory) */
} WorkerLuaQueryCtx;

/* Runs inside the backend's read loop, so it must not raise: see
 * lua_query_row_cb in mod_db.c. */
static int worker_lua_row_cb(void *opaque, HlColumn *cols, int ncols)
{
    WorkerLuaQueryCtx *qc = (WorkerLuaQueryCtx *)opaque;
    if (hl_lua_append_row(qc->L, qc->table_idx, (lua_Integer)qc->row_count + 1,
                          cols, ncols) != 0) {
        qc->failed = 1;
        return 1;
    }
    qc->row_count++;
    return 0;
}

/* ── Re-entry guard ──────────────────────────────────────────────── */

/* Same hazard as the event-loop mod_db: a __gc finalizer (a table with a
 * __gc metamethod is enough) can run during a row loop or a UDF step and
 * call db again, and the statement cache hands the same SQL back as the
 * statement being stepped - resetting it under the loop. While a statement
 * runs on this thread, db calls refuse. A to-be-closed guard brings the
 * count back down even when an error unwinds out of the loop. */
static _Thread_local int t_row_loops;

static int row_loop_guard_close(lua_State *L)
{
    (void)L;
    if (t_row_loops > 0) t_row_loops--;
    return 0;
}

static int push_row_loop_guard(lua_State *L)
{
    lua_newuserdatauv(L, 1, 0);
    if (luaL_newmetatable(L, "hull.worker.db.rowloop")) {
        lua_pushcfunction(L, row_loop_guard_close);
        lua_setfield(L, -2, "__close");
    }
    lua_setmetatable(L, -2);
    t_row_loops++;
    lua_toclose(L, -1);
    return lua_gettop(L);
}

static void refuse_reentry(lua_State *L)
{
    if (t_row_loops > 0)
        luaL_error(L, "db: called while a query's rows are being read "
                      "(from a __gc finalizer?)");
}

/* ── db.query for worker VMs ────────────────────────────────────── */

static int worker_lua_db_query(lua_State *L)
{
    refuse_reentry(L);
    const char *sql = luaL_checkstring(L, 1);
    const char *err = NULL;
    HlDbHandle *h = hl_worker_db_handle_checked(sql, &err);
    if (!h)
        return luaL_error(L, "%s", err ? err : "worker db");

    /* (sql, params) exactly: slot 2 is the params or nil, never a value
     * pushed below - with params omitted, the table / guard pushed next
     * would sit there and be read as them. */
    lua_settop(L, 2);
    /* The result table and the guard come FIRST, the calloc'd params last:
     * both allocate and can raise, and raised after the calloc they leaked
     * it. Nothing between the conversion and the free below can raise. */
    lua_newtable(L);
    int table_idx = lua_gettop(L);
    WorkerLuaQueryCtx qc = { .L = L, .table_idx = table_idx, .row_count = 0 };
    int guard = push_row_loop_guard(L);

    HlValue *params = NULL;
    int nparams = 0;
    if (!lua_isnoneornil(L, 2)) {
        if (worker_lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    int rc = hl_db_query(h, sql, params, nparams, worker_lua_row_cb, &qc, NULL);
    free(params);
    /* Closes the guard and drops the param values above it; the result
     * table is on top. */
    lua_settop(L, guard - 1);

    if (qc.failed)
        return luaL_error(L, "query: not enough memory for the result");
    if (rc != 0) {
        lua_pop(L, 1); /* pop result table */
        return hl_lua_raise_copy(L, "query: ", hl_db_errmsg(h));
    }
    return 1;
}

/* ── db.exec for worker VMs ──────────────────────────────────────── */

static int worker_lua_db_exec(lua_State *L)
{
    refuse_reentry(L);
    const char *sql = luaL_checkstring(L, 1);
    const char *err = NULL;
    HlDbHandle *h = hl_worker_db_handle_checked(sql, &err);
    if (!h)
        return luaL_error(L, "%s", err ? err : "worker db");

    lua_settop(L, 2);   /* (sql, params): see worker_lua_db_query */
    /* The guard first, the calloc'd params last: the guard allocates and
     * can raise, and raised after the calloc it leaked it. */
    int guard = push_row_loop_guard(L);   /* a UDF steps inside exec */

    HlValue *params = NULL;
    int nparams = 0;
    if (!lua_isnoneornil(L, 2)) {
        if (worker_lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    int rc = hl_db_exec(h, sql, params, nparams);
    free(params);
    lua_settop(L, guard - 1);   /* closes the guard, drops the param values */

    if (rc < 0)
        return hl_lua_raise_copy(L, "exec: ", hl_db_errmsg(h));

    lua_pushinteger(L, rc);
    return 1;
}

/* ── db.batch for worker VMs ─────────────────────────────────────── */

static int worker_lua_db_batch(lua_State *L)
{
    refuse_reentry(L);
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->handle.backend)
        return luaL_error(L, "database not available in worker");
    HlDbHandle *h = &wdb->handle;

    luaL_checktype(L, 1, LUA_TFUNCTION);

    /* The event loop's batch machinery (mod_db.c): a nested batch is a
     * savepoint and a transaction ended under the batch fails it. Raw
     * BEGIN / COMMIT here let a nested batch commit the outer's writes on
     * Postgres / MySQL and a raising outer batch stay committed (audit 8
     * c_db M2). h is the thread's default connection, which fn cannot
     * close, so it stays valid across the call. */
    if (hl_db_batch_enter(h) != 0)
        return hl_lua_raise_copy(L, "BEGIN failed: ", hl_db_errmsg(h));

    lua_pushvalue(L, 1);
    int rc = lua_pcall(L, 0, LUA_MULTRET, 0);

    if (rc != LUA_OK) {
        (void)hl_db_batch_leave(h, 0);
        return lua_error(L);
    }

    if (hl_db_batch_leave(h, 1) != 0)
        return hl_lua_raise_copy(L, "COMMIT failed: ", hl_db_errmsg(h));

    return lua_gettop(L) - 1; /* return whatever fn returned */
}

/* ── db.last_id for worker VMs ───────────────────────────────────── */

static int worker_lua_db_last_id(lua_State *L)
{
    refuse_reentry(L);
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->handle.backend)
        return luaL_error(L, "database not available in worker");
    lua_pushinteger(L, (lua_Integer)hl_db_last_id(&wdb->handle));
    return 1;
}

/* ── Init hook ──────────────────────────────────────────────────── */

static int lua_worker_db_init_hook(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"query",   worker_lua_db_query},
        {"exec",    worker_lua_db_exec},
        {"batch",   worker_lua_db_batch},
        {"last_id", worker_lua_db_last_id},
        {NULL, NULL}
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "db");
    return 0;
}

void hl_lua_worker_db_init(void)
{
    hl_lua_worker_register_init(lua_worker_db_init_hook);
}

#endif /* HL_ENABLE_DB */
