/* mod_worker.c — hull.worker module: thread pool dispatch
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "internal.h"
#include "hull/async.h"
#include "hull/net_backend.h"

#include <keel/server.h>

#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.worker module
 *
 * worker.dispatch(fn, ctx) — dispatch fn to a worker thread
 * ════════════════════════════════════════════════════════════════════ */

/* Deep-copy a Lua table (string keys only, flat) to an HlKV array.
 * Caller owns the returned array (free with hl_kv_free). */
static int lua_table_to_kv(lua_State *L, int idx, HlKV **out_kvs, int *out_count)
{
    *out_kvs = NULL;
    *out_count = 0;

    if (lua_isnoneornil(L, idx))
        return 0;

    luaL_checktype(L, idx, LUA_TTABLE);

    /* Count string keys */
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) count++;
        lua_pop(L, 1);
    }
    if (count == 0)
        return 0;

    HlKV *kvs = calloc((size_t)count, sizeof(HlKV));
    if (!kvs)
        return -1;

    int i = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }
        kvs[i].key = strdup(lua_tostring(L, -2));
        if (!kvs[i].key) {
            /* OOM on key dup: free everything copied so far and bail (M-2). */
            lua_pop(L, 2);
            hl_kv_free(kvs, i);
            return -1;
        }
        int vt = lua_type(L, -1);
        switch (vt) {
        case LUA_TBOOLEAN:
            kvs[i].value.type = HL_TYPE_BOOL;
            kvs[i].value.b = lua_toboolean(L, -1);
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1)) {
                kvs[i].value.type = HL_TYPE_INT;
                kvs[i].value.i = (int64_t)lua_tointeger(L, -1);
            } else {
                kvs[i].value.type = HL_TYPE_DOUBLE;
                kvs[i].value.d = (double)lua_tonumber(L, -1);
            }
            break;
        case LUA_TSTRING: {
            size_t slen;
            const char *sv = lua_tolstring(L, -1, &slen);
            kvs[i].value.type = HL_TYPE_TEXT;
            kvs[i].value.s = malloc(slen + 1);
            if (!kvs[i].value.s) {
                /* OOM on string dup: include this kv (with key + nil value
                 * slot) in the free count so the key is also released. */
                kvs[i].value.type = HL_TYPE_NIL;
                lua_pop(L, 2);
                hl_kv_free(kvs, i + 1);
                return -1;
            }
            memcpy((void *)kvs[i].value.s, sv, slen);
            ((char *)kvs[i].value.s)[slen] = '\0';
            kvs[i].value.len = slen;
            break;
        }
        default:
            kvs[i].value.type = HL_TYPE_NIL;
            break;
        }
        i++;
        lua_pop(L, 1);
    }

    *out_kvs = kvs;
    *out_count = i;
    return 0;
}

/* Bytecode writer callback for lua_dump */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} LuaBytecodeWriter;

static int lua_bytecode_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    LuaBytecodeWriter *bw = (LuaBytecodeWriter *)ud;
    if (bw->len + sz > bw->cap) {
        size_t new_cap = bw->cap ? bw->cap * 2 : 4096;
        while (new_cap < bw->len + sz) {
            if (new_cap > SIZE_MAX / 2) return 1;
            new_cap *= 2;
        }
        uint8_t *nb = realloc(bw->buf, new_cap);
        if (!nb) return 1;
        bw->buf = nb;
        bw->cap = new_cap;
    }
    memcpy(bw->buf + bw->len, p, sz);
    bw->len += sz;
    return 0;
}

/* push_result callback: convert HlLuaWorkerDispatchOp result to Lua value */
static void lua_push_worker_dispatch_result(lua_State *L, void *driver)
{
    HlLuaWorkerDispatchOp *op = (HlLuaWorkerDispatchOp *)driver;

    if (op->error) {
        lua_newtable(L);
        lua_pushstring(L, op->error_msg);
        lua_setfield(L, -2, "error");
        return;
    }

    switch (op->result_kind) {
    case 0: /* nil */
        lua_pushnil(L);
        break;
    case 1: /* bool */
        lua_pushboolean(L, op->result_bool);
        break;
    case 2: /* int */
        lua_pushinteger(L, (lua_Integer)op->result_int);
        break;
    case 3: /* double */
        lua_pushnumber(L, (lua_Number)op->result_double);
        break;
    case 4: /* string */
        lua_pushlstring(L, op->result_str, op->result_str_len);
        break;
    case 5: /* table */
        lua_createtable(L, 0, op->result_count);
        for (int i = 0; i < op->result_count; i++) {
            switch (op->result_kvs[i].value.type) {
            case HL_TYPE_INT:
                lua_pushinteger(L, (lua_Integer)op->result_kvs[i].value.i);
                break;
            case HL_TYPE_DOUBLE:
                lua_pushnumber(L, (lua_Number)op->result_kvs[i].value.d);
                break;
            case HL_TYPE_TEXT:
                lua_pushlstring(L, op->result_kvs[i].value.s,
                                op->result_kvs[i].value.len);
                break;
            case HL_TYPE_BOOL:
                lua_pushboolean(L, op->result_kvs[i].value.b);
                break;
            default:
                lua_pushnil(L);
                break;
            }
            lua_setfield(L, -2, op->result_kvs[i].key);
        }
        break;
    default:
        lua_pushnil(L);
        break;
    }
}

/* worker.dispatch(fn, ctx) — serialize fn + ctx, submit to thread pool */
static int lua_worker_dispatch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "worker.dispatch not available (no thread pool)");
    if (!lua->base.async_ctx)
        return luaL_error(L, "worker.dispatch requires an active event loop");

    luaL_checktype(L, 1, LUA_TFUNCTION);

    /* Serialize function to bytecode via lua_dump */
    LuaBytecodeWriter bw = {0};
    lua_pushvalue(L, 1); /* push function copy for dump */
    int dump_rc = lua_dump(L, lua_bytecode_writer, &bw, 0);
    lua_pop(L, 1); /* pop function copy */
    if (dump_rc != 0 || !bw.buf) {
        free(bw.buf);
        return luaL_error(L, "worker.dispatch: cannot serialize function (C functions not allowed)");
    }

    /* Deep-copy ctx table */
    HlKV *ctx_kvs = NULL;
    int ctx_count = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (lua_table_to_kv(L, 2, &ctx_kvs, &ctx_count) != 0) {
            free(bw.buf);
            return luaL_error(L, "worker.dispatch: failed to serialize ctx table");
        }
    }

    /* Allocate dispatch op */
    HlLuaWorkerDispatchOp *op = calloc(1, sizeof(HlLuaWorkerDispatchOp));
    if (!op) {
        free(bw.buf);
        hl_kv_free(ctx_kvs, ctx_count);
        return luaL_error(L, "worker.dispatch: out of memory");
    }

    op->server = lua->server;
    op->alloc = lua->base.alloc;
    op->bytecode = bw.buf;
    op->bytecode_len = bw.len;
    op->ctx_kvs = ctx_kvs;
    op->ctx_count = ctx_count;

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(lua->server, lua->base.net_ctx, lua->base.alloc);
    if (!actx) {
        hl_lua_worker_dispatch_op_free(op);
        free(op);
        return luaL_error(L, "worker.dispatch: out of memory");
    }

    /* Create Lua continuation */
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_worker_dispatch_result);
    if (!cont) {
        hl_lua_worker_dispatch_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return luaL_error(L, "worker.dispatch: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_lua_worker_dispatch_op_free_all;
    actx->op.on_cancel = hl_lua_worker_dispatch_cancel;
    actx->detached = (lua->active_conn == NULL);

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_lua_worker_dispatch_submit(lua->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_lua_worker_dispatch_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return luaL_error(L, "worker.dispatch: thread pool full");
    }

    /* Suspend the FD (attached only). */
    if (!actx->detached &&
        hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)lua->active_conn, (HlSuspendOp *)&actx->op) < 0) {
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        return luaL_error(L, "worker.dispatch: failed to suspend connection");
    }

    return lua_yieldk(L, 0, 0, NULL);
}

static const luaL_Reg worker_funcs[] = {
    {"dispatch", lua_worker_dispatch},
    {NULL, NULL}
};

int luaopen_hull_worker(lua_State *L)
{
    luaL_newlib(L, worker_funcs);
    return 1;
}
