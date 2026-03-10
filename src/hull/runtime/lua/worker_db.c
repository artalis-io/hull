/*
 * lua_worker_db.c — Bridge: db.* for worker Lua VMs
 *
 * Registers sync db.query/exec/batch into per-worker Lua VMs using
 * the worker's TLS SQLite connection. This is the ONLY file that
 * combines Lua + DB concerns.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "hull/worker_db.h"
#include "hull/cap/db.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <sqlite3.h>
#include <string.h>

/* ── Bind params helper (simplified for worker VMs) ─────────────── */

static int worker_lua_bind_params(sqlite3_stmt *stmt, lua_State *L, int idx)
{
    if (!lua_istable(L, idx)) return 0;
    int len = (int)luaL_len(L, idx);
    for (int i = 0; i < len; i++) {
        lua_rawgeti(L, idx, i + 1);
        int t = lua_type(L, -1);
        int rc;
        int pidx = i + 1;
        switch (t) {
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1))
                rc = sqlite3_bind_int64(stmt, pidx, (sqlite3_int64)lua_tointeger(L, -1));
            else
                rc = sqlite3_bind_double(stmt, pidx, lua_tonumber(L, -1));
            break;
        case LUA_TSTRING: {
            size_t slen;
            const char *s = lua_tolstring(L, -1, &slen);
            rc = sqlite3_bind_text(stmt, pidx, s, (int)slen, SQLITE_TRANSIENT);
            break;
        }
        case LUA_TBOOLEAN:
            rc = sqlite3_bind_int(stmt, pidx, lua_toboolean(L, -1) ? 1 : 0);
            break;
        default:
            rc = sqlite3_bind_null(stmt, pidx);
            break;
        }
        lua_pop(L, 1);
        if (rc != SQLITE_OK) return -1;
    }
    return 0;
}

/* ── db.query for worker VMs ────────────────────────────────────── */

static int worker_lua_db_query(lua_State *L)
{
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return luaL_error(L, "database not available in worker");

    const char *sql = luaL_checkstring(L, 1);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(wdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return luaL_error(L, "prepare: %s", sqlite3_errmsg(wdb->db));

    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        if (worker_lua_bind_params(stmt, L, 2) != 0) {
            sqlite3_finalize(stmt);
            return luaL_error(L, "bind: %s", sqlite3_errmsg(wdb->db));
        }
    }

    lua_newtable(L);
    int table_idx = lua_gettop(L);
    int row_num = 0;

    int ncols = 0;
    int first = 1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (first) {
            ncols = sqlite3_column_count(stmt);
            first = 0;
        }
        row_num++;
        lua_createtable(L, 0, ncols);
        for (int i = 0; i < ncols; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_INTEGER:
                lua_pushinteger(L, (lua_Integer)sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                lua_pushnumber(L, sqlite3_column_double(stmt, i));
                break;
            case SQLITE_TEXT:
                lua_pushlstring(L,
                    (const char *)sqlite3_column_text(stmt, i),
                    (size_t)sqlite3_column_bytes(stmt, i));
                break;
            case SQLITE_NULL:
            default:
                lua_pushnil(L);
                break;
            }
            lua_setfield(L, -2, name ? name : "?");
        }
        lua_rawseti(L, table_idx, row_num);
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lua_pop(L, 1); /* pop table */
        return luaL_error(L, "query: %s", sqlite3_errmsg(wdb->db));
    }

    return 1;
}

/* ── db.exec for worker VMs ──────────────────────────────────────── */

static int worker_lua_db_exec(lua_State *L)
{
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return luaL_error(L, "database not available in worker");

    const char *sql = luaL_checkstring(L, 1);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(wdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return luaL_error(L, "prepare: %s", sqlite3_errmsg(wdb->db));

    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        if (worker_lua_bind_params(stmt, L, 2) != 0) {
            sqlite3_finalize(stmt);
            return luaL_error(L, "bind: %s", sqlite3_errmsg(wdb->db));
        }
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
        return luaL_error(L, "exec: %s", sqlite3_errmsg(wdb->db));

    lua_pushinteger(L, sqlite3_changes(wdb->db));
    return 1;
}

/* ── db.batch for worker VMs ─────────────────────────────────────── */

static int worker_lua_db_batch(lua_State *L)
{
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return luaL_error(L, "database not available in worker");

    luaL_checktype(L, 1, LUA_TFUNCTION);

    if (hl_cap_db_begin(wdb->db) != 0)
        return luaL_error(L, "BEGIN failed: %s", sqlite3_errmsg(wdb->db));

    lua_pushvalue(L, 1);
    int rc = lua_pcall(L, 0, LUA_MULTRET, 0);

    if (rc != LUA_OK) {
        hl_cap_db_rollback(wdb->db);
        return lua_error(L);
    }

    if (hl_cap_db_commit(wdb->db) != 0) {
        hl_cap_db_rollback(wdb->db);
        return luaL_error(L, "COMMIT failed: %s", sqlite3_errmsg(wdb->db));
    }

    return lua_gettop(L) - 1; /* return whatever fn returned */
}

/* ── db.last_id for worker VMs ───────────────────────────────────── */

static int worker_lua_db_last_id(lua_State *L)
{
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return luaL_error(L, "database not available in worker");
    lua_pushinteger(L, (lua_Integer)hl_cap_db_last_id(wdb->db));
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
