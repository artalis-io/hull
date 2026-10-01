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
#include "hull/cap/http.h"
#include "hull/utils/alloc.h"

#include <keel/keel.h>
#include <keel/websocket_client.h>
#include <keel/url.h>

#include "hull/shared/async_backend.h"
#include "log.h"

#include <string.h>

/* Client class id is module-internal - no other file references it. */
static JSClassID js_ws_client_conn_class_id;

typedef struct {
    KlWsClientConn *client;      /* NULL after free (finalizer only) */
    int             closed;      /* 1 after on_close: fail methods closed,
                                  * but keep `client` so the finalizer still
                                  * frees it (Keel does not free the conn on
                                  * close) */
    JSValue         on_open;     /* DupValue'd callbacks */
    JSValue         on_message;
    JSValue         on_close;
    JSValue         on_error;
    JSValue         self_ref;    /* DupValue'd self object (prevent GC while active) */
    int             release_scheduled;  /* self_ref release already queued */
    JSContext      *ctx;
    HlJS           *js;
} HlJSWsClientUD;

static void js_ws_client_conn_finalizer(JSRuntime *rt, JSValue val)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque(val,
                                                           js_ws_client_conn_class_id);
    if (!ud) return;

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
    /* self_ref is the cycle-breaking handle dropped by on_close (or
     * never installed if connect failed early); freeing it here would
     * double-decrement the GC refcount. */
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
    if (!JS_IsUndefined(ud->self_ref)) JS_MarkValue(rt, ud->self_ref, mark_func);
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
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque2(ctx, this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_ThrowTypeError(ctx, "client connection closed");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data) return JS_EXCEPTION;

    int rc = kl_ws_client_send_text(ud->client, data, len);
    JS_FreeCString(ctx, data);
    return rc == 0 ? JS_TRUE : JS_ThrowTypeError(ctx, "send failed");
}

static JSValue js_ws_client_send_binary(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)argc;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque2(ctx, this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_ThrowTypeError(ctx, "client connection closed");

    size_t len;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!buf) {
        const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!data) return JS_EXCEPTION;
        int rc = kl_ws_client_send_binary(ud->client, data, len);
        JS_FreeCString(ctx, data);
        return rc == 0 ? JS_TRUE : JS_ThrowTypeError(ctx, "send failed");
    }
    int rc = kl_ws_client_send_binary(ud->client, (const char *)buf, len);
    return rc == 0 ? JS_TRUE : JS_ThrowTypeError(ctx, "send failed");
}

static JSValue js_ws_client_close(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque2(ctx, this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_UNDEFINED;

    uint32_t code = 1000;
    if (argc >= 1 && !JS_IsUndefined(argv[0]))
        JS_ToUint32(ctx, &code, argv[0]);

    const char *reason = NULL;
    size_t reason_len = 0;
    if (argc >= 2 && !JS_IsUndefined(argv[1]))
        reason = JS_ToCStringLen(ctx, &reason_len, argv[1]);

    kl_ws_client_close(ud->client, (uint16_t)code, reason, reason_len);
    if (reason) JS_FreeCString(ctx, reason);
    return JS_UNDEFINED;
}

static JSValue js_ws_client_ping(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque2(ctx, this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client || ud->closed)
        return JS_UNDEFINED;

    const char *data = NULL;
    size_t len = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0]))
        data = JS_ToCStringLen(ctx, &len, argv[0]);

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

static void js_ws_client_on_open(KlWsClientConn *ws, void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    if (JS_IsUndefined(ud->on_open))
        return;

    JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
    JSValue ret = JS_Call(ud->ctx, ud->on_open, JS_UNDEFINED, 1, &conn_obj);
    JS_FreeValue(ud->ctx, conn_obj);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ud->ctx);
        const char *msg = JS_ToCString(ud->ctx, exc);
        log_error("[hull:ws:client] on_open error: %s", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ud->ctx, msg);
        JS_FreeValue(ud->ctx, exc);
    }
    JS_FreeValue(ud->ctx, ret);
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
    JSValue ret = JS_Call(ud->ctx, ud->on_message, JS_UNDEFINED, 3, args);
    JS_FreeValue(ud->ctx, conn_obj);
    JS_FreeValue(ud->ctx, msg_val);
    JS_FreeValue(ud->ctx, is_bin_val);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ud->ctx);
        const char *msg2 = JS_ToCString(ud->ctx, exc);
        log_error("[hull:ws:client] on_message error: %s", msg2 ? msg2 : "unknown");
        if (msg2) JS_FreeCString(ud->ctx, msg2);
        JS_FreeValue(ud->ctx, exc);
    }
    JS_FreeValue(ud->ctx, ret);
}

/* The deferred half of on_close: drop the self-reference on the loop turn
 * after Keel's close callback. That may run the finalizer, which frees ud
 * and the KlWsClientConn. */
static void js_ws_client_release_self(void *user_data)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
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
        if (!be || !ud->js->base.async_ctx ||
            be->timer_add(ud->js->base.async_ctx, 0,
                          js_ws_client_release_self, ud) == 0)
            log_warn("[hull:ws:client] could not schedule the close "
                     "release; the client is kept until shutdown");
        else
            ud->release_scheduled = 1;
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
        JSValue ret = JS_Call(ud->ctx, ud->on_close, JS_UNDEFINED, 3, args);
        JS_FreeValue(ud->ctx, conn_obj);
        JS_FreeValue(ud->ctx, code_val);
        JS_FreeValue(ud->ctx, reason_val);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ud->ctx);
            const char *msg = JS_ToCString(ud->ctx, exc);
            log_error("[hull:ws:client] on_close error: %s", msg ? msg : "unknown");
            if (msg) JS_FreeCString(ud->ctx, msg);
            JS_FreeValue(ud->ctx, exc);
        }
        JS_FreeValue(ud->ctx, ret);
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
    JSValue ret = JS_Call(ud->ctx, ud->on_error, JS_UNDEFINED, 2, args);
    JS_FreeValue(ud->ctx, conn_obj);
    JS_FreeValue(ud->ctx, msg_val);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ud->ctx);
        const char *emsg = JS_ToCString(ud->ctx, exc);
        log_error("[hull:ws:client] on_error callback error: %s",
                  emsg ? emsg : "unknown");
        if (emsg) JS_FreeCString(ud->ctx, emsg);
        JS_FreeValue(ud->ctx, exc);
    }
    JS_FreeValue(ud->ctx, ret);
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

    /* Extract callbacks from handlers object */
    JSValue on_open = JS_GetPropertyStr(ctx, argv[1], "onOpen");
    JSValue on_message = JS_GetPropertyStr(ctx, argv[1], "onMessage");
    JSValue on_close = JS_GetPropertyStr(ctx, argv[1], "onClose");
    JSValue on_error = JS_GetPropertyStr(ctx, argv[1], "onError");

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

    ud->on_open = JS_IsFunction(ctx, on_open) ? on_open : (JS_FreeValue(ctx, on_open), JS_UNDEFINED);
    ud->on_message = JS_IsFunction(ctx, on_message) ? on_message : (JS_FreeValue(ctx, on_message), JS_UNDEFINED);
    ud->on_close = JS_IsFunction(ctx, on_close) ? on_close : (JS_FreeValue(ctx, on_close), JS_UNDEFINED);
    ud->on_error = JS_IsFunction(ctx, on_error) ? on_error : (JS_FreeValue(ctx, on_error), JS_UNDEFINED);
    ud->self_ref = JS_DupValue(ctx, obj); /* prevent GC while connected */
    ud->release_scheduled = 0;
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
    JS_SetModuleExport(ctx, m, "wsClient", ws);
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
    JS_SetClassProto(ctx, js_ws_client_conn_class_id, client_proto);

    JSModuleDef *m = JS_NewCModule(ctx, "hull:web:ws-client", js_ws_client_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "wsClient");
    return 0;
}
