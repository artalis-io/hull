/* mod_exec.c - hull.exec module: run a command the manifest declares
 * (docs/design/exec_broker.md)
 *
 *   local exec = require("hull.exec")
 *   local r = exec.run("unit-state", { "hs-api.service" })
 *   -- r = { status, signal, stdout, stderr, timed_out, stdout_truncated,
 *   --       stderr_truncated, duration_ms }
 *
 * The name must be declared in the manifest's `exec` section, and the
 * arguments must fill its slots (cap/exec.c, hl_exec_check_call - the one
 * matcher). A mismatch raises, naming the argument and the rule.
 *
 * This version has no command broker yet: outside `hull test` every call
 * raises after the match. Under `hull test`, test.exec_stub(name, result)
 * registers the answer; the call is still matched in full first, so a test
 * catches a bad argument, and the output is cut to the command's
 * stdout_max / stderr_max as the broker will cut it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "mod_exec.h"
#include "refusal.h"
#include "hull/cap/exec.h"
#include "hull/cap/exec_broker.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "internal.h"   /* hl_lua_check_can_wait, hl_lua_can_wait, hl_lua_async_k_ok */
#ifdef HL_ENABLE_DB
#include "hull/cap/db_registry.h"   /* hl_db_registry_open_txn */
#endif

#include <stdlib.h>
#include <string.h>

#define EXEC_STUBS_KEY "__hull_exec_stubs"

static const HlExecCmd *exec_find(HlLua *lua, const char *name, size_t n,
                                  HlExecCmd *out)
{
    if (!lua || !lua->base.exec_policy) return NULL;
    if (hl_exec_policy_find(lua->base.exec_policy, lua->base.exec_policy_len,
                            name, n, out) != 0)
        return NULL;
    return out;
}

/* r.<key> as an optional string, cut to @p max; sets *cut when it was. */
static void set_output(lua_State *L, int stub, int res, const char *key,
                       uint32_t max, const char *cut_key)
{
    size_t n = 0;
    const char *s = "";
    lua_getfield(L, stub, key);
    if (lua_type(L, -1) == LUA_TSTRING) s = lua_tolstring(L, -1, &n);
    else if (!lua_isnil(L, -1))
        luaL_error(L, "exec stub: %s must be a string", key);
    int cut = n > max;
    lua_pushlstring(L, s, cut ? max : n);
    lua_setfield(L, res, key);
    lua_pop(L, 1);
    lua_pushboolean(L, cut);
    lua_setfield(L, res, cut_key);
}

/* The result table the broker will return, built from a stub's table at
 * @p stub: every field present, the outputs cut to the command's caps. */
static void push_result(lua_State *L, int stub, const HlExecCmd *cmd)
{
    lua_createtable(L, 0, 8);
    int res = lua_gettop(L);

    lua_getfield(L, stub, "status");
    lua_Integer status = 0;
    if (lua_isinteger(L, -1)) status = lua_tointeger(L, -1);
    else if (!lua_isnil(L, -1)) luaL_error(L, "exec stub: status must be an integer");
    lua_pop(L, 1);
    lua_pushinteger(L, status);
    lua_setfield(L, res, "status");

    lua_getfield(L, stub, "signal");
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING)
        luaL_error(L, "exec stub: signal must be a string (\"SIGKILL\") or nil");
    lua_setfield(L, res, "signal");

    set_output(L, stub, res, "stdout", cmd->stdout_max, "stdout_truncated");
    set_output(L, stub, res, "stderr", cmd->stderr_max, "stderr_truncated");

    lua_getfield(L, stub, "timed_out");
    lua_pushboolean(L, lua_toboolean(L, -1));
    lua_setfield(L, res, "timed_out");
    lua_pop(L, 1);

    lua_getfield(L, stub, "duration_ms");
    lua_Integer ms = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    lua_pushinteger(L, ms < 0 ? 0 : ms);
    lua_setfield(L, res, "duration_ms");
}

/* The broker's answer, pushed before the coroutine resumes (it must not
 * raise: hl_lua_async_resume runs it under a protected push). DONE pushes
 * the result table; anything else pushes nil for lua_exec_k to raise. */
/* The HlLuaPushResultFn signature fixes the parameter type. */
// cppcheck-suppress constParameterCallback
static void lua_exec_push(lua_State *L, void *driver)
{
    const HlExecOp *op = driver;
    const HlExecReply *r = op ? op->reply : NULL;
    if (!r || r->kind != HL_EXEC_R_DONE) { lua_pushnil(L); return; }
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, r->status);
    lua_setfield(L, -2, "status");
    if (r->signal[0]) lua_pushstring(L, r->signal);
    else lua_pushnil(L);
    lua_setfield(L, -2, "signal");
    lua_pushlstring(L, (const char *)r->out, r->out_len);
    lua_setfield(L, -2, "stdout");
    lua_pushlstring(L, (const char *)r->err, r->err_len);
    lua_setfield(L, -2, "stderr");
    lua_pushboolean(L, r->timed_out);
    lua_setfield(L, -2, "timed_out");
    lua_pushboolean(L, r->out_cut);
    lua_setfield(L, -2, "stdout_truncated");
    lua_pushboolean(L, r->err_cut);
    lua_setfield(L, -2, "stderr_truncated");
    lua_pushinteger(L, r->duration_ms);
    lua_setfield(L, -2, "duration_ms");
}

static int lua_exec_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status;
    if (!hl_lua_async_k_ok(L))   /* audit 33: kctx may be freed */
        return luaL_error(L, "exec.run: this wait was abandoned");
    const HlExecOp *op = (const HlExecOp *)kctx;
    const HlExecReply *r = op ? op->reply : NULL;
    if (!r) return luaL_error(L, "exec.run: no reply from the command broker");
    if (r->kind == HL_EXEC_R_BUSY)
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "%s", r->msg);
    if (r->kind != HL_EXEC_R_DONE) return luaL_error(L, "%s", r->msg);
    /* The output is charged on the way out, 1 unit per 8 bytes. */
    lua_hlwork(L, (r->out_len + r->err_len) / 8, 0);
    return 1;
}

/* exec.run(name, args?) -> result */
static int lua_exec_run(lua_State *L)
{
    size_t name_len = 0;
    const char *name = luaL_checklstring(L, 1, &name_len);
    int has_args = !lua_isnoneornil(L, 2);
    if (has_args) luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);

    /* The values, read raw (no __index / __len: no app code), and copied
     * into one C block at once: every later step may allocate, and a Lua
     * allocation can run a __gc finalizer - app code that could change the
     * table. */
    const char *args[HL_EXEC_MAX_CALL_ARGS];
    size_t lens[HL_EXEC_MAX_CALL_ARGS];
    size_t n = 0, total = 0;
    if (has_args) {
        lua_Unsigned len = lua_rawlen(L, 2);
        if (len > HL_EXEC_MAX_CALL_ARGS)
            return luaL_error(L, "exec.run: more than %d arguments",
                              HL_EXEC_MAX_CALL_ARGS);
        for (lua_Unsigned i = 1; i <= len; i++) {
            lua_rawgeti(L, 2, (lua_Integer)i);
            if (lua_type(L, -1) != LUA_TSTRING)
                return luaL_error(L, "exec.run: argument %d must be a string",
                                  (int)i);
            args[n] = lua_tolstring(L, -1, &lens[n]);
            total += lens[n];
            n++;
            lua_pop(L, 1);
        }
    }
    if (total > HL_EXEC_MAX_ARG_BYTES)
        return luaL_error(L, "exec.run: the arguments are longer than %u bytes "
                          "together", (unsigned)HL_EXEC_MAX_ARG_BYTES);
    char *block = malloc(total ? total : 1);
    if (!block) return luaL_error(L, "exec.run: out of memory");
    for (size_t i = 0, off = 0; i < n; i++) {
        memcpy(block + off, args[i], lens[i]);
        args[i] = block + off;
        off += lens[i];
    }

    /* The match walks the policy and the arguments: charged first. */
    lua_hlwork(L, 64 + total / 8, 0);

    HlLua *lua = get_hl_lua(L);
    HlExecCmd cmd;
    char err[256];
    int rc = lua && lua->base.exec_policy
           ? hl_exec_check_call(lua->base.exec_policy, lua->base.exec_policy_len,
                                name, name_len, args, lens, n, &cmd, err, sizeof err)
           : HL_EXEC_E_UNKNOWN;
    if (rc == HL_EXEC_E_UNKNOWN) {
        free(block);
        return luaL_error(L, "exec.run: '%s' is not declared in the manifest's "
                          "exec section", name);
    }
    if (rc != HL_EXEC_OK) {
        free(block);
        return luaL_error(L, "exec.run('%s'): %s", name, err);
    }

    /* Under hull test: a stub answers, with no wait (a request test.get
     * dispatches may not wait), after the same match. */
    lua_getfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
    if (lua_istable(L, -1)) {
        free(block);
#ifdef HL_ENABLE_DB
        const char *txn = lua->base.db_registry
                        ? hl_db_registry_open_txn(lua->base.db_registry) : NULL;
        if (txn)
            return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "exec.run cannot wait while "
                                 "a transaction is open on database connection "
                                 "'%s': commit or roll back first", txn);
#endif
        lua_pushvalue(L, 1);
        lua_rawget(L, -2);
        int stub_type = lua_type(L, -1);
        if (stub_type == LUA_TNIL)
            return luaL_error(L, "exec.run: no stub for '%s' in hull test (register "
                              "one with test.exec_stub)", name);
        if (stub_type == LUA_TFUNCTION) {
            /* stub(args, name): a fresh copy of the arguments, so the stub
             * cannot change the caller's table. */
            lua_createtable(L, (int)n, 0);
            for (size_t i = 1; i <= n; i++) {
                lua_rawgeti(L, 2, (lua_Integer)i);
                lua_rawseti(L, -2, (lua_Integer)i);
            }
            lua_pushvalue(L, 1);
            lua_call(L, 2, 1);
            if (!lua_istable(L, -1))
                return luaL_error(L, "exec stub for '%s' must return a result table",
                                  name);
        }
        push_result(L, lua_gettop(L), &cmd);
        return 1;
    }
    lua_pop(L, 1);

    if (!hl_exec_broker_running() || !lua->base.async_ctx) {
        free(block);
        return luaL_error(L, "exec.run('%s'): the command broker is not running "
                          "(this host has none, or it was lost)", name);
    }
    /* The fast admission check; the broker decides (§5.2). */
    if (hl_exec_inflight() >= HL_EXEC_MAX_INFLIGHT ||
        hl_exec_inflight_cmd(name, name_len) >= cmd.concurrency) {
        free(block);
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "exec.run('%s'): too many calls "
                             "in flight", name);
    }
    if (!hl_lua_can_wait(L)) {
        free(block);                          /* before the raise below */
        hl_lua_check_can_wait(L, "exec.run");
        return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "exec.run cannot wait here");
    }

    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.net_ctx,
                                          lua->base.alloc);
    if (!ctx) { free(block); return luaL_error(L, "exec.run: out of memory"); }
    ctx->detached = (lua->active_conn == NULL);
    char why[256];
    if (hl_exec_op_start(ctx, lua->base.async_ctx, name, name_len, args, lens, n,
                         cmd.timeout_ms, why, sizeof why) != 0) {
        free(block);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "exec.run('%s'): %s", name, why);
    }
    free(block);   /* copied into the RUN frame */
    HlExecOp *op = ctx->driver;

    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc, lua_exec_push);
    if (!cont) {
        hl_async_ctx_cancel(ctx);   /* frees the op: KILL */
        return luaL_error(L, "exec.run: out of memory");
    }
    ctx->cont = cont;
    if (!ctx->detached) {
        if (hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)lua->active_conn,
                              (HlSuspendOp *)&ctx->op) < 0) {
            /* As compute.async: no cancel() (dispatch unrefs the handler). */
            ctx->cont->destroy(ctx->cont);
            ctx->cont = NULL;
            hl_async_ctx_cancel(ctx);
            return luaL_error(L, "exec.run: failed to suspend connection");
        }
    } else {
        /* No request holds it: a teardown cancels it, and the op's free
         * sends KILL. */
        hl_async_ctx_track(ctx, &lua->base.async_live);
    }
    return lua_yieldk(L, 0, (lua_KContext)op, lua_exec_k);
}

static const luaL_Reg exec_funcs[] = {
    {"run", lua_exec_run},
    {NULL, NULL}
};

int luaopen_hull_exec(lua_State *L)
{
    hl_lua_newlib(L, exec_funcs);
    return 1;
}

/* test.exec_stub(name, result | fn | nil) */
int hl_lua_exec_stub(lua_State *L)
{
    size_t n = 0;
    const char *name = luaL_checklstring(L, 1, &n);
    int t = lua_type(L, 2);
    if (t != LUA_TTABLE && t != LUA_TFUNCTION && t != LUA_TNIL && t != LUA_TNONE)
        return luaL_error(L, "test.exec_stub(name, result): result is a table, "
                          "a function, or nil to remove the stub");
    HlExecCmd cmd;
    if (!exec_find(get_hl_lua(L), name, n, &cmd))
        return luaL_error(L, "test.exec_stub: '%s' is not declared in the "
                          "manifest's exec section", name);
    lua_settop(L, 2);
    lua_getfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
    if (!lua_istable(L, -1))
        return luaL_error(L, "test.exec_stub is only available in hull test");
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    return 0;
}

void hl_lua_exec_stubs_reset(lua_State *L)
{
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
}
