/*
 * mod_ssh.c: the private bridge between the SSH stdlib and the outbound
 * byte stream (cap/net_stream.c).
 *
 * Exposes an internal module `hull.ssh._stream` with one entry, connect{...},
 * returning a stream userdata carrying read / write / close. The SSH protocol
 * itself is Lua (stdlib/lua/hull/ssh/); this file moves bytes and enforces the
 * grant, and knows nothing about SSH framing.
 *
 * TWO gates, and they do different jobs:
 *
 *   1. The POLICY gate (hl_ssh_check_connect) bounds WHERE a connection may go
 *      and as WHOM, from the manifest. It does not care who is calling, so it
 *      holds even if this module is reached directly.
 *
 *   2. The CALLER gate (stdlib-only) is what makes "the application never holds
 *      a socket" true. Without it an app could reach an already-permitted host
 *      and speak something other than SSH over it. The policy alone would allow
 *      that, because the destination is genuinely granted.
 *
 * The caller gate rests on chunk names, which is sound because a chunk name
 * cannot be forged: the one bridge that compiles caller-supplied source builds
 * its chunk name rather than accepting one (mod_template.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"        /* get_hl_lua, luaopen decls */

#ifdef HL_ENABLE_HTTP

#include "internal.h"          /* hl_lua_source_is_stdlib */

#include "hull/cap/net_policy.h"
#include "hull/cap/net_stream.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"   /* hl_net_op_suspend / _complete, HlSuspendOp */

/* Declared at its call sites rather than in a header, matching
 * mod_http_client.c and mod_compute.c. */
extern HlAsyncCont *hl_lua_async_cont_create(HlLua *lua, HlAllocator *alloc,
                                             HlLuaPushResultFn push_result);

#include <stdlib.h>
#include <string.h>

#define HL_LUA_SSH_MT "hull.ssh.stream"

/* Default connect budget. A fleet tool waits on machines that may be asleep,
 * so this is generous; an opts.timeout_ms overrides it per call. */
#define SSH_CONNECT_MS_DEF 10000

/* Uservalue 1 holds the buffer a parked write is retrying, anchoring it
 * against collection: a resumed coroutine does not get its arguments back, so
 * the bytes have to be owned by something that survives the yield. */
#define SSH_UV_WRITE_BUF 1

typedef struct {
    HlNetStream *s;     /* owned; NULL after close (idempotent, __gc-safe) */
    HlAsyncCtx  *ctx;   /* live only while parked */
    size_t       want;  /* size a parked read is retrying */
} HlLuaSshStream;

/* ── gates ──────────────────────────────────────────────────────────── */

/* The immediate Lua caller must be stdlib. Level 1 is the caller of this C
 * function; a C frame (level 0) is this one. */
static int caller_is_stdlib(lua_State *L)
{
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) == 0) return 0;
    if (lua_getinfo(L, "S", &ar) == 0) return 0;
    return hl_lua_source_is_stdlib(ar.source);
}

/* ── the stream handle ──────────────────────────────────────────────── */

static HlLuaSshStream *check_stream(lua_State *L)
{
    return luaL_checkudata(L, 1, HL_LUA_SSH_MT);
}

/* Push (nil, message) - the Lua convention this binding returns errors in, so
 * a transport failure is data the protocol code can act on rather than a raise
 * that unwinds a half-open connection. */
static int push_err(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

static int lua_ssh_close(lua_State *L)
{
    HlLuaSshStream *o = check_stream(L);
    if (o->s) {
        hl_net_stream_set_user(o->s, NULL);   /* drop before the stream goes */
        hl_net_stream_close(o->s);
        hl_net_stream_free(o->s);
        o->s = NULL;
    }
    return 0;
}

static int lua_ssh_gc(lua_State *L)
{
    HlLuaSshStream *o = luaL_checkudata(L, 1, HL_LUA_SSH_MT);
    if (o->s) {
        hl_net_stream_set_user(o->s, NULL);
        hl_net_stream_close(o->s);
        hl_net_stream_free(o->s);
        o->s = NULL;
    }
    return 0;
}

/* ── parking ────────────────────────────────────────────────────────── */

/* The loop finished what the stream was waiting on. Walk op -> stream -> this
 * binding's state (the seam in cap/net_stream.h exists for exactly this) and
 * hand the coroutine back to the runtime. */
static void ssh_on_resume(HlAsyncOp *op)
{
    HlNetStream *s = hl_net_stream_from_op(op);
    HlLuaSshStream *o = s ? hl_net_stream_user(s) : NULL;
    if (!o || !o->ctx) return;

    HlAsyncCtx *ctx = o->ctx;
    o->ctx = NULL;                 /* one resume per park */

    /* Same split as worker_wasm.c: a detached (app.main) caller has no
     * connection to hand back, an attached one does. */
    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
}

/* Park the calling coroutine on the stream's pending op. Returns 0 on success,
 * -1 if the park could not be armed - the caller must then report an error
 * rather than yield, because nothing would ever resume it. */
static int ssh_park(HlLua *lua, HlLuaSshStream *o)
{
    HlAsyncOp *op = hl_net_stream_pending_op(o->s);
    if (!op) return -1;            /* nothing pending: resuming would hang */

    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.net_ctx,
                                          lua->base.alloc);
    if (!ctx) return -1;

    /* The continuation pushes nothing: each entry point re-reads the stream
     * after resuming, so the result comes from the retry rather than being
     * smuggled across the yield. */
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc, NULL);
    if (!cont) {
        hl_async_ctx_free(ctx);
        return -1;
    }
    ctx->cont     = cont;
    ctx->detached = (lua->active_conn == NULL);

    if (!ctx->detached &&
        hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)lua->active_conn,
                          (HlSuspendOp *)&ctx->op) < 0) {
        cont->destroy(cont);
        hl_async_ctx_free(ctx);
        return -1;
    }

    o->ctx = ctx;
    hl_net_stream_set_user(o->s, o);
    op->on_resume = ssh_on_resume;
    op->on_cancel = ssh_on_resume;   /* a cancel must still un-park the coro */
    return 0;
}

/* ── read / write ───────────────────────────────────────────────────── */

/* Both entry points are shaped the same way: try, and on AGAIN park and retry
 * the identical operation after resuming. The retry is safe because read
 * consumes nothing when it reports AGAIN and write is all-or-none, so neither
 * can half-apply and then repeat. */

static int ssh_read_step(lua_State *L, HlLuaSshStream *o);

static int lua_ssh_read_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status;
    return ssh_read_step(L, (HlLuaSshStream *)kctx);
}

static int ssh_read_step(lua_State *L, HlLuaSshStream *o)
{
    if (!o->s) return push_err(L, "closed");

    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, o->want);
    long n  = hl_net_stream_read(o->s, p, o->want);

    if (n >= 0) {
        luaL_pushresultsize(&b, (size_t)n);   /* 0 is a clean EOF */
        return 1;
    }
    luaL_pushresultsize(&b, 0);
    lua_pop(L, 1);                            /* drop the empty result */

    if (n != HL_NET_E_AGAIN)
        return push_err(L, hl_net_stream_strerror((int)n));

    HlLua *lua = get_hl_lua(L);
    if (!lua || ssh_park(lua, o) != 0)
        return push_err(L, "cannot wait for data");
    return lua_yieldk(L, 0, (lua_KContext)o, lua_ssh_read_k);
}

static int lua_ssh_read(lua_State *L)
{
    HlLuaSshStream *o = check_stream(L);
    lua_Integer want = luaL_optinteger(L, 2, 4096);
    if (want <= 0) return push_err(L, "read size must be positive");

    o->want = (size_t)want;
    return ssh_read_step(L, o);
}

static int ssh_write_step(lua_State *L, HlLuaSshStream *o, int ud_idx);

static int lua_ssh_write_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status;
    /* The userdata is back at the bottom of the stack on resume. */
    return ssh_write_step(L, (HlLuaSshStream *)kctx, 1);
}

static int ssh_write_step(lua_State *L, HlLuaSshStream *o, int ud_idx)
{
    if (!o->s) return push_err(L, "closed");

    if (lua_getiuservalue(L, ud_idx, SSH_UV_WRITE_BUF) != LUA_TSTRING) {
        lua_pop(L, 1);
        return push_err(L, "nothing to write");
    }
    size_t len;
    const char *data = lua_tolstring(L, -1, &len);
    int rc = hl_net_stream_write(o->s, data, len);
    lua_pop(L, 1);                            /* the anchored buffer */

    if (rc == HL_NET_OK) {
        lua_pushnil(L);
        lua_setiuservalue(L, ud_idx, SSH_UV_WRITE_BUF);   /* release it */
        lua_pushboolean(L, 1);
        return 1;
    }
    if (rc != HL_NET_E_AGAIN)
        return push_err(L, hl_net_stream_strerror(rc));

    HlLua *lua = get_hl_lua(L);
    if (!lua || ssh_park(lua, o) != 0)
        return push_err(L, "cannot wait for send capacity");
    return lua_yieldk(L, 0, (lua_KContext)o, lua_ssh_write_k);
}

static int lua_ssh_write(lua_State *L)
{
    HlLuaSshStream *o = check_stream(L);
    size_t len;
    luaL_checklstring(L, 2, &len);
    if (!len) { lua_pushboolean(L, 1); return 1; }

    /* Anchor the bytes on the handle so a parked retry still has them. */
    lua_pushvalue(L, 2);
    lua_setiuservalue(L, 1, SSH_UV_WRITE_BUF);
    return ssh_write_step(L, o, 1);
}

/* ── connect ────────────────────────────────────────────────────────── */

static int ssh_connect_step(lua_State *L, HlLuaSshStream *o);

static int lua_ssh_connect_k(lua_State *L, int status, lua_KContext kctx)
{
    (void)status;
    return ssh_connect_step(L, (HlLuaSshStream *)kctx);
}

static int ssh_connect_step(lua_State *L, HlLuaSshStream *o)
{
    int rc = hl_net_stream_connect_result(o->s);
    if (rc == HL_NET_OK) return 1;             /* the handle is on the stack */

    if (rc == HL_NET_E_AGAIN) {
        HlLua *lua = get_hl_lua(L);
        if (!lua || ssh_park(lua, o) != 0)
            return push_err(L, "cannot wait for the connection");
        return lua_yieldk(L, 0, (lua_KContext)o, lua_ssh_connect_k);
    }

    /* Terminal failure. Release the stream now rather than leaving a dead
     * handle for __gc: the caller is getting an error, not a connection. */
    hl_net_stream_set_user(o->s, NULL);
    hl_net_stream_free(o->s);
    o->s = NULL;
    return push_err(L, hl_net_stream_strerror(rc));
}

/*
 * _stream.connect{ host=, port=, user=, timeout_ms= } -> handle | nil, err
 *
 * Refused unless the caller is stdlib AND the manifest grants this exact
 * destination and login. The order matters: the caller check first, so an
 * application probing this bridge learns nothing about the grant.
 */
static int lua_ssh_connect(lua_State *L)
{
    if (!caller_is_stdlib(L))
        return luaL_error(L,
            "ssh: the byte stream is internal to the SSH module; "
            "use require('hull.ssh')");

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "host");
    const char *host = lua_tostring(L, -1);
    lua_getfield(L, 1, "port");
    int port = (int)luaL_optinteger(L, -1, 22);
    lua_getfield(L, 1, "user");
    const char *user = lua_tostring(L, -1);
    lua_getfield(L, 1, "timeout_ms");
    int timeout = (int)luaL_optinteger(L, -1, SSH_CONNECT_MS_DEF);

    HlLua *lua = get_hl_lua(L);
    if (!lua) return push_err(L, "no runtime");

    /* The grant, before any name resolution and before a socket exists. */
    HlNetAuth auth = hl_ssh_check_connect(lua->base.ssh_policy, host, port, user);
    if (auth != HL_NET_ALLOW) {
        lua_pop(L, 4);
        return push_err(L, hl_cap_net_auth_reason(auth));
    }

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = lua->base.async_ctx;
    cfg.pool       = lua->base.thread_pool;
    cfg.host       = host;
    cfg.port       = port;
    cfg.connect_ms = timeout;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    lua_pop(L, 4);                       /* the four option lookups */

    if (!s) return push_err(L, hl_net_stream_strerror(rc));

    HlLuaSshStream *o = lua_newuserdatauv(L, sizeof *o, 1);
    memset(o, 0, sizeof *o);
    o->s    = s;
    o->want = 4096;
    luaL_setmetatable(L, HL_LUA_SSH_MT);
    hl_net_stream_set_user(s, o);

    return ssh_connect_step(L, o);
}

/* ── registration ───────────────────────────────────────────────────── */

static const luaL_Reg ssh_stream_methods[] = {
    {"read",  lua_ssh_read},
    {"write", lua_ssh_write},
    {"close", lua_ssh_close},
    {NULL, NULL}
};

static const luaL_Reg ssh_module[] = {
    {"connect", lua_ssh_connect},
    {NULL, NULL}
};

int luaopen_hull_ssh_stream(lua_State *L)
{
    /* Handle metatable: __index = methods, __gc = finalizer. A dropped handle
     * closes its stream, so an abandoned connection never leaks a descriptor. */
    luaL_newmetatable(L, HL_LUA_SSH_MT);
    lua_pushcfunction(L, lua_ssh_gc);
    lua_setfield(L, -2, "__gc");
    luaL_newlib(L, ssh_stream_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newlib(L, ssh_module);
    return 1;
}

#else  /* !HL_ENABLE_HTTP */

/* Pure-compute: cap/net_stream.c is not in the build, so there is nothing to
 * bind. The module is never registered either (modules.c), and the resolver
 * refuses `hull/ssh@1` on this build, so an app gets a clear message rather
 * than a link error. A TU has to declare something, hence the typedef. */
typedef int hl_mod_ssh_unused;

#endif /* HL_ENABLE_HTTP */
