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
 * ## Reaching a host through a relay
 *
 * `via = { host, port, tls }` dials somewhere OTHER than the SSH destination
 * and hands back the bytes, which is how a host behind a WebSocket-over-TLS
 * tunnel (Cloudflare Access, say) is reached. The WebSocket framing is not
 * here - it is pure Lua in hull.web.ws-stream, layered on the stream this
 * returns, because it is a byte transform with no authority in it.
 *
 * The POLICY gate then checks BOTH, and both must pass:
 *
 *   - hl_ssh_check_connect on the SSH destination, exactly as before, so a
 *     relay never widens which machine may be reached or as which login
 *   - hl_ssh_check_tunnel on the relay, because that is the machine a socket
 *     is actually opened to
 *
 * One list could not express this. The destination travels inside the
 * tunnel's own headers and never appears in the socket address, so a single
 * allowlist naming the relay would have authorised SSH to every host behind
 * it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"        /* get_hl_lua, luaopen decls */

#ifdef HL_ENABLE_HTTP_CLIENT

#include "internal.h"          /* hl_lua_source_is_stdlib */

#include "hull/cap/net_policy.h"
#include "hull/cap/net_stream.h"
#include "hull/tls_transport.h"  /* HlClientTls: the host's resolved trust */
#include "hull/utils/alloc.h"    /* hl_alloc_kl */
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

/* Hand a parked coroutine back, once.
 *
 * Declared before close because close has to use it: a read parked on this
 * stream is waiting for a wake-up that closing will never deliver, and
 * clearing the user pointer first - which is what stops ssh_on_resume from
 * touching a stream that is going away - also stopped it from un-parking
 * anybody. The coroutine then waited forever and its context leaked.
 *
 * Called with the stream still alive and already marked closing, so the
 * retry the coroutine performs reads the closed state and returns an error
 * rather than parking again. */
static void ssh_unpark(HlLuaSshStream *o)
{
    HlAsyncCtx *ctx = o->ctx;
    if (!ctx) return;
    o->ctx = NULL;                 /* one resume per park */

    /* Same split as worker_wasm.c: a detached (app.main) caller has no
     * connection to hand back, an attached one does. */
    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
}

static int lua_ssh_close(lua_State *L)
{
    HlLuaSshStream *o = check_stream(L);
    if (o->s) {
        /* Order is the whole content of this function. Close FIRST so the
         * stream reports closed, THEN un-park so the resumed coroutine's
         * retry sees that and returns "connection closed" instead of parking
         * again, and only then drop the user pointer and release. */
        hl_net_stream_close(o->s);
        ssh_unpark(o);
        hl_net_stream_set_user(o->s, NULL);
        hl_net_stream_free(o->s);
        o->s = NULL;
    }
    return 0;
}

static int lua_ssh_gc(lua_State *L)
{
    HlLuaSshStream *o = luaL_checkudata(L, 1, HL_LUA_SSH_MT);
    if (o->s) {
        /* NOT ssh_unpark here: resuming a coroutine from a finalizer is not
         * something the runtime guarantees. It is also unnecessary - while a
         * read is parked, the coroutine's own stack holds this userdata, so
         * it is not collectable and this cannot run. If both the coroutine
         * and the userdata have become garbage together, there is nobody
         * left to resume. */
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
    if (!o) return;
    ssh_unpark(o);                 /* one resume per park, see above */
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

    /* Register the op with the backend, which is what makes the wake-up reach
     * us at all.
     *
     * cap/net_stream.c's wake() calls backend->op_complete, and op_complete
     * looks up per-op state that ONLY op_suspend creates - with none it
     * returns early and on_resume is never scheduled. Without this the
     * connect completed, the stream reported itself ready, and the coroutine
     * stayed parked forever.
     *
     * Armed LAST so the fallible setup above is already done: if the backend
     * refuses (effectively OOM) there is nothing suspended to unwind but this
     * binding's own state. */
    const HlAsyncBackend *be = hl_async_backend();
    if (!be || !be->op_suspend ||
        be->op_suspend(lua->base.async_ctx, op) != 0) {
        o->ctx = NULL;
        cont->destroy(cont);
        hl_async_ctx_free(ctx);
        return -1;
    }
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
 * _stream.connect{ host=, port=, user=, timeout_ms=,
 *                  via = { host=, port=, tls=, tls_hostname= } }
 *     -> handle | nil, err
 *
 * `host`/`port`/`user` always name the SSH DESTINATION, whether or not a
 * relay is used: they are what the grant, the host key and the login are
 * about. `via`, when present, names the machine a socket is actually opened
 * to, and the bytes that come back are the relay's, not SSH's - wrapping them
 * is the caller's job (hull.web.ws-stream).
 *
 * Refused unless the caller is stdlib AND the manifest grants the exact
 * destination and login - and, with a relay, the relay too. The order
 * matters: the caller check first, so an application probing this bridge
 * learns nothing about the grant.
 */
static int lua_ssh_connect(lua_State *L)
{
    if (!caller_is_stdlib(L))
        return luaL_error(L,
            "ssh: the byte stream is internal to the SSH module; "
            "use require('hull.ssh')");

    luaL_checktype(L, 1, LUA_TTABLE);
    int base = lua_gettop(L);

    lua_getfield(L, 1, "host");
    const char *host = lua_tostring(L, -1);
    lua_getfield(L, 1, "port");
    int port = (int)luaL_optinteger(L, -1, 22);
    lua_getfield(L, 1, "user");
    const char *user = lua_tostring(L, -1);
    lua_getfield(L, 1, "timeout_ms");
    int timeout = (int)luaL_optinteger(L, -1, SSH_CONNECT_MS_DEF);

    /* The relay, if any. Its strings stay on the stack until after connect:
     * cfg.host and cfg.tls_hostname are BORROWED for the duration of the
     * call, so popping them first would hand the transport freed memory. */
    const char *via_host = NULL, *via_sni = NULL;
    int via_port = 0, via_tls = 0, has_via = 0;
    lua_getfield(L, 1, "via");
    if (!lua_isnil(L, -1) && !lua_istable(L, -1)) {
        /* Refuse rather than fall through. A `via` that is present but not a
         * table would otherwise be ignored, and the connection would go
         * DIRECT - a caller that asked to reach a host through a relay, and
         * whose credentials live in that relay's headers, would never find
         * out. Same reasoning as the tls downgrade refused below. */
        lua_settop(L, base);
        return push_err(L, "ssh: via must be a table");
    }
    if (lua_istable(L, -1)) {
        int v = lua_gettop(L);
        has_via = 1;
        lua_getfield(L, v, "host");
        via_host = lua_tostring(L, -1);
        lua_getfield(L, v, "port");
        via_port = (int)luaL_optinteger(L, -1, 443);
        lua_getfield(L, v, "tls");
        via_tls = lua_toboolean(L, -1);
        lua_getfield(L, v, "tls_hostname");
        via_sni = lua_tostring(L, -1);
    }

    HlLua *lua = get_hl_lua(L);
    if (!lua) { lua_settop(L, base); return push_err(L, "no runtime"); }

    /* The grants, before any name resolution and before a socket exists.
     *
     * The DESTINATION first even when a relay is in play: refusing on the
     * machine the app asked to reach is the more informative answer, and it
     * keeps the relay from being probed by an app that may not reach the
     * host behind it anyway. */
    HlNetAuth auth = hl_ssh_check_connect(lua->base.ssh_policy, host, port, user);
    if (auth == HL_NET_ALLOW && has_via)
        auth = hl_ssh_check_tunnel(lua->base.ssh_policy, via_host, via_port);
    if (auth != HL_NET_ALLOW) {
        lua_settop(L, base);
        return push_err(L, hl_cap_net_auth_reason(auth));
    }

    HlNetStreamConfig cfg;
    KlAllocator       kalloc;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = lua->base.async_ctx;
    cfg.pool       = lua->base.thread_pool;
    cfg.host       = has_via ? via_host : host;
    cfg.port       = has_via ? via_port : port;
    cfg.connect_ms = timeout;

    if (via_tls) {
        /* Refuse rather than downgrade. A caller that asked for an encrypted
         * relay and quietly got a plaintext one would never find out, and the
         * credentials in a tunnel's headers are exactly what the TLS is
         * protecting. NULL here means this build composed no TLS feature, or
         * this invocation resolved no CA bundle. */
        const HlClientTls *t = lua->base.client_tls;
        if (!t || !t->cfg) {
            lua_settop(L, base);
            return push_err(L,
                "ssh: a TLS tunnel was requested but this build has no TLS "
                "trust anchor (no CA bundle resolved, or TLS is not composed "
                "into this binary)");
        }
        cfg.tls       = t->cfg;
        /* The runtime's own allocator, bridged. Stack-local is safe because
         * the transport COPIES the KlAllocator by value (net_stream.c's
         * s->tls_alloc) - and the HlAllocator it points through is the
         * runtime's, which outlives every stream made from it. Routing
         * through hl_alloc_kl rather than kl_allocator_default also keeps
         * this file free of a libkeel symbol, as serve_cli.c does. */
        kalloc        = hl_alloc_kl(lua->base.alloc);
        cfg.tls_alloc = &kalloc;
        /* Default the certificate name to the relay's host, which is what a
         * caller wants unless it dialled an address and expects another
         * name. Never the SSH destination: the relay presents its own. */
        cfg.tls_hostname = via_sni;
    }

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    lua_settop(L, base);                 /* every option lookup, at once */

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

#else  /* !HL_ENABLE_HTTP_CLIENT */

/* Nothing to bind: the guard matches where the module is registered
 * (http_register.c, under the same flag), and on a pure-compute build
 * cap/net_stream.c is filtered out of CAP_SRCS entirely. The resolver refuses
 * `hull/ssh@1` on such a build, so an app gets a clear message rather than a
 * link error. A translation unit has to declare something, hence the typedef.
 *
 * The object is still produced, because the http-lua feature archive names it.
 */
typedef int hl_mod_ssh_unused;

#endif /* HL_ENABLE_HTTP_CLIENT */
