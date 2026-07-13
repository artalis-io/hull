/*
 * lua_worker_db.c — Bridge: db.* for worker Lua VMs
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
#include "hull/worker_db.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

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

    int len = (int)luaL_len(L, idx);
    if (len <= 0)
        return 0;
    if ((size_t)len > SIZE_MAX / sizeof(HlValue))
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
} WorkerLuaQueryCtx;

static int worker_lua_row_cb(void *opaque, HlColumn *cols, int ncols)
{
    WorkerLuaQueryCtx *qc = (WorkerLuaQueryCtx *)opaque;
    qc->row_count++;

    if (!lua_checkstack(qc->L, ncols + 2))
        return -1;
    lua_createtable(qc->L, 0, ncols);
    for (int i = 0; i < ncols; i++) {
        switch (cols[i].value.type) {
        case HL_TYPE_INT:
            lua_pushinteger(qc->L, (lua_Integer)cols[i].value.i);
            break;
        case HL_TYPE_DOUBLE:
            lua_pushnumber(qc->L, (lua_Number)cols[i].value.d);
            break;
        case HL_TYPE_TEXT:
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
        lua_setfield(qc->L, -2, cols[i].name ? cols[i].name : "?");
    }
    lua_rawseti(qc->L, qc->table_idx, qc->row_count);
    return 0;
}

/* ── db.query for worker VMs ────────────────────────────────────── */

static int worker_lua_db_query(lua_State *L)
{
    const char *sql = luaL_checkstring(L, 1);
    const char *err = NULL;
    HlDbHandle *h = hl_worker_db_handle_checked(sql, &err);
    if (!h)
        return luaL_error(L, "%s", err ? err : "worker db");

    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (worker_lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    lua_newtable(L);
    int table_idx = lua_gettop(L);
    WorkerLuaQueryCtx qc = { .L = L, .table_idx = table_idx, .row_count = 0 };

    int rc = hl_db_query(h, sql, params, nparams, worker_lua_row_cb, &qc, NULL);

    /* Rotate the result table below the aliased param values so the pop
     * removes params, not the result (same discipline as the sync path). */
    if (nparams > 0) {
        lua_rotate(L, table_idx - nparams, 1);
        lua_pop(L, nparams);
    }
    free(params);

    if (rc != 0) {
        lua_pop(L, 1); /* pop result table */
        return luaL_error(L, "query: %s", hl_db_errmsg(h));
    }
    return 1;
}

/* ── db.exec for worker VMs ──────────────────────────────────────── */

static int worker_lua_db_exec(lua_State *L)
{
    const char *sql = luaL_checkstring(L, 1);
    const char *err = NULL;
    HlDbHandle *h = hl_worker_db_handle_checked(sql, &err);
    if (!h)
        return luaL_error(L, "%s", err ? err : "worker db");

    HlValue *params = NULL;
    int nparams = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (worker_lua_to_hl_values(L, 2, &params, &nparams) != 0)
            return luaL_error(L, "params must be a table");
    }

    int rc = hl_db_exec(h, sql, params, nparams);

    if (nparams > 0)
        lua_pop(L, nparams);
    free(params);

    if (rc < 0)
        return luaL_error(L, "exec: %s", hl_db_errmsg(h));

    lua_pushinteger(L, rc);
    return 1;
}

/* ── db.batch for worker VMs ─────────────────────────────────────── */

static int worker_lua_db_batch(lua_State *L)
{
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->handle.backend)
        return luaL_error(L, "database not available in worker");
    HlDbHandle *h = &wdb->handle;

    luaL_checktype(L, 1, LUA_TFUNCTION);

    if (hl_db_begin(h) != 0)
        return luaL_error(L, "BEGIN failed: %s", hl_db_errmsg(h));

    lua_pushvalue(L, 1);
    int rc = lua_pcall(L, 0, LUA_MULTRET, 0);

    if (rc != LUA_OK) {
        hl_db_rollback(h);
        return lua_error(L);
    }

    if (hl_db_commit(h) != 0) {
        hl_db_rollback(h);
        return luaL_error(L, "COMMIT failed: %s", hl_db_errmsg(h));
    }

    return lua_gettop(L) - 1; /* return whatever fn returned */
}

/* ── db.last_id for worker VMs ───────────────────────────────────── */

static int worker_lua_db_last_id(lua_State *L)
{
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
