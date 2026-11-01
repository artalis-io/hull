/*
 * mod_ws_client.c - hull.web.ws-client module (outbound WebSocket connect)
 *
 * Exposes: ws.connect(url, handlers)
 *          client connection methods: conn:send / conn:send_binary / conn:close / conn:ping
 *
 * Outbound connections require a non-empty manifest hosts allowlist;
 * the check is enforced at ws.connect() call time by hl_http_check_host.
 *
 * Server-side WebSocket helpers live in mod_ws_server.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "refusal.h"
#include "hull/cap/vm_thread.h"   /* --vm-thread */
#include "mod_buffer.h"
#include "hull/shared/work_charge.h"   /* hl_work_send_units */
#include "internal.h"   /* HL_LUA_ARM */
#include "hull/cap/db_registry.h"   /* hl_db_registry_guard_stale_txns */
#include "hull/cap/http.h"
#include "hull/limits/http.h"   /* HL_WS_CLIENT_MAX_LIVE (audit 32) */
#include "hull/utils/alloc.h"
#include "hull/shared/async.h"   /* HlAsyncTeardown */
#include "hull/shared/async_backend.h"

#include <keel/keel.h>
#include <keel/websocket_client.h>
#include <keel/url.h>

#include "log.h"

#include <stddef.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * Client WebSocket conn
 * ════════════════════════════════════════════════════════════════════ */

/* HL_WS_CLIENT_CONN_MT and HlLuaWsClientUD are in mod_buffer.h. */

/* Drop the connect deadline (on_open, on_error, on_close, teardown). */
static void lua_ws_client_disarm_connect(HlLuaWsClientUD *ud,
                                         HlAsyncBackendCtx *loop)
{
    if (ud->connect_timer && loop) {
        const HlAsyncBackend *be = hl_async_backend();
        if (be) be->timer_cancel(loop, ud->connect_timer);
    }
    ud->connect_timer = 0;
}

/* Free the Keel connection (never from inside one of Keel's callbacks) and
 * give its live-client slot back: the buffers the slot stood for go with it
 * (audit 32). */
static void lua_ws_client_free_conn(HlLuaWsClientUD *ud)
{
    if (ud->client) {
        kl_ws_client_free(ud->client);
        ud->client = NULL;
    }
    hl_ws_client_slot_release(&ud->slot);
}

/* Drop a pending release turn (lua_ws_client_finish's). */
static void lua_ws_client_cancel_release(HlLuaWsClientUD *ud,
                                         HlAsyncBackendCtx *loop)
{
    if (ud->release_timer && loop) {
        const HlAsyncBackend *be = hl_async_backend();
        if (be) be->timer_cancel(loop, ud->release_timer);
    }
    ud->release_timer = 0;
}

/* Runtime teardown, the loop still alive: free the Keel connection now. The
 * userdata (and its refs) go with the Lua state. */
static void lua_ws_client_teardown_stop(HlAsyncTeardown *t, HlAsyncBackendCtx *loop)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)(void *)
        ((char *)t - offsetof(HlLuaWsClientUD, teardown));
    ud->closed = 1;
    lua_ws_client_disarm_connect(ud, loop);
    lua_ws_client_cancel_release(ud, loop);
    lua_ws_client_free_conn(ud);
}

/* Client conn methods */

static int lua_ws_client_send(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client || ud->closed)
        return luaL_error(L, "client connection closed");

    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_hlwork(L, (size_t)hl_work_send_units(len, 1), 0);   /* audit 14 */
    /* Again, after the conversion (audit 27): a number converted to a
     * string allocates, and the allocation can run a __gc that closes this
     * connection. */
    // cppcheck-suppress knownConditionTrueFalse
    if (!ud->client || ud->closed)
        return luaL_error(L, "client connection closed");

    int rc = kl_ws_client_send_text(ud->client, data, len);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int lua_ws_client_send_binary(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client || ud->closed)
        return luaL_error(L, "client connection closed");

    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_hlwork(L, (size_t)hl_work_send_units(len, 1), 0);   /* audit 14 */
    // cppcheck-suppress knownConditionTrueFalse
    if (!ud->client || ud->closed)   /* audit 27, as send */
        return luaL_error(L, "client connection closed");

    int rc = kl_ws_client_send_binary(ud->client, data, len);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static void lua_ws_client_close_expired(void *user_data);

/* Close-handshake deadline (audit 26): Keel waits in CLOSING for the peer's
 * Close with no timer, so a peer that never answered held the client, its fd
 * and the userdata until teardown. Armed only once the connection is open
 * (the connect deadline still runs before that) and only once. */
static void lua_ws_client_arm_close(HlLuaWsClientUD *ud)
{
    if (!ud->closed && ud->client && !ud->close_armed &&
        ud->connect_timer == 0 && ud->lua && !ud->lua->base.shutting_down &&
        ud->lua->base.async_ctx) {
        const HlAsyncBackend *be = hl_async_backend();
        ud->connect_timer = be ? be->timer_add(ud->lua->base.async_ctx,
                                               HL_WS_CLIENT_CLOSE_TIMEOUT_MS,
                                               lua_ws_client_close_expired, ud)
                               : 0;
        ud->close_armed = ud->connect_timer != 0;
    }
}

static int lua_ws_client_close(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client || ud->closed)
        return 0;

    uint16_t code = (uint16_t)luaL_optinteger(L, 2, 1000);
    size_t reason_len = 0;
    const char *reason = luaL_optlstring(L, 3, NULL, &reason_len);
    /* Again, after the conversions (a number argument allocates, and the
     * allocation can run a __gc that closes or frees the client). */
    // cppcheck-suppress knownConditionTrueFalse
    if (!ud->client || ud->closed)
        return 0;

    if (!ud->opened) {
        /* Before the upgrade completed Keel's close does nothing (audit 27):
         * record it; on_open sends it and arms the close deadline. The
         * first request wins. */
        if (!ud->close_requested) {
            if (reason_len > sizeof ud->close_reason)
                reason_len = sizeof ud->close_reason;
            if (reason && reason_len) memcpy(ud->close_reason, reason, reason_len);
            ud->close_reason_len = reason ? reason_len : 0;
            ud->close_code = code;
            ud->close_requested = 1;
        }
        return 0;
    }

    kl_ws_client_close(ud->client, code, reason, reason_len);
    lua_ws_client_arm_close(ud);
    return 0;
}

static int lua_ws_client_ping(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client || ud->closed)
        return 0;

    size_t len = 0;
    const char *data = luaL_optlstring(L, 2, NULL, &len);
    lua_hlwork(L, (size_t)hl_work_send_units(len, 1), 0);   /* as send */
    /* Again, after the conversion (audit 37, as send since audit 27): a
     * number converted to a string allocates, and the allocation can run a
     * __gc that closes this connection. */
    // cppcheck-suppress knownConditionTrueFalse
    if (!ud->client || ud->closed)
        return 0;
    kl_ws_client_send_ping(ud->client, data, len);
    return 0;
}

static void unref_slot(lua_State *L, int *ref)
{
    if (*ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, *ref);
        *ref = LUA_NOREF;
    }
}

/* The finalizer. Not reachable from app code: the metatable is locked and
 * methods live in a separate table. As a method (`conn:__gc()`), it freed a
 * KlWsClientConn Keel still dispatched to, and the real finalizer then
 * unref'd the same refs a second time, corrupting the registry. Each ref is
 * reset once released, so a second call is harmless anyway. */
static int lua_ws_client_gc(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    hl_async_teardown_unlink(&ud->teardown);
    /* Whenever the loop is still reachable, shutting down or not: a timer
     * left armed fired into the freed userdata (audit 38). The entry points
     * clear async_ctx before the runtime goes, and the backend wraps are
     * orphaned (no-ops) once the loop is freed. */
    if (ud->lua && ud->lua->base.async_ctx) {
        lua_ws_client_disarm_connect(ud, ud->lua->base.async_ctx);
        lua_ws_client_cancel_release(ud, ud->lua->base.async_ctx);
    }
    ud->connect_timer = 0;
    ud->release_timer = 0;
    lua_ws_client_free_conn(ud);
    unref_slot(L, &ud->on_open_ref);
    unref_slot(L, &ud->on_message_ref);
    unref_slot(L, &ud->on_close_ref);
    unref_slot(L, &ud->on_error_ref);
    unref_slot(L, &ud->self_ref);
    return 0;
}

static const luaL_Reg ws_client_conn_methods[] = {
    {"send",        lua_ws_client_send},
    {"send_binary", lua_ws_client_send_binary},
    {"close",       lua_ws_client_close},
    {"ping",        lua_ws_client_ping},
    {NULL, NULL}
};

static void hl_lua_ws_register_client_conn_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_WS_CLIENT_CONN_MT);
    lua_pushcfunction(L, lua_ws_client_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    hl_lua_setfuncs(L, ws_client_conn_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

/* A client callback, run as one protected call on the main state: the
 * callback, self, and the event's values (a message is the peer's, of the
 * peer's size) are pushed inside it. Pushed before lua_pcall, a memory error
 * had nothing to unwind to and aborted the process. Each is its own run of
 * the instruction budget. */
typedef struct {
    int         fn_ref;
    int         self_ref;
    int         kind;       /* 0 open, 1 message, 2 close, 3 error */
    const char *data;
    size_t      len;
    int         flag;       /* message: is_binary */
    int         code;       /* close: code */
} HlWscCall;

static int ws_client_call_k(lua_State *L)
{
    HlWscCall *c = (HlWscCall *)lua_touserdata(L, 1);
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->self_ref);
    int n = 1;
    switch (c->kind) {
    case 1:
        lua_pushlstring(L, c->data ? c->data : "", c->data ? c->len : 0);
        lua_pushboolean(L, c->flag);
        n = 3;
        break;
    case 2:
        lua_pushinteger(L, c->code);
        if (c->data && c->len > 0) lua_pushlstring(L, c->data, c->len);
        else                       lua_pushnil(L);
        n = 3;
        break;
    case 3:
        lua_pushstring(L, c->data ? c->data : "unknown");
        n = 2;
        break;
    default:
        break;
    }
    lua_call(L, n, 0);
    return 0;
}

static void ws_client_call(HlLuaWsClientUD *ud, HlWscCall *c, const char *what)
{
    lua_State *L = ud->L;
    if (!lua_checkstack(L, 8)) {
        log_error("[hull:ws:client] %s: out of memory", what);
        return;
    }
    /* An entry like a ws-server callback: it neither joins nor leaves a
     * stale transaction (audit 6 M1). */
    hl_db_registry_guard_stale_txns(ud->lua->base.db_registry);
    HL_LUA_ARM_KIND(ud->lua, L, HL_RUN_CALLBACK);
    lua_pushcfunction(L, ws_client_call_k);
    lua_pushlightuserdata(L, c);
    int prc = lua_pcall(L, 1, 0, 0);
    hl_db_registry_guard_stale_txns(ud->lua->base.db_registry);
    if (prc != LUA_OK) {
        char ebuf[512];
        log_error("[hull:ws:client] %s error: %s", what,
                  hl_lua_error_text(ud->lua, L, -1, ebuf, sizeof ebuf));
        lua_pop(L, 1);
    }
}

/* Client callbacks */

/* --vm-thread (cap/vm_thread.h): ws-client on_open runs on the VM thread. */
static void lua_ws_client_on_open_impl(KlWsClientConn *ws, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    void *user_data;
} lua_ws_client_on_open_vmt_args;
static void lua_ws_client_on_open_vmt_job(void *ud)
{
    lua_ws_client_on_open_vmt_args *a = (lua_ws_client_on_open_vmt_args *)ud;
    lua_ws_client_on_open_impl(a->ws, a->user_data);
}
static void lua_ws_client_on_open(KlWsClientConn *ws, void *user_data)
{
    HlVmThread *vmt = ((HlLuaWsClientUD *)user_data)->lua->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        lua_ws_client_on_open_vmt_args a = { ws, user_data };
        (void)hl_vmt_run(vmt, lua_ws_client_on_open_vmt_job, &a, NULL, NULL);
        return;
    }
    lua_ws_client_on_open_impl(ws, user_data);
}

static void lua_ws_client_on_open_impl(KlWsClientConn *ws, void *user_data)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    lua_ws_client_disarm_connect(ud, ud->lua->base.async_ctx);
    ud->opened = 1;
    /* conn:close() ran before the upgrade completed (audit 27): close now,
     * with its code and reason, and arm the close deadline. The app's
     * on_open is not run - it asked for the connection to end - and
     * on_close fires once, from Keel's close or the deadline. */
    if (ud->close_requested) {
        kl_ws_client_close(ws, ud->close_code,
                           ud->close_reason_len ? ud->close_reason : NULL,
                           ud->close_reason_len);
        lua_ws_client_arm_close(ud);
        return;
    }
    if (ud->on_open_ref == LUA_NOREF)
        return;

    HlWscCall c = { ud->on_open_ref, ud->self_ref, 0, NULL, 0, 0, 0 };
    ws_client_call(ud, &c, "on_open");
}

/* --vm-thread (cap/vm_thread.h): ws-client on_message runs on the VM thread. */
static void lua_ws_client_on_message_impl(KlWsClientConn *ws, const char *data, size_t len, int is_binary, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    const char *data;
    size_t len;
    int is_binary;
    void *user_data;
} lua_ws_client_on_message_vmt_args;
static void lua_ws_client_on_message_vmt_job(void *ud)
{
    lua_ws_client_on_message_vmt_args *a = (lua_ws_client_on_message_vmt_args *)ud;
    lua_ws_client_on_message_impl(a->ws, a->data, a->len, a->is_binary, a->user_data);
}
static void lua_ws_client_on_message(KlWsClientConn *ws, const char *data, size_t len, int is_binary, void *user_data)
{
    HlVmThread *vmt = ((HlLuaWsClientUD *)user_data)->lua->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        lua_ws_client_on_message_vmt_args a = { ws, data, len, is_binary, user_data };
        (void)hl_vmt_run(vmt, lua_ws_client_on_message_vmt_job, &a, NULL, NULL);
        return;
    }
    lua_ws_client_on_message_impl(ws, data, len, is_binary, user_data);
}

static void lua_ws_client_on_message_impl(KlWsClientConn *ws, const char *data,
                                       size_t len, int is_binary,
                                       void *user_data)
{
    (void)ws;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    if (ud->on_message_ref == LUA_NOREF)
        return;

    HlWscCall c = { ud->on_message_ref, ud->self_ref, 1, data, len,
                    is_binary, 0 };
    ws_client_call(ud, &c, "on_message");
}

/* The loop turn after the connection ended (a timer, not one of Keel's
 * callbacks: safe to free there). Without it the Keel client - and its up to
 * 4 MiB drain and frame buffer - lived until the userdata was collected, and
 * an app that kept a closed client kept its slot (audit 32). */
static void lua_ws_client_release(void *user_data)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    ud->release_timer = 0;   /* fired */
    lua_ws_client_free_conn(ud);
}

/* The connection is over (closed, or failed - Keel closes it before calling
 * on_error, and fires no on_close after). */
static void lua_ws_client_finish(HlLuaWsClientUD *ud)
{
    /* Release self-ref - allow GC */
    if (ud->self_ref != LUA_NOREF) {
        luaL_unref(ud->L, LUA_REGISTRYINDEX, ud->self_ref);
        ud->self_ref = LUA_NOREF;
    }
    /* Mark closed (methods fail closed) but keep `client` non-NULL so __gc
     * frees the KlWsClientConn - Keel does NOT free it on close, and we must
     * not free it here either (Keel touches `ws` immediately after this
     * callback returns: `ws->state = WSC_CLOSED; wsc_close_connection(ws)`).
     * It is freed on the next loop turn instead (audit 32); with no timer,
     * __gc still frees it. */
    ud->closed = 1;
    if (ud->client && !ud->release_timer && ud->lua &&
        !ud->lua->base.shutting_down && ud->lua->base.async_ctx) {
        const HlAsyncBackend *be = hl_async_backend();
        ud->release_timer = be ? be->timer_add(ud->lua->base.async_ctx, 0,
                                               lua_ws_client_release, ud)
                               : 0;
    }
}

/* --vm-thread (cap/vm_thread.h): ws-client on_close runs on the VM thread. */
static void lua_ws_client_on_close_impl(KlWsClientConn *ws, uint16_t code, const char *reason, size_t reason_len, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    uint16_t code;
    const char *reason;
    size_t reason_len;
    void *user_data;
} lua_ws_client_on_close_vmt_args;
static void lua_ws_client_on_close_vmt_job(void *ud)
{
    lua_ws_client_on_close_vmt_args *a = (lua_ws_client_on_close_vmt_args *)ud;
    lua_ws_client_on_close_impl(a->ws, a->code, a->reason, a->reason_len, a->user_data);
}
static void lua_ws_client_on_close(KlWsClientConn *ws, uint16_t code, const char *reason, size_t reason_len, void *user_data)
{
    HlVmThread *vmt = ((HlLuaWsClientUD *)user_data)->lua->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        lua_ws_client_on_close_vmt_args a = { ws, code, reason, reason_len, user_data };
        (void)hl_vmt_run(vmt, lua_ws_client_on_close_vmt_job, &a, NULL, NULL);
        return;
    }
    lua_ws_client_on_close_impl(ws, code, reason, reason_len, user_data);
}

static void lua_ws_client_on_close_impl(KlWsClientConn *ws, uint16_t code,
                                     const char *reason, size_t reason_len,
                                     void *user_data)
{
    (void)ws;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    lua_ws_client_disarm_connect(ud, ud->lua->base.async_ctx);
    /* Over before on_close runs (audit 30): a conn:close() from it armed the
     * close deadline after the disarm above (__gc cancelled it, but until
     * then it held the userdata for a connection that had ended). */
    ud->closed = 1;

    if (ud->on_close_ref != LUA_NOREF) {
        HlWscCall c = { ud->on_close_ref, ud->self_ref, 2, reason, reason_len,
                        0, code };
        ws_client_call(ud, &c, "on_close");
    }

    lua_ws_client_finish(ud);
}

/* --vm-thread (cap/vm_thread.h): ws-client on_error runs on the VM thread. */
static void lua_ws_client_on_error_impl(KlWsClientConn *ws, const char *msg, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    const char *msg;
    void *user_data;
} lua_ws_client_on_error_vmt_args;
static void lua_ws_client_on_error_vmt_job(void *ud)
{
    lua_ws_client_on_error_vmt_args *a = (lua_ws_client_on_error_vmt_args *)ud;
    lua_ws_client_on_error_impl(a->ws, a->msg, a->user_data);
}
static void lua_ws_client_on_error(KlWsClientConn *ws, const char *msg, void *user_data)
{
    HlVmThread *vmt = ((HlLuaWsClientUD *)user_data)->lua->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        lua_ws_client_on_error_vmt_args a = { ws, msg, user_data };
        (void)hl_vmt_run(vmt, lua_ws_client_on_error_vmt_job, &a, NULL, NULL);
        return;
    }
    lua_ws_client_on_error_impl(ws, msg, user_data);
}

static void lua_ws_client_on_error_impl(KlWsClientConn *ws, const char *msg,
                                     void *user_data)
{
    (void)ws;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    lua_ws_client_disarm_connect(ud, ud->lua->base.async_ctx);
    ud->closed = 1;   /* terminal: as in on_close (audit 30) */
    if (ud->on_error_ref == LUA_NOREF) {
        log_error("[hull:ws:client] error: %s", msg ? msg : "unknown");
        lua_ws_client_finish(ud);
        return;
    }

    HlWscCall c = { ud->on_error_ref, ud->self_ref, 3, msg, 0, 0, 0 };
    ws_client_call(ud, &c, "on_error");
    /* Terminal: Keel closed the connection and will not call on_close, so
     * this is where the client is released (it stayed pinned until VM
     * teardown). */
    lua_ws_client_finish(ud);
}

/* The connect deadline passed before the upgrade completed: free the Keel
 * client (a timer callback, not one of Keel's: safe) and report it as the
 * connection's terminal error, as Keel reports a failed connect. */
static void lua_ws_client_connect_expired(void *user_data)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    ud->connect_timer = 0;   /* fired */
    if (ud->closed || !ud->client) return;
    lua_ws_client_free_conn(ud);
    lua_ws_client_on_error(NULL, "connect timeout", ud);
}

/* The peer never answered conn:close() (audit 26): free the Keel client (a
 * timer callback, not one of Keel's: safe) and end the connection as Keel
 * would on its close - on_close once, with 1006, then finish. Keel fires no
 * callback for a freed client, and a normal close cancelled this timer. */
static void lua_ws_client_close_expired(void *user_data)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    ud->connect_timer = 0;   /* fired */
    if (ud->closed || !ud->client) return;
    lua_ws_client_free_conn(ud);
    static const char why[] = "close timeout";
    lua_ws_client_on_close(NULL, 1006, why, sizeof why - 1, ud);
}

/* ws.connect(url, handlers [, opts]) */
static int lua_ws_connect(lua_State *L)
{
    /* `lua` is read on lines further down; cppcheck's data-flow
     * loses track across the early luaL_error returns below. */
    /* cppcheck-suppress unreadVariable */
    HlLua *lua = get_hl_lua(L);

    const char *url = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    /* Check host allowlist */
    KlUrl parsed;
    if (kl_url_parse(url, &parsed) != 0)
        return luaL_error(L, "invalid WebSocket URL");

#ifdef HL_ENABLE_HTTP_CLIENT
    /* No http config means an empty manifest.hosts: nothing is allowed. This
     * used to skip the check, so ws.connect reached any host. */
    if (!lua->base.http_cfg ||
        hl_http_check_host(lua->base.http_cfg, parsed.host,
                           parsed.host_len) != 0)
        return luaL_error(L, "host not in allowlist");
    /* wss:// takes the client TLS config http.fetch uses (audit 24: Keel
     * needs cfg->tls for it and NULL was passed, so every wss:// failed). */
    if (parsed.is_https && !lua->base.http_cfg->tls)
        return luaL_error(L, "ws.connect: wss:// needs TLS, and this "
                             "process has no client TLS configuration");
#else
    return luaL_error(L,
        "ws.connect requires HL_ENABLE_HTTP_CLIENT (build-time)");
#endif

    if (!lua->server)
        return luaL_error(L, "ws.connect requires running server");
    /* A __gc finalizer run by lua_close at teardown: the loop is gone
     * (audit 16). */
    if (lua->base.shutting_down || !lua->base.async_ctx)
        return luaL_error(L, "ws.connect: runtime is shutting down");

    /* Create userdata */
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)lua_newuserdata(L,
                                                                sizeof(HlLuaWsClientUD));
    memset(ud, 0, sizeof *ud);
    ud->client = NULL;
    ud->closed = 0;
    ud->on_open_ref = LUA_NOREF;
    ud->on_message_ref = LUA_NOREF;
    ud->on_close_ref = LUA_NOREF;
    ud->on_error_ref = LUA_NOREF;
    ud->self_ref = LUA_NOREF;
    ud->L = lua->L; /* use main thread for callbacks */
    ud->lua = lua;

    luaL_setmetatable(L, HL_WS_CLIENT_CONN_MT);

    /* Extract callbacks */
    lua_getfield(L, 2, "on_open");
    if (lua_isfunction(L, -1))
        ud->on_open_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 2, "on_message");
    if (lua_isfunction(L, -1))
        ud->on_message_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 2, "on_close");
    if (lua_isfunction(L, -1))
        ud->on_close_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 2, "on_error");
    if (lua_isfunction(L, -1))
        ud->on_error_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    /* A live-client slot (audit 32), after the handlers' conversions (app
     * code) and before the Keel client exists. A client the app dropped
     * whose Keel connection was not freed yet goes with a collection, so at
     * the cap collect once and look again (as fs.mmap does for its SIGBUS
     * slots). The userdata owns the slot from here: __gc gives it back if
     * anything below raises. */
    if (lua->base.ws_clients >= HL_WS_CLIENT_MAX_LIVE) {
        lua_gc(L, LUA_GCCOLLECT);
        /* The collection ran __gc finalizers: app code. (async_ctx was
         * checked above and is not cleared by app code; shutting_down is.) */
        if (lua->base.shutting_down)
            return luaL_error(L, "ws.connect: runtime is shutting down");
    }
    /* Read again: the collection's __gc finalizers give slots back. */
    // cppcheck-suppress duplicateCondition
    if (lua->base.ws_clients >= HL_WS_CLIENT_MAX_LIVE)
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "ws.connect: too many live WebSocket clients "
                          "(at most %d per VM)", HL_WS_CLIENT_MAX_LIVE);
    /* The connect - DNS, socket, connect, the TLS handshake for wss:// -
     * charged before it as an outbound HTTP request is (audit 40), and
     * before the slot and the self-reference, so a trip leaks neither. */
    lua_hlwork(L, HL_WORK_WS_CONNECT_UNITS, 0);
    lua->base.ws_clients++;
    ud->slot = &lua->base.ws_clients;

    /* Store self-ref to prevent GC while connected */
    lua_pushvalue(L, -1); /* dup userdata */
    ud->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Connect */
    KlWsClientCallbacks cbs = {
        .on_open = lua_ws_client_on_open,
        .on_message = lua_ws_client_on_message,
        .on_close = lua_ws_client_on_close,
        .on_error = lua_ws_client_on_error,
    };

    /* Keel keeps the tls pointer for the connection's life; the runtime's
     * client TLS config outlives every client (teardown stops them first). */
    KlWsClientConfig wcfg;
    memset(&wcfg, 0, sizeof wcfg);
#ifdef HL_ENABLE_HTTP_CLIENT
    wcfg.tls = parsed.is_https ? lua->base.http_cfg->tls : NULL;
#endif
    /* Keel's default frame limit is 16 MiB per client, off the heap:
     * capped, so the live-client cap bounds the total (audit 32). */
    wcfg.max_frame_size = hl_ws_client_max_frame(lua->mem_limit);
    KlWsClientConn *client = kl_ws_client_connect(
        &lua->server->ev, &lua->server->alloc_storage, &wcfg, url, &cbs, ud);

    if (!client) {
        /* Clean up self_ref */
        luaL_unref(L, LUA_REGISTRYINDEX, ud->self_ref);
        ud->self_ref = LUA_NOREF;
        hl_ws_client_slot_release(&ud->slot);
        return luaL_error(L, "WebSocket connect failed");
    }

    ud->client = client;
    ud->teardown.stop = lua_ws_client_teardown_stop;
    hl_async_teardown_link(&lua->base.async_live, &ud->teardown);

    /* Connect + handshake deadline (audit 25): the manifest's http timeout,
     * else 30 s, as http.fetch. Without one the connect is not started. */
    int ms = HL_HTTP_DEFAULT_TIMEOUT_MS;
#ifdef HL_ENABLE_HTTP_CLIENT
    ms = hl_http_timeout_resolve(0, lua->base.http_cfg->timeout_ms,
                                 HL_HTTP_FETCH_MAX_TIMEOUT_MS);
#endif
    const HlAsyncBackend *be = hl_async_backend();
    ud->connect_timer = be ? be->timer_add(lua->base.async_ctx, (uint64_t)ms,
                                           lua_ws_client_connect_expired, ud)
                           : 0;
    if (ud->connect_timer == 0) {
        hl_async_teardown_unlink(&ud->teardown);
        lua_ws_client_free_conn(ud);   /* not inside a Keel event */
        ud->closed = 1;
        luaL_unref(L, LUA_REGISTRYINDEX, ud->self_ref);
        ud->self_ref = LUA_NOREF;
        return luaL_error(L, "WebSocket connect failed");
    }

    return 1; /* return userdata on stack */
}

/* ── Module opener ─────────────────────────────────────────────────── */

static const luaL_Reg ws_client_funcs[] = {
    {"connect", lua_ws_connect},
    {NULL, NULL}
};

int luaopen_hull_ws_client(lua_State *L)
{
    hl_lua_ws_register_client_conn_mt(L);
    hl_lua_newlib(L, ws_client_funcs);
    return 1;
}
