/* mod_smtp.c - hull.smtp module: SMTP email sending
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/smtp.h"
#include "hull/cap/smtp_op.h"
#include "hull/cap/smtp_async.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "internal.h"   /* hl_lua_check_can_wait */

#include <sh_arena.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.smtp module
 *
 * smtp.send(opts) → { ok = true } or { ok = false, error = "..." }
 * ════════════════════════════════════════════════════════════════════ */

/* Recipients past this many are refused: SMTP servers cap RCPT TO at 100
 * per message (RFC 5321 4.5.3.1.8 sets that as the minimum a server must
 * take), and the list is copied into the scratch arena. */
#define HL_LUA_SMTP_CC_MAX 1000

/* Helper: copy the string array at stack index @p idx into the scratch
 * arena. Every entry is copied or the whole call fails (audit 10): it used
 * to read the length through __len (app code, and an int cast of whatever it
 * returned) and to drop - silently - a non-string entry or one the arena had
 * no room for, so a message went out to fewer recipients than asked. Raises
 * on a bad entry or a full arena. */
static void lua_get_string_array(lua_State *L, int idx, const char *what,
                                 const char ***out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    size_t len = (size_t)lua_rawlen(L, idx);
    if (len == 0)
        return;
    if (len > HL_LUA_SMTP_CC_MAX)
        luaL_error(L, "smtp.send: %s has more than %d entries", what,
                   HL_LUA_SMTP_CC_MAX);

    /* Copied into the scratch arena (reset at the next run). */
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        luaL_error(L, "smtp.send: no scratch space for %s", what);

    const char **arr = sh_arena_calloc(lua->scratch, len, sizeof(const char *));
    if (!arr)
        luaL_error(L, "smtp.send: out of scratch space for %s", what);

    for (size_t i = 1; i <= len; i++) {
        lua_rawgeti(L, idx, (lua_Integer)i);
        if (lua_type(L, -1) != LUA_TSTRING)
            luaL_error(L, "smtp.send: %s[%d] must be a string", what, (int)i);
        size_t slen;
        const char *s = lua_tolstring(L, -1, &slen);
        char *copy = sh_arena_alloc(lua->scratch, slen + 1);
        if (!copy)
            luaL_error(L, "smtp.send: out of scratch space for %s", what);
        memcpy(copy, s, slen + 1);
        arr[i - 1] = copy;
        lua_pop(L, 1);
    }

    *out = arr;
    *out_count = (int)len;
}

/* Push a { ok = false, error = "..." } failure table (leaves it on the stack). */
static int lua_smtp_fail(lua_State *L, const char *err)
{
    lua_newtable(L);
    lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
    lua_pushstring(L, err ? err : "smtp send failed"); lua_setfield(L, -2, "error");
    return 1;
}

/* Async resume push_result: read the published terminal (audit once) and push the
 * { ok, error } result onto the resumed coroutine. driver is the HlSmtpAsyncOp. */
static void lua_push_smtp_result(lua_State *L, void *driver)
{
    HlSmtpResult r;
    hl_smtp_async_finish((HlSmtpAsyncOp *)driver, &r);
    lua_newtable(L);
    if (r.rc == 0) {
        lua_pushboolean(L, 1); lua_setfield(L, -2, "ok");
    } else {
        lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
        lua_pushstring(L, r.token ? r.token : "smtp send failed");
        lua_setfield(L, -2, "error");
    }
}

/* smtp.send(opts) */
static int lua_smtp_send(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.smtp_cfg)
        return luaL_error(L, "smtp not configured (no hosts in manifest)");

    luaL_checktype(L, 1, LUA_TTABLE);

    /* Extract required fields. Every value stays on the stack until the
     * message has been copied: lua_tostring converts a number in place and
     * an __index may return a fresh string, and once popped nothing
     * referenced it - a later field's conversion could collect it, and the
     * freed bytes were sent to the SMTP server. */
    luaL_checkstack(L, 24, "smtp.send");
    lua_getfield(L, 1, "host");
    const char *host = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "port");
    int port = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 587;

    lua_getfield(L, 1, "username");
    const char *username = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "password");
    const char *password = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "tls");
    int use_tls = 0;
    if (lua_isboolean(L, -1))
        use_tls = lua_toboolean(L, -1) ? 1 : 0;
    else if (lua_isinteger(L, -1))
        use_tls = (int)lua_tointeger(L, -1);

    lua_getfield(L, 1, "from");
    const char *from = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "to");
    const char *to = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "subject");
    const char *subject = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "body");
    const char *body = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "content_type");
    const char *content_type = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "reply_to");
    const char *reply_to = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    /* CC array (optional) */
    const char **cc = NULL;
    int cc_count = 0;
    lua_getfield(L, 1, "cc");
    if (lua_istable(L, -1))
        lua_get_string_array(L, lua_gettop(L), "cc", &cc, &cc_count);
    else if (!lua_isnil(L, -1))
        return luaL_error(L, "smtp.send: cc must be an array of strings");

    /* Validate required fields */
    if (!host)    { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "host required"); lua_setfield(L, -2, "error"); return 1; }
    if (!from)    { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "from required"); lua_setfield(L, -2, "error"); return 1; }
    if (!to)      { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "to required"); lua_setfield(L, -2, "error"); return 1; }
    if (!subject) { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "subject required"); lua_setfield(L, -2, "error"); return 1; }
    if (!body)    { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "body required"); lua_setfield(L, -2, "error"); return 1; }

    /* Build message struct */
    HlSmtpMessage msg = {
        .host = host,
        .port = port,
        .username = username,
        .password = password,
        .use_tls = use_tls,
        .from = from,
        .to = to,
        .cc = cc,
        .cc_count = cc_count,
        .reply_to = reply_to,
        .subject = subject,
        .body = body,
        .content_type = content_type,
    };

    /* No active event loop (app.main CLI / in-process test harness / direct C):
     * synchronous model 1 on the calling thread. This is the ONLY sync path -
     * we never fall back to it merely because admission or the pool is
     * unavailable (that resolves as an async connect_failed below). */
    if (!lua->base.async_ctx || !lua->base.smtp_async) {
        const char *err_msg = NULL;
        int rc = hl_cap_smtp_send(lua->base.smtp_cfg, &msg, &err_msg);
        if (rc == 0) {
            lua_newtable(L);
            lua_pushboolean(L, 1); lua_setfield(L, -2, "ok");
            return 1;
        }
        return lua_smtp_fail(L, err_msg);
    }

    /* Active event loop: model 2. Validate (CR/LF in a header field, missing
     * fields) and authorize the host on the submit side (audited exactly once
     * here) BEFORE any reservation or worker submission. */
    if (hl_smtp_validate_message(&msg) != 0)
        return lua_smtp_fail(L, "validation_failed");
    if (hl_smtp_check_host(lua->base.smtp_cfg, msg.host) != 0) {
        hl_smtp_audit_denied(&msg);
        return lua_smtp_fail(L, "host_not_allowed");
    }

    hl_lua_check_can_wait(L, "smtp.send()");   /* before anything is armed */

    /* Deep-copy the message into an owned op (crosses to the worker thread). */
    int timeout_ms = lua->base.smtp_cfg->timeout_ms > 0
                         ? lua->base.smtp_cfg->timeout_ms : 0;
    HlSmtpOp *op = hl_smtp_op_create(&msg, timeout_ms);
    if (!op)
        return lua_smtp_fail(L, "connect_failed");

    /* Create the continuation vehicle (the runtime cont captures this coroutine). */
    HlAsyncCtx *actx = hl_async_ctx_create(lua->server, lua->base.net_ctx,
                                           lua->base.alloc);
    if (!actx) { hl_smtp_op_free(op); return lua_smtp_fail(L, "connect_failed"); }
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                 HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                 lua_push_smtp_result);
    if (!cont) { hl_smtp_op_free(op); hl_async_ctx_free(actx);
                 return lua_smtp_fail(L, "connect_failed"); }
    actx->cont     = cont;
    actx->detached = (lua->active_conn == NULL);

    HlSmtpAsyncReq req = {
        .server     = lua->base.smtp_async,
        .pool       = lua->base.thread_pool,
        .net_ctx    = lua->base.net_ctx,
        .actx       = actx,
        .req_handle = lua->active_conn,
        .detached   = (lua->active_conn == NULL),
        .inputs     = op,
    };
    HlSmtpAsyncOutcome out;
    hl_smtp_async_submit(&req, &out);

    if (out.disposition == HL_SMTP_ASYNC_SUSPENDED)
        return lua_yieldk(L, 0, 0, NULL);   /* resumed by lua_push_smtp_result */

    /* RESOLVED: an immediate scheduling failure (already audited once in the
     * orchestration). The public token stays connect_failed; never blocks. */
    return lua_smtp_fail(L, out.result.token ? out.result.token : "connect_failed");
}

static const luaL_Reg smtp_funcs[] = {
    {"send", lua_smtp_send},
    {NULL, NULL}
};

int luaopen_hull_smtp(lua_State *L)
{
    luaL_newlib(L, smtp_funcs);
    return 1;
}
