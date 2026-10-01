/*
 * lua_worker.c - Per-worker Lua VMs for worker.dispatch()
 *
 * Manages TLS-keyed Lua VMs on worker threads. Each worker gets a
 * minimal Lua VM (base, string, table, math, utf8) with capabilities
 * registered via init hooks (e.g. db.* from lua/worker_db.c).
 *
 * Zero DB knowledge - capabilities are plugged in via hooks.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "internal.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"

#include <keel/thread_pool.h>
#include <keel/async.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

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

/* ── Worker Lua VM (one per dispatch) ──────────────────────────────── */

/* Each dispatch gets a FRESH state, closed when it returns. A VM reused
 * across dispatches leaked: a global, or a change to a shared library table
 * (string, math, db...), made by one dispatch was seen by the next one on
 * that thread - another request's code. A per-dispatch _ENV does not fix
 * that: rawset and getmetatable("").__index still reach the shared tables.
 * A new state costs tens of microseconds, small beside a thread hop. */

typedef struct {
    size_t used;
    size_t limit;   /* 0 = none */
} WorkerHeap;

/* The app's heap limit, counted per VM. Deliberately not the server's
 * tracking allocator: that one belongs to the event loop thread. */
static void *worker_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    WorkerHeap *heap = (WorkerHeap *)ud;
    size_t old = ptr ? osize : 0;   /* osize is a type tag when ptr is NULL */
    if (nsize == 0) {
        heap->used = heap->used >= old ? heap->used - old : 0;
        free(ptr);
        return NULL;
    }
    if (nsize > old && heap->limit > 0 &&
        (nsize - old > heap->limit || heap->used > heap->limit - (nsize - old)))
        return NULL;
    void *p = realloc(ptr, nsize);
    if (!p) return NULL;
    if (nsize > old) heap->used += nsize - old;
    else             heap->used -= old - nsize;
    return p;
}

static lua_State *worker_vm_new(WorkerHeap *heap,
                                const HlLuaWorkerDispatchOp *op)
{
    heap->used = 0;
    heap->limit = op->mem_limit;
    lua_State *L = lua_newstate(worker_alloc, heap);
    if (!L) return NULL;

    /* The same instruction budget a request handler gets: without it
     * `while true do end` held a pool thread for good, and a few of those
     * starved every db.async / compute.async / smtp job. */
    if (op->max_instructions > 0)
        lua_sethook(L, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(op->max_instructions));

    /* Open minimal standard libraries */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1);
    lua_pop(L, 1);

    /* Remove dangerous functions from minimal VM */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "load");
    lua_pushnil(L); lua_setglobal(L, "print");
    lua_pushnil(L); lua_setglobal(L, "io");
    lua_pushnil(L); lua_setglobal(L, "os");
    lua_pushnil(L); lua_setglobal(L, "require");

    /* The registered hooks install `db`: only for an app that declared it. */
    if (op->with_db) {
        for (int i = 0; i < init_hook_count; i++) {
            if (init_hooks[i](L) != 0) {
                log_error("[hull:worker] init hook %d failed", i);
                lua_close(L);
                return NULL;
            }
        }
    }
    return L;
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
                /* OOM on key dup: leave partial state - caller (op_free)
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
                char *buf = malloc(slen + 1);
                if (!buf) {
                    /* OOM on string dup: mark slot NIL but bump count so
                     * the key allocation is released by op_free. */
                    op->result_kvs[idx].value.s = NULL;
                    op->result_kvs[idx].value.type = HL_TYPE_NIL;
                    op->result_count++;
                    lua_pop(L, 2);
                    return -1;
                }
                memcpy(buf, sv, slen);
                buf[slen] = '\0';
                op->result_kvs[idx].value.s = buf;
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

static void lua_dispatch_run(lua_State *L, HlLuaWorkerDispatchOp *op);

static void lua_dispatch_work_fn(void *ud)
{
    HlLuaWorkerDispatchOp *op = (HlLuaWorkerDispatchOp *)ud;

    WorkerHeap heap;
    lua_State *L = worker_vm_new(&heap, op);
    if (!L) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "failed to create worker Lua VM");
        return;
    }
    lua_dispatch_run(L, op);
    lua_close(L);
}

static void lua_dispatch_run(lua_State *L, HlLuaWorkerDispatchOp *op)
{

    /* Load the bytecode */
    /* Binary: lua_worker_dispatch dumped this from a function value
     * (luaL_checktype LUA_TFUNCTION) - never bytes the app supplied. */
    int rc = luaL_loadbufferx(L, (const char *)op->bytecode,
                              op->bytecode_len, "dispatch", "b");
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
    HlAsyncCtx *ctx = op->async_ctx;
    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
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
