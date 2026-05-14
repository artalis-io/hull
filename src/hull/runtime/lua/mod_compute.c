/* mod_compute.c — hull.compute module: WASM compute plugins
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "mod_buffer.h"
#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/cap/wasm_stream.h"
#include "hull/cap/fs.h"
#include "hull/alloc.h"
#include "hull/async.h"
#include "hull/worker_wasm.h"
#include "hull/vfs.h"

#include <keel/server.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.compute module — WASM compute plugins
 *
 * compute.call(name, input, opts?) -> output, err
 * compute.load(name) -> true, err
 * ════════════════════════════════════════════════════════════════════ */

/* ── WasmBuffer metatable ─────────────────────────────────────────── */

static HlWasmBuffer *check_wasm_buf(lua_State *L, int idx)
{
    HlWasmBuffer **pp = luaL_testudata(L, idx, HL_WASM_BUF_MT);
    if (!pp || !*pp) return NULL;
    return *pp;
}

static int lua_wasm_buf_len(lua_State *L)
{
    HlWasmBuffer *buf = check_wasm_buf(L, 1);
    lua_pushinteger(L, buf ? (lua_Integer)hl_wasm_buffer_len(buf) : 0);
    return 1;
}

static int lua_wasm_buf_bytes(lua_State *L)
{
    HlWasmBuffer *buf = check_wasm_buf(L, 1);
    if (!buf || buf->closed) {
        lua_pushnil(L);
        return 1;
    }
    const void *data = hl_wasm_buffer_data(buf);
    size_t len = hl_wasm_buffer_len(buf);
    if (data && len > 0)
        lua_pushlstring(L, (const char *)data, len);
    else
        lua_pushlstring(L, "", 0);
    return 1;
}

static int lua_wasm_buf_close(lua_State *L)
{
    HlWasmBuffer **pp = luaL_testudata(L, 1, HL_WASM_BUF_MT);
    if (pp && *pp) {
        HlAllocator *a = (*pp)->alloc;
        hl_wasm_buffer_destroy(*pp);
        hl_alloc_free(a, *pp, sizeof(HlWasmBuffer));
        *pp = NULL;
    }
    return 0;
}

static int lua_wasm_buf_gc(lua_State *L)
{
    return lua_wasm_buf_close(L);
}

static int lua_wasm_buf_tostring(lua_State *L)
{
    HlWasmBuffer *buf = check_wasm_buf(L, 1);
    if (!buf || buf->closed)
        lua_pushstring(L, "WasmBuffer(closed)");
    else
        lua_pushfstring(L, "WasmBuffer(%d)", (int)buf->len);
    return 1;
}

static void lua_register_wasm_buf_metatable(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"len",   lua_wasm_buf_len},
        {"bytes", lua_wasm_buf_bytes},
        {"close", lua_wasm_buf_close},
        {NULL, NULL}
    };

    luaL_newmetatable(L, HL_WASM_BUF_MT);

    /* __index = methods table */
    luaL_newlib(L, methods);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lua_wasm_buf_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, lua_wasm_buf_close);
    lua_setfield(L, -2, "__close");

    lua_pushcfunction(L, lua_wasm_buf_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, lua_wasm_buf_len);
    lua_setfield(L, -2, "__len");

    lua_pop(L, 1); /* pop metatable */
}

/* Helper: push an HlWasmBuffer* as Lua userdata. Takes ownership of buf. */
void lua_push_wasm_buffer(lua_State *L, HlWasmBuffer *buf)
{
    HlWasmBuffer **pp = lua_newuserdata(L, sizeof(HlWasmBuffer *));
    *pp = buf;
    luaL_setmetatable(L, HL_WASM_BUF_MT);
}

/* compute.buffer(string) -> WasmBuffer(OWNED) */
static int lua_compute_buffer(lua_State *L)
{
    size_t len = 0;
    const char *str = luaL_checklstring(L, 1, &len);

    HlLua *lua_ctx = get_hl_lua(L);
    HlWasmBuffer *buf = hl_wasm_buffer_create_owned(str, len,
                                                      lua_ctx ? lua_ctx->base.alloc : NULL);
    if (!buf)
        return luaL_error(L, "compute.buffer: out of memory");

    lua_push_wasm_buffer(L, buf);
    return 1;
}

/* Clamp per-call opts to runtime ceiling (three-tier: per-call <= CLI/manifest <= compile-time) */
static void wasm_clamp_opts(HlWasmCallOpts *opts, const HlRuntime *base)
{
    hl_cap_wasm_clamp_opts(opts,
        base->wasm_config.max_input, base->wasm_config.max_output,
        base->wasm_config.heap_size, base->wasm_config.stack_size,
        base->wasm_config.gas);
}

/* compute.call(name, input, opts?) -> output_string | nil, error_string | nil */
static int lua_compute_call(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua)
        return luaL_error(L, "compute.call: runtime not available");

    if (!lua->base.wasm_cache)
        return luaL_error(L, "compute.call: WASM runtime not initialized");

    const char *name = luaL_checkstring(L, 1);
    const void *input;
    size_t input_len = 0;

    /* Accept WasmBuffer, MappedBuffer, or string as input */
    HlWasmBuffer *wbuf_in = check_wasm_buf(L, 2);
    if (wbuf_in && !wbuf_in->closed) {
        input = hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer **mmap_pp = luaL_testudata(L, 2, HL_MMAP_MT);
        if (mmap_pp && *mmap_pp && !(*mmap_pp)->closed) {
            input = (*mmap_pp)->addr;
            input_len = (*mmap_pp)->len;
        } else {
            input = luaL_checklstring(L, 2, &input_len);
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;

    /* Parse opts table if provided */
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "max_input");
        if (lua_isinteger(L, -1))
            opts.max_input = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "max_output");
        if (lua_isinteger(L, -1))
            opts.max_output = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "heap");
        if (lua_isinteger(L, -1))
            opts.heap_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "stack");
        if (lua_isinteger(L, -1))
            opts.stack_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "gas");
        if (lua_isinteger(L, -1))
            opts.gas = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "buffer");
        if (lua_toboolean(L, -1))
            want_buffer = 1;
        lua_pop(L, 1);
    }

    wasm_clamp_opts(&opts, &lua->base);

    if (want_buffer) {
        HlWasmBuffer *out_buf = NULL;
        const char *err_msg = NULL;

        int rc = hl_cap_wasm_call_buf(lua->base.wasm_cache, name,
                                       input, input_len,
                                       &out_buf, &opts, NULL, NULL,
                                       lua->base.app_vfs,
                                       lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL,
                                       lua->base.alloc, &err_msg);
        if (rc != 0) {
            lua_pushnil(L);
            lua_pushstring(L, err_msg ? err_msg : "unknown_error");
            return 2;
        }

        lua_push_wasm_buffer(L, out_buf);
        lua_pushnil(L);
        return 2;
    }

    /* Non-buffer path (original behavior) */
    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    int rc = hl_cap_wasm_call(lua->base.wasm_cache, name,
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               lua->base.app_vfs,
                               lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL,
                               lua->base.alloc, &err_msg);

    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "unknown_error");
        return 2;
    }

    if (output && output_len > 0)
        lua_pushlstring(L, (const char *)output, output_len);
    else
        lua_pushlstring(L, "", 0);
    lua_pushnil(L);

    hl_alloc_free(lua->base.alloc, output, output_len);
    return 2;
}

/* compute.load(name) -> true | nil, error_string */
static int lua_compute_load(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.wasm_cache)
        return luaL_error(L, "compute.load: WASM runtime not initialized");

    const char *name = luaL_checkstring(L, 1);

    int rc = hl_cap_wasm_load(lua->base.wasm_cache, name,
                               lua->base.app_vfs,
                               lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, rc == HL_WASM_ERR_NOT_FOUND ? "not_found" : "load_failed");
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ── compute.async.call ─────────────────────────────────────────────── */

/* push_result callback: convert HlWorkerWasmOp result to Lua value.
 * On error, raises a Lua error (propagates through the coroutine resume
 * as a regular failure). On success, returns the output directly:
 *   - buffer mode → WasmBuffer userdata
 *   - normal mode → output string
 * Matches db.async / http.async / gpu.async error-throwing convention. */
static void lua_push_worker_wasm_result(lua_State *L, void *driver)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)driver;

    if (op->error) {
        luaL_error(L, "compute.async: %s", op->error_msg);
        return; /* unreachable */
    }
    if (op->output_buf) {
        /* Buffer mode: push WasmBuffer userdata, transfer ownership */
        lua_push_wasm_buffer(L, op->output_buf);
        op->output_buf = NULL; /* prevent hl_worker_wasm_op_free from destroying */
        return;
    }
    if (op->output && op->output_len > 0)
        lua_pushlstring(L, (const char *)op->output, op->output_len);
    else
        lua_pushlstring(L, "", 0);
}

static int lua_compute_async_call(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "compute.async not available (no thread pool)");
    if (!lua->server || !lua->active_conn)
        return luaL_error(L, "compute.async can only be called from a request handler");

    if (!lua->base.wasm_cache)
        return luaL_error(L, "compute.async.call: WASM runtime not initialized");

    const char *name = luaL_checkstring(L, 1);
    const void *input;
    size_t input_len = 0;

    /* Accept WasmBuffer, MappedBuffer, or string as input */
    HlWasmBuffer *wbuf_in = check_wasm_buf(L, 2);
    if (wbuf_in && !wbuf_in->closed) {
        input = hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer **mmap_pp = luaL_testudata(L, 2, HL_MMAP_MT);
        if (mmap_pp && *mmap_pp && !(*mmap_pp)->closed) {
            input = (*mmap_pp)->addr;
            input_len = (*mmap_pp)->len;
        } else {
            input = luaL_checklstring(L, 2, &input_len);
        }
    }

    /* Parse opts */
    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "max_input");
        if (lua_isinteger(L, -1)) opts.max_input = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "max_output");
        if (lua_isinteger(L, -1)) opts.max_output = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "heap");
        if (lua_isinteger(L, -1)) opts.heap_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "stack");
        if (lua_isinteger(L, -1)) opts.stack_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "gas");
        if (lua_isinteger(L, -1)) opts.gas = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "buffer");
        if (lua_toboolean(L, -1)) want_buffer = 1;
        lua_pop(L, 1);
    }

    wasm_clamp_opts(&opts, &lua->base);

    /* Pre-load module on event loop thread (cache writes are not thread-safe) */
    {
        int rc = hl_cap_wasm_load(lua->base.wasm_cache, name,
                                   lua->base.app_vfs,
                                   lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL);
        if (rc != 0)
            return luaL_error(L, "compute.async.call: %s",
                              rc == HL_WASM_ERR_NOT_FOUND ? "not_found" : "load_failed");
    }

    /* Allocate worker op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op)
        return luaL_error(L, "compute.async.call: out of memory");

    op->server = lua->server;
    op->wasm_cache = lua->base.wasm_cache;
    op->app_vfs = lua->base.app_vfs;
    op->app_dir = lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL;
    op->alloc = lua->base.alloc;
    snprintf(op->name, sizeof(op->name), "%s", name);
    op->opts = opts;
    op->want_buffer = want_buffer;

    /* Deep-copy input (Lua string lives on GC heap) */
    if (input_len > 0) {
        op->input = malloc(input_len);
        if (!op->input) {
            free(op);
            return luaL_error(L, "compute.async.call: out of memory");
        }
        memcpy(op->input, input, input_len);
    }
    op->input_len = input_len;

    /* Create async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.alloc);
    if (!ctx) {
        hl_worker_wasm_op_free(op);
        free(op);
        return luaL_error(L, "compute.async.call: out of memory");
    }

    /* Create Lua continuation */
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_worker_wasm_result);
    if (!cont) {
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "compute.async.call: out of memory");
    }
    ctx->cont = cont;
    ctx->driver = op;
    ctx->free_driver = hl_worker_wasm_op_free_all;
    ctx->op.on_cancel = hl_worker_wasm_async_cancel;

    op->async_ctx = ctx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_worker_wasm_submit(lua->base.thread_pool, op) != 0) {
        ctx->cont->destroy(ctx->cont);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "compute.async.call: thread pool full");
    }

    /* Suspend connection and yield */
    if (kl_async_suspend(lua->server, lua->active_conn, &ctx->op) < 0) {
        op->cancelled = 1;
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
        return luaL_error(L, "compute.async.call: failed to suspend connection");
    }

    return lua_yieldk(L, 0, 0, NULL);
}

/* ── WasmInstance metatable ─────────────────────────────────────────── */

#define HL_WASM_INST_MT "HlWasmInstance"

static HlWasmInstance *check_wasm_inst(lua_State *L, int idx)
{
    HlWasmInstance **pp = luaL_testudata(L, idx, HL_WASM_INST_MT);
    if (!pp || !*pp) return NULL;
    return *pp;
}

/* inst:call(input, opts?) -> output, err  OR  buf, err */
static int lua_wasm_inst_call(lua_State *L)
{
    HlWasmInstance *pi = check_wasm_inst(L, 1);
    if (!pi)
        return luaL_error(L, "WasmInstance:call: invalid instance");

    HlLua *lua = get_hl_lua(L);

    /* Parse input (string, WasmBuffer, MappedBuffer) */
    const void *input;
    size_t input_len = 0;
    HlWasmBuffer *wbuf_in = check_wasm_buf(L, 2);
    if (wbuf_in && !wbuf_in->closed) {
        input = hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer **mmap_pp = luaL_testudata(L, 2, HL_MMAP_MT);
        if (mmap_pp && *mmap_pp && !(*mmap_pp)->closed) {
            input = (*mmap_pp)->addr;
            input_len = (*mmap_pp)->len;
        } else {
            input = luaL_checklstring(L, 2, &input_len);
        }
    }

    /* Parse per-call opts */
    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "gas");
        if (lua_isinteger(L, -1)) opts.gas = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "max_input");
        if (lua_isinteger(L, -1)) opts.max_input = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "max_output");
        if (lua_isinteger(L, -1)) opts.max_output = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "buffer");
        if (lua_toboolean(L, -1)) want_buffer = 1;
        lua_pop(L, 1);
    }

    if (lua) wasm_clamp_opts(&opts, &lua->base);

    if (want_buffer) {
        HlWasmBuffer *out_buf = NULL;
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_instance_call_buf(pi, input, input_len,
                                                &out_buf, &opts, NULL, NULL,
                                                lua ? lua->base.alloc : NULL, &err_msg);
        if (rc != 0) {
            lua_pushnil(L);
            lua_pushstring(L, err_msg ? err_msg : "unknown_error");
            return 2;
        }
        lua_push_wasm_buffer(L, out_buf);
        lua_pushnil(L);
        return 2;
    }

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;
    int rc = hl_cap_wasm_instance_call(pi, input, input_len,
                                        &output, &output_len,
                                        &opts, NULL, NULL,
                                        lua ? lua->base.alloc : NULL, &err_msg);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "unknown_error");
        return 2;
    }

    if (output && output_len > 0)
        lua_pushlstring(L, (const char *)output, output_len);
    else
        lua_pushlstring(L, "", 0);
    lua_pushnil(L);

    if (lua) hl_alloc_free(lua->base.alloc, output, output_len);
    else free(output);
    return 2;
}

/* inst:async_call(input, opts?) — dispatch to thread pool, yield */
static int lua_wasm_inst_async_call(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "WasmInstance:async_call: not available (no thread pool)");
    if (!lua->server || !lua->active_conn)
        return luaL_error(L, "WasmInstance:async_call: can only be called from a request handler");

    HlWasmInstance *pi = check_wasm_inst(L, 1);
    if (!pi || pi->closed)
        return luaL_error(L, "WasmInstance:async_call: instance closed");
    if (atomic_load(&pi->busy))
        return luaL_error(L, "WasmInstance:async_call: instance busy");

    /* Parse input */
    const void *input;
    size_t input_len = 0;
    HlWasmBuffer *wbuf_in = check_wasm_buf(L, 2);
    if (wbuf_in && !wbuf_in->closed) {
        input = hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer **mmap_pp = luaL_testudata(L, 2, HL_MMAP_MT);
        if (mmap_pp && *mmap_pp && !(*mmap_pp)->closed) {
            input = (*mmap_pp)->addr;
            input_len = (*mmap_pp)->len;
        } else {
            input = luaL_checklstring(L, 2, &input_len);
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "gas");
        if (lua_isinteger(L, -1)) opts.gas = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "max_input");
        if (lua_isinteger(L, -1)) opts.max_input = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "max_output");
        if (lua_isinteger(L, -1)) opts.max_output = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "buffer");
        if (lua_toboolean(L, -1)) want_buffer = 1;
        lua_pop(L, 1);
    }
    wasm_clamp_opts(&opts, &lua->base);

    /* Allocate worker op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op)
        return luaL_error(L, "WasmInstance:async_call: out of memory");

    op->server = lua->server;
    op->persistent_inst = pi;
    op->alloc = lua->base.alloc;
    snprintf(op->name, sizeof(op->name), "%s", pi->name);
    op->opts = opts;
    op->want_buffer = want_buffer;

    /* Deep-copy input */
    if (input_len > 0) {
        op->input = malloc(input_len);
        if (!op->input) {
            free(op);
            return luaL_error(L, "WasmInstance:async_call: out of memory");
        }
        memcpy(op->input, input, input_len);
    }
    op->input_len = input_len;

    /* Set busy before dispatch (event loop thread) */
    atomic_store(&pi->busy, 1);

    /* Create async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.alloc);
    if (!ctx) {
        atomic_store(&pi->busy, 0);
        hl_worker_wasm_op_free(op);
        free(op);
        return luaL_error(L, "WasmInstance:async_call: out of memory");
    }

    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_worker_wasm_result);
    if (!cont) {
        atomic_store(&pi->busy, 0);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "WasmInstance:async_call: out of memory");
    }
    ctx->cont = cont;
    ctx->driver = op;
    ctx->free_driver = hl_worker_wasm_op_free_all;
    ctx->op.on_cancel = hl_worker_wasm_async_cancel;

    op->async_ctx = ctx;
    op->cancelled = 0;

    if (hl_worker_wasm_submit(lua->base.thread_pool, op) != 0) {
        atomic_store(&pi->busy, 0);
        ctx->cont->destroy(ctx->cont);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "WasmInstance:async_call: thread pool full");
    }

    if (kl_async_suspend(lua->server, lua->active_conn, &ctx->op) < 0) {
        atomic_store(&pi->busy, 0);
        op->cancelled = 1;
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
        return luaL_error(L, "WasmInstance:async_call: failed to suspend");
    }

    return lua_yieldk(L, 0, 0, NULL);
}

static int lua_wasm_inst_close(lua_State *L)
{
    HlWasmInstance **pp = luaL_testudata(L, 1, HL_WASM_INST_MT);
    if (pp && *pp) {
        hl_cap_wasm_instance_destroy(*pp);
        *pp = NULL;
    }
    return 0;
}

static int lua_wasm_inst_name(lua_State *L)
{
    HlWasmInstance *pi = check_wasm_inst(L, 1);
    if (pi)
        lua_pushstring(L, pi->name);
    else
        lua_pushnil(L);
    return 1;
}

static int lua_wasm_inst_closed(lua_State *L)
{
    HlWasmInstance *pi = check_wasm_inst(L, 1);
    lua_pushboolean(L, !pi || pi->closed);
    return 1;
}

static int lua_wasm_inst_gc(lua_State *L)
{
    return lua_wasm_inst_close(L);
}

static int lua_wasm_inst_tostring(lua_State *L)
{
    HlWasmInstance *pi = check_wasm_inst(L, 1);
    if (!pi || pi->closed)
        lua_pushstring(L, "WasmInstance(closed)");
    else
        lua_pushfstring(L, "WasmInstance(%s)", pi->name);
    return 1;
}

/* inst.async:call(input, opts?) — proxy that prepends the instance as arg 1.
 * Stack on entry: async_proxy, input, [opts]
 * We need to retrieve the instance from the proxy's upvalue. */
static int lua_wasm_inst_async_proxy_call(lua_State *L)
{
    /* The instance userdata is stored as upvalue 1 of this closure */
    lua_pushvalue(L, lua_upvalueindex(1)); /* push instance */
    lua_insert(L, 1);                      /* move to position 1 */
    return lua_wasm_inst_async_call(L);    /* call with instance as self */
}

/* __index metamethod for WasmInstance.
 * Returns methods directly for known names, creates async proxy for "async". */
static int lua_wasm_inst_index(lua_State *L)
{
    const char *key = lua_tostring(L, 2);
    if (!key)
        return 0;

    if (strcmp(key, "async") == 0) {
        /* Create { call = <closure with inst upvalue> } */
        lua_newtable(L);
        lua_pushvalue(L, 1);  /* push the instance userdata */
        lua_pushcclosure(L, lua_wasm_inst_async_proxy_call, 1);
        lua_setfield(L, -2, "call");
        return 1;
    }

    /* Look up in methods table (stored as upvalue 1 of this __index) */
    lua_pushvalue(L, 2);                    /* push key */
    lua_gettable(L, lua_upvalueindex(1));   /* methods[key] */
    return 1;
}

static void lua_register_wasm_inst_metatable(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"call",       lua_wasm_inst_call},
        {"close",      lua_wasm_inst_close},
        {"name",       lua_wasm_inst_name},
        {"closed",     lua_wasm_inst_closed},
        {NULL, NULL}
    };

    luaL_newmetatable(L, HL_WASM_INST_MT);

    /* __index = closure(lua_wasm_inst_index, methods_table) */
    luaL_newlib(L, methods);
    lua_pushcclosure(L, lua_wasm_inst_index, 1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lua_wasm_inst_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, lua_wasm_inst_close);
    lua_setfield(L, -2, "__close");
    lua_pushcfunction(L, lua_wasm_inst_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1);
}

/* compute.instance(name, opts?) -> WasmInstance | nil, err */
static int lua_compute_instance(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua)
        return luaL_error(L, "compute.instance: runtime not available");
    if (!lua->base.wasm_cache)
        return luaL_error(L, "compute.instance: WASM runtime not initialized");

    const char *name = luaL_checkstring(L, 1);

    HlWasmCallOpts opts = {0};
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "heap");
        if (lua_isinteger(L, -1)) opts.heap_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "stack");
        if (lua_isinteger(L, -1)) opts.stack_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "gas");
        if (lua_isinteger(L, -1)) opts.gas = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "max_input");
        if (lua_isinteger(L, -1)) opts.max_input = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "max_output");
        if (lua_isinteger(L, -1)) opts.max_output = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    wasm_clamp_opts(&opts, &lua->base);

    const char *err_msg = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        lua->base.wasm_cache, name, &opts,
        lua->base.app_vfs,
        lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL,
        lua->base.alloc, &err_msg);

    if (!pi) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "unknown_error");
        return 2;
    }

    HlWasmInstance **pp = lua_newuserdata(L, sizeof(HlWasmInstance *));
    *pp = pi;
    luaL_setmetatable(L, HL_WASM_INST_MT);
    return 1;
}

/* compute.segment(module, segment, data)
 * compute.segment(module, segment, nil) -- remove segment
 * compute.segment(module, nil)          -- remove all segments
 */
static int lua_compute_segment(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua)
        return luaL_error(L, "compute.segment: runtime not available");
    if (!lua->base.wasm_cache)
        return luaL_error(L, "compute.segment: WASM runtime not initialized");

    const char *module_name = luaL_checkstring(L, 1);

    /* compute.segment(module, nil) -> remove all */
    if (lua_gettop(L) < 3 && lua_isnil(L, 2)) {
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_data_load(lua->base.wasm_cache, module_name,
                                        NULL, NULL, 0, NULL,
                                        lua->base.app_vfs,
                                        lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL,
                                        &err_msg);
        if (rc != 0) {
            lua_pushnil(L);
            lua_pushstring(L, err_msg ? err_msg : "unknown_error");
            return 2;
        }
        /* Release all MappedBuffer pins for this module */
        lua_pushfstring(L, "__hull_data_%s", module_name);
        lua_pushnil(L);
        lua_settable(L, LUA_REGISTRYINDEX);
        lua_pushboolean(L, 1);
        return 1;
    }

    const char *segment_name = luaL_checkstring(L, 2);

    /* compute.segment(module, segment, nil) -> remove segment */
    if (lua_isnil(L, 3) || lua_isnoneornil(L, 3)) {
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_data_load(lua->base.wasm_cache, module_name,
                                        segment_name, NULL, 0, NULL,
                                        lua->base.app_vfs,
                                        lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL,
                                        &err_msg);
        if (rc != 0) {
            lua_pushnil(L);
            lua_pushstring(L, err_msg ? err_msg : "unknown_error");
            return 2;
        }
        /* Release MappedBuffer registry pin for this segment */
        lua_pushfstring(L, "__hull_data_%s", module_name);
        lua_gettable(L, LUA_REGISTRYINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, segment_name);
            lua_pushnil(L);
            lua_settable(L, -3);
        }
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
        return 1;
    }

    /* compute.segment(module, segment, data) -> add/replace */
    const void *data = NULL;
    size_t data_len = 0;
    void *pre_alloc = NULL;
    int has_mmap_ref = 0;

    if (lua_isstring(L, 3)) {
        data = lua_tolstring(L, 3, &data_len);
    } else {
        /* Check for MappedBuffer */
        HlMappedBuffer **mmap_pp = luaL_testudata(L, 3, HL_MMAP_MT);
        if (mmap_pp && *mmap_pp && !(*mmap_pp)->closed) {
            data = (*mmap_pp)->addr;
            data_len = (*mmap_pp)->len;
            pre_alloc = (*mmap_pp)->addr;
            has_mmap_ref = 1;
        } else {
            return luaL_error(L, "compute.segment: data must be a string or MappedBuffer");
        }
    }

    if (data_len == 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "data_empty");
        return 2;
    }

    const char *err_msg = NULL;
    int rc = hl_cap_wasm_data_load(lua->base.wasm_cache, module_name,
                                    segment_name, data, data_len, pre_alloc,
                                    lua->base.app_vfs,
                                    lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL,
                                    &err_msg);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "unknown_error");
        return 2;
    }

    /* Pin MappedBuffer in registry to prevent GC while shared heap uses it.
     * Registry["__hull_data_<module>"] = { [segment] = MappedBuffer, ... } */
    lua_pushfstring(L, "__hull_data_%s", module_name);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushfstring(L, "__hull_data_%s", module_name);
        lua_pushvalue(L, -2);  /* dup the table */
        lua_settable(L, LUA_REGISTRYINDEX);
    }
    lua_pushstring(L, segment_name);
    if (has_mmap_ref)
        lua_pushvalue(L, 3);  /* push the MappedBuffer userdata */
    else
        lua_pushnil(L);       /* string data was copied, no pin needed */
    lua_settable(L, -3);
    lua_pop(L, 1);  /* pop the pins table */

    lua_pushboolean(L, 1);
    return 1;
}

/* ── compute.stream ────────────────────────────────────────────────── */

typedef struct {
    lua_State *L;
    int        func_ref;
    int        error;
} LuaStreamCbCtx;

static int lua_stream_cb_trampoline(const void *data, size_t len,
                                     uint32_t index, int is_last,
                                     void *user_data)
{
    LuaStreamCbCtx *ctx = (LuaStreamCbCtx *)user_data;
    if (ctx->error) return -1;

    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->func_ref);
    lua_pushlstring(ctx->L, (const char *)data, len);
    lua_pushinteger(ctx->L, (lua_Integer)(index + 1)); /* 1-indexed for Lua */
    lua_pushboolean(ctx->L, is_last);

    if (lua_pcall(ctx->L, 3, 0, 0) != LUA_OK) {
        ctx->error = 1;
        lua_pop(ctx->L, 1);
        return -1;
    }
    return 0;
}

/* compute.stream(name, input, [output], [opts]) -> string | true, err */
static int lua_compute_stream(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua)
        return luaL_error(L, "compute.stream: runtime not available");
    if (!lua->base.wasm_cache)
        return luaL_error(L, "compute.stream: WASM runtime not initialized");

    const char *name = luaL_checkstring(L, 1);

    /* ── Parse input (arg 2) ─────────────────────────────────────── */
    HlStreamInput input = {0};

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "file");
        if (lua_isstring(L, -1)) {
            input.kind = HL_STREAM_IN_FILE;
            input.path = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
        if (input.kind != HL_STREAM_IN_FILE)
            return luaL_error(L, "compute.stream: input table must have 'file' key");
    } else {
        HlBufferView view;
        if (lua_get_buffer(L, 2, &view)) {
            input.kind = HL_STREAM_IN_BUFFER;
            input.buffer = view;
        } else {
            size_t slen;
            const char *s = luaL_checklstring(L, 2, &slen);
            input.kind = HL_STREAM_IN_BUFFER;
            input.buffer.data = s;
            input.buffer.len = slen;
        }
    }

    /* ── Parse output (arg 3) and opts (arg 3 or 4) ──────────────── */
    HlStreamOutput out_storage = {0};
    HlStreamOutput *out_ptr = NULL;
    void *out_data = NULL;
    size_t out_len = 0;
    LuaStreamCbCtx cb_ctx = {0};
    cb_ctx.func_ref = LUA_NOREF;
    /* Parse output (arg 3): nil/absent → buffer, function → callback,
     * table with "file" → file, table without "file" → treat as opts */
    int has_output = 0;
    int opts_arg = 4;

    if (lua_isfunction(L, 3)) {
        has_output = 1;
        cb_ctx.L = L;
        lua_pushvalue(L, 3);
        cb_ctx.func_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        out_storage.kind = HL_STREAM_OUT_CALLBACK;
        out_storage.callback.fn = lua_stream_cb_trampoline;
        out_storage.callback.user_data = &cb_ctx;
        out_ptr = &out_storage;
    } else if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "file");
        if (lua_isstring(L, -1)) {
            has_output = 1;
            out_storage.kind = HL_STREAM_OUT_FILE;
            out_storage.path = lua_tostring(L, -1);
            out_ptr = &out_storage;
        }
        lua_pop(L, 1);
        if (!has_output) {
            /* It's the opts table */
            opts_arg = 3;
        }
    }

    if (!has_output) {
        /* Default: return buffer */
        out_storage.kind = HL_STREAM_OUT_BUFFER;
        out_storage.buffer.data = &out_data;
        out_storage.buffer.len = &out_len;
        out_ptr = &out_storage;
    }

    /* ── Parse opts ──────────────────────────────────────────────── */
    HlStreamOpts stream_opts = {0};
    if (lua_istable(L, opts_arg)) {
        lua_getfield(L, opts_arg, "chunk_size");
        if (!lua_isnil(L, -1))
            stream_opts.chunk_size = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, opts_arg, "gas");
        if (!lua_isnil(L, -1))
            stream_opts.call_opts.gas = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, opts_arg, "heap");
        if (!lua_isnil(L, -1))
            stream_opts.call_opts.heap_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, opts_arg, "stack");
        if (!lua_isnil(L, -1))
            stream_opts.call_opts.stack_size = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    wasm_clamp_opts(&stream_opts.call_opts, &lua->base);

    /* ── Call stream API ─────────────────────────────────────────── */
    const char *err = NULL;
    HlStreamResult res = {0};

    int rc = hl_cap_wasm_stream(
        lua->base.wasm_cache, name,
        &input, out_ptr, &stream_opts,
        lua->base.fs_cfg,
        lua->base.app_vfs,
        lua->base.app_vfs ? lua->base.app_vfs->root_dir : NULL,
        lua->base.alloc, &res, &err);

    /* Clean up callback reference */
    if (cb_ctx.func_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, cb_ctx.func_ref);

    if (rc != HL_WASM_OK) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "stream_failed");
        return 2;
    }

    /* Return based on output mode */
    if (out_storage.kind == HL_STREAM_OUT_BUFFER && out_data) {
        lua_pushlstring(L, (const char *)out_data, out_len);
        hl_alloc_free(lua->base.alloc, out_data, out_len);
        lua_pushnil(L);
        return 2;
    } else if (out_storage.kind == HL_STREAM_OUT_BUFFER) {
        lua_pushlstring(L, "", 0);
        lua_pushnil(L);
        return 2;
    }

    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int lua_compute_available(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    lua_pushboolean(L, lua && lua->base.wasm_cache != NULL);
    return 1;
}

static const luaL_Reg compute_funcs[] = {
    {"available", lua_compute_available},
    {"call",     lua_compute_call},
    {"load",     lua_compute_load},
    {"buffer",   lua_compute_buffer},
    {"instance", lua_compute_instance},
    {"segment",  lua_compute_segment},
    {"stream",   lua_compute_stream},
    {NULL, NULL}
};

static const luaL_Reg compute_async_funcs[] = {
    {"call", lua_compute_async_call},
    {NULL, NULL}
};

int luaopen_hull_compute(lua_State *L)
{
    lua_register_wasm_buf_metatable(L);
    lua_register_wasm_inst_metatable(L);
    luaL_newlib(L, compute_funcs);
    luaL_newlib(L, compute_async_funcs);
    lua_setfield(L, -2, "async");  /* compute.async = {...} */
    return 1;
}

#endif /* HL_ENABLE_WASM */
