/* mod_exec.c - hull.exec module: run a command the manifest declares
 * (docs/design/exec_broker.md)
 *
 *   local exec = require("hull.exec")
 *   local r = exec.run("unit-state", { "hs-api.service" })
 *   local r = exec.run("key-import", {}, { stdin = record })
 *   -- r = { status, signal, stdout, stderr, timed_out, stdout_truncated,
 *   --       stderr_truncated, duration_ms }
 *
 * The name must be declared in the manifest's `exec` section, and the
 * arguments must fill its slots (cap/exec.c, hl_exec_check_call - the one
 * matcher). A mismatch raises, naming the argument and the rule. `stdin`
 * (a string) is written to the command's stdin; a non-empty one needs the
 * command's stdin_max, and is never logged.
 *
 * The command broker runs the call (cap/exec_broker.c). Under `hull test`,
 * test.exec_stub(name, result) registers the answer; the call is still
 * matched in full first, so a test catches a bad argument, and the output
 * is cut to the command's stdout_max / stderr_max as the broker cuts it.
 * For exec.stream the stub carries `chunks` (and `on_write`), queued on a
 * stub stream at once (hl_exec_stream_stub), so nothing waits.
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
#include "hull/shared/work_charge.h"
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

/* exec.run(name, args?, opts?) -> result; opts = { stdin = string } */
static int lua_exec_run(lua_State *L)
{
    size_t name_len = 0;
    const char *name = luaL_checklstring(L, 1, &name_len);
    int has_args = !lua_isnoneornil(L, 2);
    if (has_args) luaL_checktype(L, 2, LUA_TTABLE);
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TTABLE);
    lua_settop(L, 3);

    /* opts, read raw (lua_next / lua_rawget: no metamethods): `stdin` is
     * the only key, so a typo is never a silent no-op. */
    const char *in = NULL;
    size_t in_len = 0;
    if (lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING || strcmp(lua_tostring(L, -2), "stdin") != 0)
                return luaL_error(L, "exec.run: unknown option (the only one is stdin)");
            lua_pop(L, 1);
        }
        lua_pushliteral(L, "stdin");
        lua_rawget(L, 3);
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TSTRING)
                return luaL_error(L, "exec.run: stdin must be a string");
            in = lua_tolstring(L, -1, &in_len);
        }
        lua_pop(L, 1);   /* still referenced by opts: the bytes stay valid */
        if (in_len > HL_EXEC_STDIN_CEIL)
            return luaL_error(L, "exec.run: stdin is longer than %d bytes",
                              (int)HL_EXEC_STDIN_CEIL);
    }

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
        return luaL_error(L, "exec.run: the arguments are longer than %d bytes "
                          "together", (int)HL_EXEC_MAX_ARG_BYTES);
    char *block = malloc((total + in_len) ? total + in_len : 1);
    if (!block) return luaL_error(L, "exec.run: out of memory");
    for (size_t i = 0, off = 0; i < n; i++) {
        memcpy(block + off, args[i], lens[i]);
        args[i] = block + off;
        off += lens[i];
    }
    if (in_len) {
        memcpy(block + total, in, in_len);
        in = block + total;
    }

    /* The match walks the policy and the arguments, and stdin is copied
     * twice more (the frame, the broker): charged first. */
    lua_hlwork(L, 64 + (total + in_len) / 8, 0);

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
    if (in_len > cmd.stdin_max) {
        free(block);
        if (!cmd.stdin_max)
            return luaL_error(L, "exec.run('%s'): the command takes no stdin "
                              "(declare stdin_max)", name);
        return luaL_error(L, "exec.run('%s'): stdin is %d bytes; the command's "
                          "stdin_max is %d", name, (int)in_len, (int)cmd.stdin_max);
    }

    /* Under hull test: a stub answers, with no wait (a request test.get
     * dispatches may not wait), after the same match. */
    lua_getfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
    if (lua_istable(L, -1)) {
        int stubs = lua_gettop(L);
        /* the stub's copy of stdin, then nothing of block is needed */
        if (in_len) lua_pushlstring(L, in, in_len); else lua_pushnil(L);
        int stub_in = lua_gettop(L);
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
        lua_rawget(L, stubs);
        int stub_type = lua_type(L, -1);
        if (stub_type == LUA_TNIL)
            return luaL_error(L, "exec.run: no stub for '%s' in hull test (register "
                              "one with test.exec_stub)", name);
        if (stub_type == LUA_TFUNCTION) {
            /* stub(args, name, stdin): a fresh copy of the arguments, so the
             * stub cannot change the caller's table. */
            lua_createtable(L, (int)n, 0);
            for (size_t i = 1; i <= n; i++) {
                lua_rawgeti(L, 2, (lua_Integer)i);
                lua_rawseti(L, -2, (lua_Integer)i);
            }
            lua_pushvalue(L, 1);
            lua_pushvalue(L, stub_in);
            lua_call(L, 3, 1);
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
                         in, in_len, cmd.timeout_ms, why, sizeof why) != 0) {
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

/* ── exec.stream (docs/design/exec_stream.md §4) ───────────────────── */

#define HL_LUA_EXEC_STREAM_MT "hull.exec.stream"

typedef struct {
    HlExecStream *s;           /* one reference; NULL once collected */
} LuaExecStream;               /* user value 1: a stub's on_write (hull test) */

/* Queue a stub's chunks, the array at @p idx ({ fd = "stdout" | "stderr",
 * data = string }, in order). Raises on a malformed one. */
static void stub_push_chunks(lua_State *L, HlExecStream *s, int idx, const char *name)
{
    idx = lua_absindex(L, idx);
    lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        if (!lua_istable(L, -1))
            luaL_error(L, "exec stub for '%s': chunk %d is not a table", name, (int)i);
        lua_getfield(L, -1, "fd");
        const char *fd = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
        int f = fd && strcmp(fd, "stdout") == 0 ? 1 : fd && strcmp(fd, "stderr") == 0 ? 2 : 0;
        lua_pop(L, 1);
        if (!f)
            luaL_error(L, "exec stub for '%s': chunk %d fd must be \"stdout\" or "
                       "\"stderr\"", name, (int)i);
        lua_getfield(L, -1, "data");
        size_t len = 0;
        const char *data = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : NULL;
        if (!data) luaL_error(L, "exec stub for '%s': chunk %d data must be a string", name, (int)i);
        if (hl_exec_stream_stub_chunk(s, f, data, len) != 0)
            luaL_error(L, "exec.stream: out of memory");
        lua_pop(L, 2);
    }
}

static LuaExecStream *check_stream(lua_State *L)
{
    LuaExecStream *o = luaL_checkudata(L, 1, HL_LUA_EXEC_STREAM_MT);
    if (!o->s) luaL_error(L, "exec.stream: the stream is gone");
    return o;
}

/* The push of a parked stream wait: nothing (the continuation reads the
 * stream again, with the error checks a push may not make). */
// cppcheck-suppress constParameterCallback
static void lua_stream_push(lua_State *L, void *driver)
{
    (void)driver;
    lua_pushnil(L);
}

/* Park the running coroutine on @p s until @p kind is ready, continuing in
 * @p k. Raises (never returns) when the wait is refused. */
static int stream_park(lua_State *L, HlExecStream *s, int kind, const char *what,
                       lua_KFunction k)
{
    HlLua *lua = get_hl_lua(L);
    if (!hl_lua_can_wait(L)) {
        hl_lua_check_can_wait(L, what);
        return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "%s cannot wait here", what);
    }
    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.net_ctx,
                                          lua->base.alloc);
    if (!ctx) return luaL_error(L, "%s: out of memory", what);
    ctx->detached = (lua->active_conn == NULL);
    if (hl_exec_stream_wait(ctx, s, kind) != 0) {
        hl_async_ctx_free(ctx);
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "%s: another %s of this stream "
                             "is already waiting", what, what);
    }
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc, lua_stream_push);
    if (!cont) {
        hl_async_ctx_cancel(ctx);   /* the wait's free closes the stream */
        return luaL_error(L, "%s: out of memory", what);
    }
    ctx->cont = cont;
    if (!ctx->detached) {
        if (hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)lua->active_conn,
                              (HlSuspendOp *)&ctx->op) < 0) {
            ctx->cont->destroy(ctx->cont);
            ctx->cont = NULL;
            hl_async_ctx_cancel(ctx);
            return luaL_error(L, "%s: failed to suspend connection", what);
        }
    } else {
        hl_async_ctx_track(ctx, &lua->base.async_live);
    }
    return lua_yieldk(L, 0, 0, k);
}

/* The next chunk as { fd = "stdout" | "stderr", data = string }, charged
 * before it is taken (a raise then leaves it queued), or 0 pushed when none
 * is queued. */
static int take_chunk(lua_State *L, HlExecStream *s)
{
    size_t len = hl_exec_stream_peek(s);
    if (len == (size_t)-1) return 0;
    lua_hlwork(L, HL_WORK_EXEC_CHUNK_UNITS + len / 8, 0);
    HlExecChunk *c = hl_exec_stream_take(s);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, c->fd == 2 ? "stderr" : "stdout");
    lua_setfield(L, -2, "fd");
    lua_pushlstring(L, (const char *)c->data, c->len);
    free(c);                    /* after the copy; a raise above leaks nothing */
    lua_setfield(L, -2, "data");
    return 1;
}

static int lua_stream_read_k(lua_State *L, int status, lua_KContext kctx);

/* s:read() -> { fd, data } | nil once both pipes are closed */
static int lua_stream_read(lua_State *L)
{
    LuaExecStream *o = check_stream(L);
    if (take_chunk(L, o->s)) return 1;
    if (hl_exec_stream_drained(o->s)) { lua_pushnil(L); return 1; }
    lua_settop(L, 1);
    return stream_park(L, o->s, HL_EXEC_WAIT_READ, "exec.stream read", lua_stream_read_k);
}

static int lua_stream_read_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status; (void)kctx;
    if (!hl_lua_async_k_ok(L)) return luaL_error(L, "exec.stream: this wait was abandoned");
    LuaExecStream *o = check_stream(L);
    if (take_chunk(L, o->s)) return 1;
    lua_pushnil(L);
    return 1;
}

static int lua_stream_write_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status; (void)kctx;
    if (!hl_lua_async_k_ok(L)) return luaL_error(L, "exec.stream: this wait was abandoned");
    LuaExecStream *o = check_stream(L);
    if (!hl_exec_stream_write_ok(o->s))
        return luaL_error(L, "exec.stream write: the command's stdin closed before "
                          "the bytes were written (stdin_closed)");
    lua_pushboolean(L, 1);
    return 1;
}

/* s:write(bytes) -> true once the broker has them (parks for credit) */
static int lua_stream_write(lua_State *L)
{
    LuaExecStream *o = check_stream(L);
    size_t n = 0;
    const char *p = luaL_checklstring(L, 2, &n);
    lua_hlwork(L, 64 + n / 8, 0);
    char why[192];
    const char *code = NULL;
    if (hl_exec_stream_is_stub(o->s)) {
        /* hull test: the stub's on_write(bytes) may return more chunks. */
        if (hl_exec_stream_stub_write(o->s, n, why, sizeof why, &code) != 0)
            return luaL_error(L, "%s (%s)", why, code);
        lua_getiuservalue(L, 1, 1);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 2);
            lua_call(L, 1, 1);
            if (lua_istable(L, -1)) stub_push_chunks(L, o->s, -1, "on_write");
            else if (!lua_isnil(L, -1))
                return luaL_error(L, "exec stub on_write must return an array of chunks or nil");
        }
        if (!o->s) return luaL_error(L, "exec.stream: the stream is gone");
        lua_pushboolean(L, 1);
        return 1;
    }
    /* A write that will have to wait is refused before anything is queued:
     * refused after, its bytes still went out (review of PR 2, L1). */
    if (hl_exec_stream_write_would_wait(o->s, n) && !hl_lua_can_wait(L)) {
        hl_lua_check_can_wait(L, "exec.stream write");
        return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "exec.stream write cannot wait here");
    }
    int rc = hl_exec_stream_write(o->s, p, n, why, sizeof why, &code);
    if (rc < 0) {
        if (strcmp(code, "busy") == 0) return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "%s", why);
        return luaL_error(L, "%s (%s)", why, code);
    }
    if (rc == 1) { lua_pushboolean(L, 1); return 1; }
    lua_settop(L, 1);
    return stream_park(L, o->s, HL_EXEC_WAIT_WRITE, "exec.stream write", lua_stream_write_k);
}

/* s:close_write(): EOF on the command's stdin once queued bytes are out */
static int lua_stream_close_write(lua_State *L)
{
    LuaExecStream *o = check_stream(L);
    char why[192];
    if (hl_exec_stream_close_write(o->s, why, sizeof why) != 0)
        return luaL_error(L, "%s", why);
    return 0;
}

/* s:close(): kill the command; later reads return nil. Idempotent. */
static int lua_stream_close(lua_State *L)
{
    LuaExecStream *o = luaL_checkudata(L, 1, HL_LUA_EXEC_STREAM_MT);
    if (o->s) hl_exec_stream_close(o->s);
    return 0;
}

static int push_stream_result(lua_State *L, const HlExecReply *r)
{
    if (r->kind == HL_EXEC_R_BUSY) return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "%s", r->msg);
    if (r->kind != HL_EXEC_R_DONE) return luaL_error(L, "%s", r->msg);
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, r->status);
    lua_setfield(L, -2, "status");
    if (r->signal[0]) lua_pushstring(L, r->signal); else lua_pushnil(L);
    lua_setfield(L, -2, "signal");
    lua_pushboolean(L, r->timed_out);
    lua_setfield(L, -2, "timed_out");
    lua_pushboolean(L, r->out_cut);
    lua_setfield(L, -2, "stdout_truncated");
    lua_pushboolean(L, r->err_cut);
    lua_setfield(L, -2, "stderr_truncated");
    lua_pushinteger(L, (lua_Integer)r->out_bytes);
    lua_setfield(L, -2, "stdout_length");
    lua_pushinteger(L, (lua_Integer)r->err_bytes);
    lua_setfield(L, -2, "stderr_length");
    lua_pushinteger(L, r->duration_ms);
    lua_setfield(L, -2, "duration_ms");
    return 1;
}

static int lua_stream_result_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status; (void)kctx;
    if (!hl_lua_async_k_ok(L)) return luaL_error(L, "exec.stream: this wait was abandoned");
    LuaExecStream *o = check_stream(L);
    const HlExecReply *r = hl_exec_stream_result(o->s);
    if (!r) return luaL_error(L, "exec.stream: no result from the command broker");
    return push_stream_result(L, r);
}

/* s:result() -> the result once the command has ended (parks) */
static int lua_stream_result(lua_State *L)
{
    LuaExecStream *o = check_stream(L);
    const HlExecReply *r = hl_exec_stream_result(o->s);
    if (r) return push_stream_result(L, r);
    lua_settop(L, 1);
    return stream_park(L, o->s, HL_EXEC_WAIT_RESULT, "exec.stream result",
                       lua_stream_result_k);
}

/* __gc / __close: an abandoned stream never holds its slot. */
static int lua_stream_gc(lua_State *L)
{
    LuaExecStream *o = luaL_checkudata(L, 1, HL_LUA_EXEC_STREAM_MT);
    if (o->s) {
        hl_exec_stream_close(o->s);
        hl_exec_stream_release(o->s);
        o->s = NULL;
    }
    return 0;
}

static const luaL_Reg stream_methods[] = {
    {"read", lua_stream_read},
    {"write", lua_stream_write},
    {"close_write", lua_stream_close_write},
    {"close", lua_stream_close},
    {"result", lua_stream_result},
    {NULL, NULL}
};

/* Under hull test: the stream test.exec_stub registered for @p name (the
 * stubs table at @p stubs), after the same match. Its chunks and result are
 * queued at once, so nothing waits (a request test.get dispatches may not).
 * Frees @p block. */
static int stub_stream(lua_State *L, int stubs, const char *name, size_t n, char *block,
                       const char *in, size_t in_len, int stdin_open, const HlExecCmd *cmd)
{
    if (in_len) lua_pushlstring(L, in, in_len); else lua_pushnil(L);
    int stub_in = lua_gettop(L);
    free(block);
#ifdef HL_ENABLE_DB
    HlLua *lua = get_hl_lua(L);
    const char *txn = lua->base.db_registry ? hl_db_registry_open_txn(lua->base.db_registry)
                                            : NULL;
    if (txn)
        return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "exec.stream cannot wait while a "
                             "transaction is open on database connection '%s': commit "
                             "or roll back first", txn);
#endif
    lua_pushvalue(L, 1);
    lua_rawget(L, stubs);
    int t = lua_type(L, -1);
    if (t == LUA_TNIL)
        return luaL_error(L, "exec.stream: no stub for '%s' in hull test (register one "
                          "with test.exec_stub)", name);
    if (t == LUA_TFUNCTION) {
        /* stub(args, name, stdin): a fresh copy of the arguments */
        lua_createtable(L, (int)n, 0);
        for (size_t i = 1; i <= n; i++) {
            lua_rawgeti(L, 2, (lua_Integer)i);
            lua_rawseti(L, -2, (lua_Integer)i);
        }
        lua_pushvalue(L, 1);
        lua_pushvalue(L, stub_in);
        lua_call(L, 3, 1);
    }
    if (!lua_istable(L, -1))
        return luaL_error(L, "exec stub for '%s' must be (or return) a result table", name);
    int stub = lua_gettop(L);

    LuaExecStream *o = lua_newuserdatauv(L, sizeof *o, 1);
    o->s = NULL;
    luaL_setmetatable(L, HL_LUA_EXEC_STREAM_MT);
    int ud = lua_gettop(L);
    o->s = hl_exec_stream_stub(stdin_open, cmd->stream_stdin_max, in_len,
                               cmd->stream_stdout_max, cmd->stream_stderr_max);
    if (!o->s)
        return luaL_error(L, "exec.stream('%s'): stream stubs need a host with the "
                          "command broker (Linux, macOS)", name);
    lua_getfield(L, stub, "chunks");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            return luaL_error(L, "exec stub for '%s': chunks must be an array", name);
        stub_push_chunks(L, o->s, -1, name);
    }
    lua_pop(L, 1);
    lua_getfield(L, stub, "status");
    lua_Integer status = lua_isnil(L, -1) ? 0 : luaL_checkinteger(L, -1);
    lua_getfield(L, stub, "signal");
    const char *sig = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
    lua_getfield(L, stub, "timed_out");
    int timed_out = lua_toboolean(L, -1);
    lua_getfield(L, stub, "duration_ms");
    lua_Integer ms = lua_isnil(L, -1) ? 0 : luaL_checkinteger(L, -1);
    if (hl_exec_stream_stub_finish(o->s, (int32_t)status, sig, timed_out,
                                   (uint32_t)(ms < 0 ? 0 : ms)) != 0)
        return luaL_error(L, "exec.stream: out of memory");
    lua_pop(L, 4);
    lua_getfield(L, stub, "on_write");
    if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
        return luaL_error(L, "exec stub for '%s': on_write must be a function", name);
    lua_setiuservalue(L, ud, 1);
    lua_settop(L, ud);
    return 1;
}

/* exec.stream(name, args?, opts?) -> stream; opts = { stdin, stdin_open } */
static int lua_exec_stream(lua_State *L)
{
    size_t name_len = 0;
    const char *name = luaL_checklstring(L, 1, &name_len);
    int has_args = !lua_isnoneornil(L, 2);
    if (has_args) luaL_checktype(L, 2, LUA_TTABLE);
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TTABLE);
    lua_settop(L, 3);

    const char *in = NULL;
    size_t in_len = 0;
    int stdin_open = 0;
    if (lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
            if (!k || (strcmp(k, "stdin") != 0 && strcmp(k, "stdin_open") != 0))
                return luaL_error(L, "exec.stream: unknown option (stdin, stdin_open)");
            lua_pop(L, 1);
        }
        lua_pushliteral(L, "stdin");
        lua_rawget(L, 3);
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TSTRING)
                return luaL_error(L, "exec.stream: stdin must be a string");
            in = lua_tolstring(L, -1, &in_len);
        }
        lua_pop(L, 1);
        lua_pushliteral(L, "stdin_open");
        lua_rawget(L, 3);
        if (!lua_isnil(L, -1) && !lua_isboolean(L, -1))
            return luaL_error(L, "exec.stream: stdin_open must be a boolean");
        stdin_open = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (in_len > HL_EXEC_STDIN_CEIL)
            return luaL_error(L, "exec.stream: stdin is longer than %d bytes (write "
                              "the rest)", (int)HL_EXEC_STDIN_CEIL);
    }
    const char *args[HL_EXEC_MAX_CALL_ARGS];
    size_t lens[HL_EXEC_MAX_CALL_ARGS];
    size_t n = 0, total = 0;
    if (has_args) {
        lua_Unsigned len = lua_rawlen(L, 2);
        if (len > HL_EXEC_MAX_CALL_ARGS)
            return luaL_error(L, "exec.stream: more than %d arguments", HL_EXEC_MAX_CALL_ARGS);
        for (lua_Unsigned i = 1; i <= len; i++) {
            lua_rawgeti(L, 2, (lua_Integer)i);
            if (lua_type(L, -1) != LUA_TSTRING)
                return luaL_error(L, "exec.stream: argument %d must be a string", (int)i);
            args[n] = lua_tolstring(L, -1, &lens[n]);
            total += lens[n];
            n++;
            lua_pop(L, 1);
        }
    }
    if (total > HL_EXEC_MAX_ARG_BYTES)
        return luaL_error(L, "exec.stream: the arguments are longer than %d bytes "
                          "together", (int)HL_EXEC_MAX_ARG_BYTES);
    /* Copied at once into one C block, as exec.run does (a later allocation
     * may run a __gc that changes the tables). */
    char *block = malloc((total + in_len) ? total + in_len : 1);
    if (!block) return luaL_error(L, "exec.stream: out of memory");
    for (size_t i = 0, off = 0; i < n; i++) {
        memcpy(block + off, args[i], lens[i]);
        args[i] = block + off;
        off += lens[i];
    }
    if (in_len) { memcpy(block + total, in, in_len); in = block + total; }
    lua_hlwork(L, 64 + (total + in_len) / 8, 0);

    HlLua *lua = get_hl_lua(L);
    HlExecCmd cmd;
    char err[256];
    int rc = lua && lua->base.exec_policy
           ? hl_exec_check_call(lua->base.exec_policy, lua->base.exec_policy_len,
                                name, name_len, args, lens, n, &cmd, err, sizeof err)
           : HL_EXEC_E_UNKNOWN;
    const char *fail = NULL;
    char msg[320];
    if (rc == HL_EXEC_E_UNKNOWN) {
        snprintf(msg, sizeof msg, "exec.stream: '%s' is not declared in the manifest's "
                 "exec section", name);
        fail = msg;
    } else if (rc != HL_EXEC_OK) {
        snprintf(msg, sizeof msg, "exec.stream('%s'): %s", name, err);
        fail = msg;
    } else if (!(cmd.flags & HL_EXEC_CMD_STREAM)) {
        snprintf(msg, sizeof msg, "exec.stream('%s'): not a stream command (declare "
                 "stream = true; exec.run runs it)", name);
        fail = msg;
    } else if ((in_len || stdin_open) && cmd.stream_stdin_max == 0) {
        snprintf(msg, sizeof msg, "exec.stream('%s'): the command takes no stdin "
                 "(declare stdin_max)", name);
        fail = msg;
    } else if (in_len > cmd.stream_stdin_max) {
        snprintf(msg, sizeof msg, "exec.stream('%s'): stdin is longer than its "
                 "stdin_max", name);
        fail = msg;
    }
    if (fail) { free(block); return luaL_error(L, "%s", fail); }

    lua_getfield(L, LUA_REGISTRYINDEX, EXEC_STUBS_KEY);
    if (lua_istable(L, -1))
        return stub_stream(L, lua_gettop(L), name, n, block, in, in_len, stdin_open, &cmd);
    lua_pop(L, 1);
    if (!hl_exec_broker_running() || !lua->base.async_ctx) {
        free(block);
        return luaL_error(L, "exec.stream('%s'): the command broker is not running "
                          "(this host has none, or it was lost)", name);
    }
    if (hl_exec_inflight() >= HL_EXEC_MAX_INFLIGHT ||
        hl_exec_inflight_cmd(name, name_len) >= cmd.concurrency) {
        free(block);
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "exec.stream('%s'): too many "
                             "calls in flight", name);
    }
    /* Its reads will wait: refused where a wait is (a transaction open, the
     * top level, middleware). */
    if (!hl_lua_can_wait(L)) {
        free(block);
        hl_lua_check_can_wait(L, "exec.stream");
        return hl_lua_refuse(L, HL_LUA_REFUSAL_WAIT "exec.stream cannot wait here");
    }
    LuaExecStream *o = lua_newuserdatauv(L, sizeof *o, 1);
    o->s = NULL;
    luaL_setmetatable(L, HL_LUA_EXEC_STREAM_MT);
    char why[256];
    o->s = hl_exec_stream_start(lua->base.async_ctx, name, name_len, args, lens, n,
                                in, in_len, stdin_open, cmd.stream_stdin_max,
                                why, sizeof why);
    free(block);
    if (!o->s) return luaL_error(L, "exec.stream('%s'): %s", name, why);
    return 1;
}

static const luaL_Reg exec_funcs[] = {
    {"run", lua_exec_run},
    {"stream", lua_exec_stream},
    {NULL, NULL}
};

int luaopen_hull_exec(lua_State *L)
{
    /* The stream metatable: methods through the dispatcher, a finalizer
     * that closes an abandoned stream, locked (with __gc reachable a stream
     * could be freed under a parked read). */
    luaL_newmetatable(L, HL_LUA_EXEC_STREAM_MT);
    lua_pushcfunction(L, lua_stream_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, lua_stream_gc);
    lua_setfield(L, -2, "__close");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    hl_lua_newlib(L, stream_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
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
