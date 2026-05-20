/*
 * lua_worker.c — Per-worker Lua VMs for worker.dispatch()
 *
 * Manages TLS-keyed Lua VMs on worker threads. Each worker gets a
 * minimal Lua VM (base, string, table, math, utf8) with capabilities
 * registered via init hooks (e.g. db.* from lua/worker_db.c).
 *
 * Zero DB knowledge — capabilities are plugged in via hooks.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "internal.h"
#include "hull/async.h"
#include "hull/async_backend.h"
#include "hull/net_backend.h"
#include "hull/alloc.h"

#include <keel/thread_pool.h>
#include <keel/async.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "log.h"

/* ── Init hooks ────────────────────────────────────────────────────── */

#define MAX_WORKER_INIT_HOOKS 8

static HlLuaWorkerInitFn init_hooks[MAX_WORKER_INIT_HOOKS];
static int               init_hook_count;

void hl_lua_worker_register_init(HlLuaWorkerInitFn fn)
{
    if (init_hook_count < MAX_WORKER_INIT_HOOKS)
        init_hooks[init_hook_count++] = fn;
}

/* ── Per-worker Lua VM (TLS, lazy init) ────────────────────────────── */

typedef struct {
    lua_State *L;
} HlLuaWorkerCtx;

static pthread_key_t  lua_worker_key;
static pthread_once_t lua_worker_once = PTHREAD_ONCE_INIT;

static void lua_worker_destructor(void *ptr)
{
    if (!ptr) return;
    HlLuaWorkerCtx *wctx = (HlLuaWorkerCtx *)ptr;
    if (wctx->L) lua_close(wctx->L);
    free(wctx);
}

static void lua_worker_key_create(void)
{
    pthread_key_create(&lua_worker_key, lua_worker_destructor);
}

static HlLuaWorkerCtx *get_lua_worker_ctx(void)
{
    pthread_once(&lua_worker_once, lua_worker_key_create);

    HlLuaWorkerCtx *wctx = (HlLuaWorkerCtx *)pthread_getspecific(lua_worker_key);
    if (wctx) return wctx;

    wctx = calloc(1, sizeof(HlLuaWorkerCtx));
    if (!wctx) return NULL;

    wctx->L = luaL_newstate();
    if (!wctx->L) {
        free(wctx);
        return NULL;
    }

    /* Open minimal standard libraries */
    luaL_requiref(wctx->L, "_G", luaopen_base, 1);
    lua_pop(wctx->L, 1);
    luaL_requiref(wctx->L, "string", luaopen_string, 1);
    lua_pop(wctx->L, 1);
    luaL_requiref(wctx->L, "table", luaopen_table, 1);
    lua_pop(wctx->L, 1);
    luaL_requiref(wctx->L, "math", luaopen_math, 1);
    lua_pop(wctx->L, 1);
    luaL_requiref(wctx->L, "utf8", luaopen_utf8, 1);
    lua_pop(wctx->L, 1);

    /* Remove dangerous functions from minimal VM */
    lua_pushnil(wctx->L); lua_setglobal(wctx->L, "dofile");
    lua_pushnil(wctx->L); lua_setglobal(wctx->L, "loadfile");
    lua_pushnil(wctx->L); lua_setglobal(wctx->L, "load");
    lua_pushnil(wctx->L); lua_setglobal(wctx->L, "print");
    lua_pushnil(wctx->L); lua_setglobal(wctx->L, "io");
    lua_pushnil(wctx->L); lua_setglobal(wctx->L, "os");
    lua_pushnil(wctx->L); lua_setglobal(wctx->L, "require");

    /* Run registered init hooks (e.g. db.*, json.*) */
    for (int i = 0; i < init_hook_count; i++) {
        if (init_hooks[i](wctx->L) != 0) {
            log_error("[hull:worker] init hook %d failed", i);
            lua_close(wctx->L);
            free(wctx);
            return NULL;
        }
    }

    pthread_setspecific(lua_worker_key, wctx);
    return wctx;
}

/* ── KV helpers: Lua table ↔ HlKV array ───────────────────────────── */

static void push_kv_table(lua_State *L, const HlKV *kvs, int count)
{
    lua_createtable(L, 0, count);
    for (int i = 0; i < count; i++) {
        switch (kvs[i].value.type) {
        case HL_TYPE_INT:
            lua_pushinteger(L, (lua_Integer)kvs[i].value.i);
            break;
        case HL_TYPE_DOUBLE:
            lua_pushnumber(L, kvs[i].value.d);
            break;
        case HL_TYPE_TEXT:
            lua_pushlstring(L, kvs[i].value.s, kvs[i].value.len);
            break;
        case HL_TYPE_BOOL:
            lua_pushboolean(L, kvs[i].value.b);
            break;
        case HL_TYPE_NIL:
        default:
            lua_pushnil(L);
            break;
        }
        lua_setfield(L, -2, kvs[i].key);
    }
}

/* Capture the Lua value at stack top into the dispatch op result fields.
 * Returns 0 on success. */
static int capture_result(lua_State *L, HlLuaWorkerDispatchOp *op)
{
    int t = lua_type(L, -1);
    switch (t) {
    case LUA_TNIL:
        op->result_kind = 0;
        break;
    case LUA_TBOOLEAN:
        op->result_kind = 1;
        op->result_bool = lua_toboolean(L, -1);
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, -1)) {
            op->result_kind = 2;
            op->result_int = (int64_t)lua_tointeger(L, -1);
        } else {
            op->result_kind = 3;
            op->result_double = (double)lua_tonumber(L, -1);
        }
        break;
    case LUA_TSTRING: {
        op->result_kind = 4;
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        op->result_str = malloc(len + 1);
        if (!op->result_str) return -1;
        memcpy(op->result_str, s, len);
        op->result_str[len] = '\0';
        op->result_str_len = len;
        break;
    }
    case LUA_TTABLE: {
        op->result_kind = 5;
        /* Count string keys */
        int count = 0;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) count++;
            lua_pop(L, 1);
        }
        if (count == 0) {
            op->result_kvs = NULL;
            op->result_count = 0;
            break;
        }
        op->result_kvs = calloc((size_t)count, sizeof(HlKV));
        if (!op->result_kvs) return -1;
        op->result_count = 0;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 1);
                continue;
            }
            int idx = op->result_count;
            const char *key = lua_tostring(L, -2);
            op->result_kvs[idx].key = strdup(key);
            if (!op->result_kvs[idx].key) {
                /* OOM on key dup: leave partial state — caller (op_free)
                 * will release everything we've copied so far (M-2). */
                lua_pop(L, 2);
                return -1;
            }

            int vt = lua_type(L, -1);
            switch (vt) {
            case LUA_TBOOLEAN:
                op->result_kvs[idx].value.type = HL_TYPE_BOOL;
                op->result_kvs[idx].value.b = lua_toboolean(L, -1);
                break;
            case LUA_TNUMBER:
                if (lua_isinteger(L, -1)) {
                    op->result_kvs[idx].value.type = HL_TYPE_INT;
                    op->result_kvs[idx].value.i = (int64_t)lua_tointeger(L, -1);
                } else {
                    op->result_kvs[idx].value.type = HL_TYPE_DOUBLE;
                    op->result_kvs[idx].value.d = (double)lua_tonumber(L, -1);
                }
                break;
            case LUA_TSTRING: {
                size_t slen;
                const char *sv = lua_tolstring(L, -1, &slen);
                op->result_kvs[idx].value.type = HL_TYPE_TEXT;
                op->result_kvs[idx].value.s = malloc(slen + 1);
                if (!op->result_kvs[idx].value.s) {
                    /* OOM on string dup: mark slot NIL but bump count so
                     * the key allocation is released by op_free. */
                    op->result_kvs[idx].value.type = HL_TYPE_NIL;
                    op->result_count++;
                    lua_pop(L, 2);
                    return -1;
                }
                memcpy((void *)op->result_kvs[idx].value.s, sv, slen);
                ((char *)op->result_kvs[idx].value.s)[slen] = '\0';
                op->result_kvs[idx].value.len = slen;
                break;
            }
            default:
                op->result_kvs[idx].value.type = HL_TYPE_NIL;
                break;
            }
            op->result_count++;
            lua_pop(L, 1);
        }
        break;
    }
    default:
        op->result_kind = 0; /* unsupported → nil */
        break;
    }
    return 0;
}

/* ── KlWorkItem callbacks ──────────────────────────────────────────── */

static void lua_dispatch_work_fn(void *ud)
{
    HlLuaWorkerDispatchOp *op = (HlLuaWorkerDispatchOp *)ud;

    HlLuaWorkerCtx *wctx = get_lua_worker_ctx();
    if (!wctx || !wctx->L) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "failed to create worker Lua VM");
        return;
    }

    lua_State *L = wctx->L;

    /* Load the bytecode */
    int rc = luaL_loadbuffer(L, (const char *)op->bytecode,
                              op->bytecode_len, "dispatch");
    if (rc != LUA_OK) {
        op->error = 1;
        const char *msg = lua_tostring(L, -1);
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "load: %s", msg ? msg : "(unknown)");
        lua_pop(L, 1);
        return;
    }

    /* Reconstruct ctx table from HlKV array */
    if (op->ctx_kvs && op->ctx_count > 0) {
        push_kv_table(L, op->ctx_kvs, op->ctx_count);
    } else {
        lua_newtable(L); /* empty ctx */
    }

    /* Call fn(ctx) */
    rc = lua_pcall(L, 1, 1, 0);
    if (rc != LUA_OK) {
        op->error = 1;
        const char *msg = lua_tostring(L, -1);
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "dispatch: %s", msg ? msg : "(unknown)");
        lua_pop(L, 1);
        return;
    }

    /* Capture the return value */
    if (capture_result(L, op) != 0) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg), "out of memory");
    }
    lua_pop(L, 1); /* pop result */
}

static void lua_dispatch_done_fn(void *ud)
{
    HlLuaWorkerDispatchOp *op = (HlLuaWorkerDispatchOp *)ud;
    if (op->cancelled) {
        HlAsyncCtx *ctx = op->async_ctx;
        hl_lua_worker_dispatch_op_free(op);
        free(op);
        if (ctx) hl_async_ctx_free(ctx);
        return;
    }
#ifdef HL_ENABLE_HTTP
    hl_net_op_complete(op->async_ctx->net_ctx,
                       (HlSuspendOp *)&op->async_ctx->op);
#else
    (void)op;
#endif
}

static void lua_dispatch_cancel_fn(void *ud)
{
    HlLuaWorkerDispatchOp *op = (HlLuaWorkerDispatchOp *)ud;
    HlAsyncCtx *ctx = op->async_ctx;
    hl_lua_worker_dispatch_op_free(op);
    free(op);
    if (ctx) hl_async_ctx_free(ctx);
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_lua_worker_dispatch_submit(HlAsyncBackendPool *pool,
                                   HlLuaWorkerDispatchOp *op)
{
    if (!pool || !op) return -1;
    const HlAsyncBackend *be = hl_async_backend();
    return be->pool_submit(pool, lua_dispatch_work_fn, lua_dispatch_done_fn,
                           lua_dispatch_cancel_fn, op);
}

void hl_lua_worker_dispatch_op_free(HlLuaWorkerDispatchOp *op)
{
    if (!op) return;
    free(op->bytecode);
    hl_kv_free(op->ctx_kvs, op->ctx_count);
    op->ctx_kvs = NULL;
    free(op->result_str);
    hl_kv_free(op->result_kvs, op->result_count);
    op->result_kvs = NULL;
}

void hl_lua_worker_dispatch_op_free_all(void *ptr)
{
    HlLuaWorkerDispatchOp *op = (HlLuaWorkerDispatchOp *)ptr;
    hl_lua_worker_dispatch_op_free(op);
    free(op);
}

void hl_lua_worker_dispatch_cancel(KlAsyncOp *kl_op, void *user_data)
{
    (void)kl_op;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlLuaWorkerDispatchOp *op = (HlLuaWorkerDispatchOp *)ctx->driver;

    op->cancelled = 1;
    if (ctx->cont) {
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
    }
}
