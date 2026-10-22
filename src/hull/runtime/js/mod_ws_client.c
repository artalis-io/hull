/*
 * mod_ws_client.c - hull:web:ws-client module (outbound WebSocket connect)
 *
 * Exposes: ws.connect(url, handlers)
 *          client connection methods: send / sendBinary / close / ping
 *
 * Outbound connections require a non-empty manifest hosts allowlist;
 * the check is enforced at ws.connect() call time.
 *
 * Server-side WebSocket helpers live in mod_ws_server.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/shared/work_charge.h"   /* hl_work_send_units */
#include "internal.h"            /* async gate, instruction budget */
#include "hull/cap/http.h"
#include "hull/cap/db_registry.h"   /* hl_db_registry_guard_stale_txns */
#include "hull/utils/alloc.h"

#include <keel/keel.h>
#include <keel/websocket_client.h>
#include <keel/url.h>

#include "hull/shared/async_backend.h"
#include "hull/shared/async.h"   /* HlAsyncTeardown */
#include "log.h"

#include <stddef.h>
#include <string.h>

/* The client class id and HlJSWsClientUD are declared in mod_buffer.h. */
JSClassID js_ws_client_conn_class_id;

/* Runtime teardown, the loop still alive: free the Keel connection now and
 * drop the self-reference (a pending release timer is cancelled first), so
 * the object goes with the runtime. Releasing it can run the finalizer,
 * which frees ud: nothing touches ud after that. */
static void js_ws_client_teardown_stop(HlAsyncTeardown *t, HlAsyncBackendCtx *loop)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)(void *)
        ((char *)t - offsetof(HlJSWsClientUD, teardown));
    ud->closed = 1;
    if (ud->client) {
        kl_ws_client_free(ud->client);
        ud->client = NULL;
    }
    if (ud->release_timer && loop) {
        const HlAsyncBackend *be = hl_async_backend();
        if (be) be->timer_cancel(loop, ud->release_timer);
    }
    ud->release_timer = 0;
    if (!JS_IsUndefined(ud->self_ref)) {
        JSContext *ctx = ud->ctx;
        JSValue self = ud->self_ref;
        ud->self_ref = JS_UNDEFINED;
        JS_FreeValue(ctx, self);   /* may free ud */
    }
}

static void js_ws_client_conn_finalizer(JSRuntime *rt, JSValue val)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque(val,
                                                           js_ws_client_conn_class_id);
    if (!ud) return;

    hl_async_teardown_unlink(&ud->teardown);
    if (ud->client)
        kl_ws_client_free(ud->client);

    /* Mirror the gc_mark guards: callbacks may have been cleared to
     * JS_UNDEFINED by on_close before the finalizer runs. Freeing
     * UNDEFINED is a no-op in QuickJS today, but the explicit check
     * documents the invariant and keeps the two callbacks symmetric. */
    if (!JS_IsUndefined(ud->on_open))    JS_FreeValueRT(rt, ud->on_open);
    if (!JS_IsUndefined(ud->on_message)) JS_FreeValueRT(rt, ud->on_message);
    if (!JS_IsUndefined(ud->on_close))   JS_FreeValueRT(rt, ud->on_close);
    if (!JS_IsUndefined(ud->on_error))   JS_FreeValueRT(rt, ud->on_error);
    /* self_ref is undefined here: while it is held the object cannot be
     * finalized (gc_mark does not report it). */
    js_free_rt(rt, ud);
}

static void js_ws_client_conn_gc_mark(JSRuntime *rt, JSValueConst val,
                                        JS_MarkFunc *mark_func)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque(val,
                                                           js_ws_client_conn_class_id);
    if (!ud) return;
    if (!JS_IsUndefined(ud->on_open)) JS_MarkValue(rt, ud->on_open, mark_func);
    if (!JS_IsUndefined(ud->on_message)) JS_MarkValue(rt, ud->on_message, mark_func);
    if (!JS_IsUndefined(ud->on_close)) JS_MarkValue(rt, ud->on_close, mark_func);
    if (!JS_IsUndefined(ud->on_error)) JS_MarkValue(rt, ud->on_error, mark_func);
    /* Not self_ref: see HlJSWsClientUD. */
}

static const JSClassDef js_ws_client_conn_class = {
    "WsClientConn",
    .finalizer = js_ws_client_conn_finalizer,
    .gc_mark = js_ws_client_conn_gc_mark,
};

/* Client conn methods - send/sendBinary/close/ping */

static JSValue js_ws_client_send(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque(this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_ThrowTypeError(ctx, "client connection closed");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data) return JS_EXCEPTION;
    /* The toString above is app code, which may have closed the client
     * (audit 23). */
    if (!ud->client || ud->closed) {
        JS_FreeCString(ctx, data);
        return JS_ThrowTypeError(ctx, "client connection closed");
    }
    if (hl_js_budget_charge(ctx, hl_work_send_units(len, 1))) {   /* audit 14 */
        JS_FreeCString(ctx, data);
        return JS_EXCEPTION;
    }

    int rc = kl_ws_client_send_text(ud->client, data, len);
    JS_FreeCString(ctx, data);
    return rc == 0 ? JS_TRUE : JS_ThrowTypeError(ctx, "send failed");
}

static JSValue js_ws_client_send_binary(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)argc;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque(this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_ThrowTypeError(ctx, "client connection closed");

    /* Any byte view (a TypedArray or DataView too, not only a bare
     * ArrayBuffer - a Uint8Array went out as its toString, "1,2,3"), or a
     * string; no app code runs (audit 22). */
    HlBufferView view = {0};
    const char *str = NULL;
    int needs_free = 0;
    if (!js_get_buffer(ctx, argv[0], &view, &str, &needs_free))
        return JS_ThrowTypeError(ctx, "ws.sendBinary: expected ArrayBuffer, TypedArray "
                                 "or string");
    if (hl_js_budget_charge(ctx, hl_work_send_units(view.len, 1))) {   /* audit 14 */
        if (needs_free && str) JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    int rc = kl_ws_client_send_binary(ud->client, (const char *)view.data, view.len);
    if (needs_free && str) JS_FreeCString(ctx, str);
    return rc == 0 ? JS_TRUE : JS_ThrowTypeError(ctx, "send failed");
}

static JSValue js_ws_client_close(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque(this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_UNDEFINED;

    /* A valueOf / toString that throws fails the call (audit 23: the
     * exception was left pending and the close went out with a default),
     * and, being app code, may itself have closed the client. */
    uint32_t code = 1000;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) &&
        JS_ToUint32(ctx, &code, argv[0]) < 0)
        return JS_EXCEPTION;

    const char *reason = NULL;
    size_t reason_len = 0;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        reason = JS_ToCStringLen(ctx, &reason_len, argv[1]);
        if (!reason) return JS_EXCEPTION;
    }
    if (!ud->client || ud->closed) {
        if (reason) JS_FreeCString(ctx, reason);
        return JS_UNDEFINED;
    }

    kl_ws_client_close(ud->client, (uint16_t)code, reason, reason_len);
    if (reason) JS_FreeCString(ctx, reason);
    return JS_UNDEFINED;
}

static JSValue js_ws_client_ping(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque(this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_UNDEFINED;

    const char *data = NULL;
    size_t len = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        data = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!data) return JS_EXCEPTION;   /* audit 23 */
    }
    /* The toString is app code: it may have closed the client. */
    if (!ud->client || ud->closed) {
        if (data) JS_FreeCString(ctx, data);
        return JS_UNDEFINED;
    }

    kl_ws_client_send_ping(ud->client, data, len);
    if (data) JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_ws_client_conn_proto_funcs[] = {
    JS_CFUNC_DEF("send", 1, js_ws_client_send),
    JS_CFUNC_DEF("sendBinary", 1, js_ws_client_send_binary),
    JS_CFUNC_DEF("close", 0, js_ws_client_close),
    JS_CFUNC_DEF("ping", 0, js_ws_client_ping),
};

/* ── Client callbacks ──────────────────────────────────────────────── */

/* Every callback is an entry point, like the ws-server ones (ws.c): no
 * request is active - left as the last run had it, an op the callback made
 * suspended (and later completed) that request's connection, mid-upload -
 * nothing chains into an earlier run, the budget is the callback's own, and
 * its microtasks are drained before it returns. Left queued, they ran in the
 * next HTTP request's drain, with that request's connection and life
 * active. */
static void js_ws_client_call(HlJSWsClientUD *ud, JSValueConst fn,
                              int argc, JSValueConst *argv, const char *what)
{
    JSContext *ctx = ud->ctx;
    HlJS *js = ud->js;
    if (js) {
        js->active_conn = NULL;
        js->active_req = NULL;
        js->active_timer = NULL;
        js->active_life = NULL;
        js->active_on_complete = NULL;
        js->active_on_complete_ctx = NULL;
        js->last_async_cont = NULL;
        js->async_pending = 0;
        hl_js_budget_arm(js);
        hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */
    }
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    if (JS_IsException(ret)) {
        int tripped = js && js->budget_tripped;
        JSValue exc = JS_GetException(ctx);
        const char *msg = tripped ? NULL : JS_ToCString(ctx, exc);
        log_error("[hull:ws:client] %s error: %s", what,
                  msg ? msg : tripped ? hl_js_trip_reason(js) : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        else JS_FreeValue(ctx, JS_GetException(ctx));   /* a throwing toString (audit 10) */
        JS_FreeValue(ctx, exc);
    } else if (js) {
        /* An async callback is a run like any other entry's: its queued
         * code runs now, its continuations are wired to its promise, and
         * its wait is checked - a transaction open across it is rolled back
         * and the callback failed at its next resume, not continued in
         * autocommit (audit 7 M3). */
        int st;
        (void)hl_js_entry_park(js, ret, &st);
    }
    JS_FreeValue(ctx, ret);
    if (js) {
        hl_js_run_jobs(js);
        js->last_async_cont = NULL;   /* an un-awaited op belongs to no run */
        hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */
    }
}

static void js_ws_client_on_open(KlWsClientConn *ws, void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    if (JS_IsUndefined(ud->on_open))
        return;

    JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
    js_ws_client_call(ud, ud->on_open, 1, &conn_obj, "on_open");
    JS_FreeValue(ud->ctx, conn_obj);
}

static void js_ws_client_on_message(KlWsClientConn *ws, const char *data,
                                      size_t len, int is_binary, void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    if (JS_IsUndefined(ud->on_message))
        return;

    JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
    JSValue msg_val;
    if (is_binary) {
        msg_val = JS_NewArrayBufferCopy(ud->ctx, (const uint8_t *)data, len);
    } else {
        msg_val = JS_NewStringLen(ud->ctx, data, len);
    }
    JSValue is_bin_val = JS_NewBool(ud->ctx, is_binary);

    JSValue args[3] = { conn_obj, msg_val, is_bin_val };
    js_ws_client_call(ud, ud->on_message, 3, args, "on_message");
    JS_FreeValue(ud->ctx, conn_obj);
    JS_FreeValue(ud->ctx, msg_val);
    JS_FreeValue(ud->ctx, is_bin_val);
}

/* The deferred half of on_close: drop the self-reference on the loop turn
 * after Keel's close callback. That may run the finalizer, which frees ud
 * and the KlWsClientConn. */
static void js_ws_client_release_self(void *user_data)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    ud->release_timer = 0;   /* fired */
    if (JS_IsUndefined(ud->self_ref)) return;
    JSValue self = ud->self_ref;
    ud->self_ref = JS_UNDEFINED;
    JS_FreeValue(ud->ctx, self);
}

/* The connection is over (closed, or failed - Keel closes it before calling
 * on_error, and fires no on_close after). */
static void js_ws_client_finish(HlJSWsClientUD *ud)
{
    /* Mark closed (methods fail closed) but keep `client` non-NULL so the
     * finalizer frees the KlWsClientConn - Keel does NOT free it on close,
     * and we must not free it here (Keel touches `ws` immediately after this
     * callback returns: `ws->state = WSC_CLOSED; wsc_close_connection(ws)`). */
    ud->closed = 1;

    /* Release the self-reference on the next loop turn, never here. When it
     * is the last reference QuickJS runs the finalizer at once, freeing ud
     * (this function then wrote ud->closed into freed memory) and the
     * KlWsClientConn Keel is about to use. If no timer can be had the
     * reference is kept: a leak until teardown, not a use after free. */
    if (!JS_IsUndefined(ud->self_ref) && !ud->release_scheduled) {
        const HlAsyncBackend *be = hl_async_backend();
        uint64_t h = (be && ud->js->base.async_ctx)
            ? be->timer_add(ud->js->base.async_ctx, 0,
                            js_ws_client_release_self, ud)
            : 0;
        if (h == 0) {
            log_warn("[hull:ws:client] could not schedule the close "
                     "release; the client is kept until shutdown");
        } else {
            ud->release_scheduled = 1;
            ud->release_timer = h;
        }
    }
}

static void js_ws_client_on_close(KlWsClientConn *ws, uint16_t code,
                                    const char *reason, size_t reason_len,
                                    void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;

    if (!JS_IsUndefined(ud->on_close)) {
        JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
        JSValue code_val = JS_NewInt32(ud->ctx, code);
        JSValue reason_val = (reason && reason_len > 0)
                                 ? JS_NewStringLen(ud->ctx, reason, reason_len)
                                 : JS_NULL;

        JSValue args[3] = { conn_obj, code_val, reason_val };
        js_ws_client_call(ud, ud->on_close, 3, args, "on_close");
        JS_FreeValue(ud->ctx, conn_obj);
        JS_FreeValue(ud->ctx, code_val);
        JS_FreeValue(ud->ctx, reason_val);
    }

    js_ws_client_finish(ud);
}

static void js_ws_client_on_error(KlWsClientConn *ws, const char *msg,
                                    void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    if (JS_IsUndefined(ud->on_error)) {
        log_error("[hull:ws:client] error: %s", msg ? msg : "unknown");
        js_ws_client_finish(ud);
        return;
    }

    JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
    JSValue msg_val = JS_NewString(ud->ctx, msg ? msg : "unknown");

    JSValue args[2] = { conn_obj, msg_val };
    js_ws_client_call(ud, ud->on_error, 2, args, "on_error callback");
    JS_FreeValue(ud->ctx, conn_obj);
    JS_FreeValue(ud->ctx, msg_val);
    /* Terminal: Keel closed the connection and will not call on_close, so
     * this is where the client is released (it stayed pinned until VM
     * teardown). */
    js_ws_client_finish(ud);
}

/* ── ws module functions ───────────────────────────────────────────── */


/* ── connect() ────────────────────────────────────────────────────── */

static JSValue js_ws_connect(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    /* `js` is read on lines 316/332/371/etc.; cppcheck's data-flow
     * loses track across the early-return below. */
    /* cppcheck-suppress unreadVariable */
    HlJS *js = get_hl_js(ctx);

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ws.connect requires (url, handlers)");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    /* Check host allowlist */
    KlUrl parsed;
    if (kl_url_parse(url, &parsed) != 0) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "invalid WebSocket URL");
    }

#ifdef HL_ENABLE_HTTP_CLIENT
    /* No http config means an empty manifest.hosts: nothing is allowed. This
     * used to skip the check, so ws.connect reached any host. */
    if (!js->base.http_cfg ||
        hl_http_check_host(js->base.http_cfg, parsed.host,
                           parsed.host_len) != 0) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "host not in allowlist");
    }
#else
    /* Without HL_ENABLE_HTTP_CLIENT the host allowlist function isn't
     * compiled in. ws.connect on a server-only build can't dial out
     * to arbitrary hosts - fail closed. */
    JS_FreeCString(ctx, url);
    return JS_ThrowTypeError(ctx,
        "ws.connect requires HL_ENABLE_HTTP_CLIENT (build-time)");
#endif

    if (!js->server) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "ws.connect requires running server");
    }
    /* A finalizer run at teardown: the loop is gone (audit 16). */
    if (js->base.shutting_down || !js->base.async_ctx) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "ws.connect: runtime is shutting down");
    }

    /* Extract callbacks from handlers object */
    JSValue on_open = JS_GetPropertyStr(ctx, argv[1], "onOpen");
    JSValue on_message = JS_IsException(on_open) ? JS_EXCEPTION
                       : JS_GetPropertyStr(ctx, argv[1], "onMessage");
    JSValue on_close = JS_IsException(on_message) ? JS_EXCEPTION
                     : JS_GetPropertyStr(ctx, argv[1], "onClose");
    JSValue on_error = JS_IsException(on_close) ? JS_EXCEPTION
                     : JS_GetPropertyStr(ctx, argv[1], "onError");
    /* A throwing getter fails the call (audit 23: the client connected
     * without that handler and the exception pending). */
    if (JS_IsException(on_error)) {
        JS_FreeCString(ctx, url);
        JS_FreeValue(ctx, on_open);
        JS_FreeValue(ctx, on_message);
        JS_FreeValue(ctx, on_close);
        return JS_EXCEPTION;
    }

    /* Create JS client conn object */
    JSValue obj = JS_NewObjectClass(ctx, (int)js_ws_client_conn_class_id);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, url);
        JS_FreeValue(ctx, on_open);
        JS_FreeValue(ctx, on_message);
        JS_FreeValue(ctx, on_close);
        JS_FreeValue(ctx, on_error);
        return obj;
    }

    HlJSWsClientUD *ud = js_malloc(ctx, sizeof(HlJSWsClientUD));
    if (!ud) {
        JS_FreeCString(ctx, url);
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, on_open);
        JS_FreeValue(ctx, on_message);
        JS_FreeValue(ctx, on_close);
        JS_FreeValue(ctx, on_error);
        return JS_EXCEPTION;
    }
    memset(ud, 0, sizeof *ud);

    ud->on_open = JS_IsFunction(ctx, on_open) ? on_open : (JS_FreeValue(ctx, on_open), JS_UNDEFINED);
    ud->on_message = JS_IsFunction(ctx, on_message) ? on_message : (JS_FreeValue(ctx, on_message), JS_UNDEFINED);
    ud->on_close = JS_IsFunction(ctx, on_close) ? on_close : (JS_FreeValue(ctx, on_close), JS_UNDEFINED);
    ud->on_error = JS_IsFunction(ctx, on_error) ? on_error : (JS_FreeValue(ctx, on_error), JS_UNDEFINED);
    ud->self_ref = JS_DupValue(ctx, obj); /* a GC root while connected */
    ud->release_scheduled = 0;
    ud->release_timer = 0;
    ud->ctx = ctx;
    ud->js = js;
    ud->client = NULL;
    ud->closed = 0;

    JS_SetOpaque(obj, ud);

    /* Connect */
    KlWsClientCallbacks cbs = {
        .on_open = js_ws_client_on_open,
        .on_message = js_ws_client_on_message,
        .on_close = js_ws_client_on_close,
        .on_error = js_ws_client_on_error,
    };

    KlWsClientConn *client = kl_ws_client_connect(
        &js->server->ev, &js->server->alloc_storage, NULL, url, &cbs, ud);

    JS_FreeCString(ctx, url);

    if (!client) {
        /* Connection failed immediately - clean up self_ref */
        JS_FreeValue(ctx, ud->self_ref);
        ud->self_ref = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "WebSocket connect failed");
    }

    ud->client = client;
    ud->teardown.stop = js_ws_client_teardown_stop;
    hl_async_teardown_link(&js->base.async_live, &ud->teardown);

    return obj;
}

/* ── Module init ───────────────────────────────────────────────────── */

static int js_ws_client_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/web/ws-client",
                                    "hull:web:ws-client") != 0) return -1;

    JSValue ws = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ws, "connect",
                      JS_NewCFunction(ctx, js_ws_connect, "connect", 2));
    hl_js_export_frozen(ctx, m, "wsClient", ws);
    return 0;
}

int hl_js_init_ws_client_module(JSContext *ctx, HlJS *js)
{
    (void)js;

    JS_NewClassID(&js_ws_client_conn_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_ws_client_conn_class_id,
                &js_ws_client_conn_class);
    JSValue client_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, client_proto, js_ws_client_conn_proto_funcs,
                               sizeof(js_ws_client_conn_proto_funcs) /
                               sizeof(js_ws_client_conn_proto_funcs[0]));
    hl_js_set_class_proto_frozen(ctx, js_ws_client_conn_class_id, client_proto);

    JSModuleDef *m = JS_NewCModule(ctx, "hull:web:ws-client", js_ws_client_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "wsClient");
    return 0;
}
