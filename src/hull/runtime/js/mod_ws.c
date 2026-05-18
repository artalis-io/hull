/*
 * mod_ws.c — hull:ws module + WebSocket conn JS class (QuickJS)
 *
 * Provides:
 *   - ws.broadcast(path, data, binary?)
 *   - ws.connections(path)
 *   - ws.connect(url, handlers, opts?) — client WebSocket
 *   - conn.id (getter), conn.path (getter)
 *   - conn.send(text), conn.sendBinary(data), conn.close(code, reason), conn.ping(data)
 *   - conn.data — JS object (per-connection storage)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/ws.h"
#include "hull/cap/http.h"
#include "hull/alloc.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>
#include <keel/websocket_client.h>
#include <keel/url.h>

#include "log.h"

#include <string.h>

/* ── JS class IDs ──────────────────────────────────────────────────── */

JSClassID js_ws_conn_class_id;
JSClassID js_ws_client_conn_class_id;

/* ── Server conn opaque ───────────────────────────────────────────── */

typedef struct {
    HlWsConn *conn;       /* NULL after close */
    JSValue   data_val;   /* per-connection data object (JS_UNDEFINED = none) */
    JSContext *ctx;        /* owning context */
} HlJSWsConnUD;

/* Push or create the conn JS object for a given HlWsConn.
 * Uses the user_data pointer on HlWsConn to cache the JS object. */
JSValue hl_js_ws_push_conn(JSContext *ctx, HlWsConn *conn)
{
    if (conn->user_data) {
        /* Already have a JS object — return dup'd ref */
        JSValue *stored = (JSValue *)conn->user_data;
        return JS_DupValue(ctx, *stored);
    }

    /* Create new JS object */
    JSValue obj = JS_NewObjectClass(ctx, (int)js_ws_conn_class_id);
    if (JS_IsException(obj))
        return obj;

    HlJSWsConnUD *ud = js_malloc(ctx, sizeof(HlJSWsConnUD));
    if (!ud) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    ud->conn = conn;
    ud->data_val = JS_UNDEFINED;
    ud->ctx = ctx;
    JS_SetOpaque(obj, ud);

    /* Store a dup'd value on the conn for lookup */
    JSValue *stored = js_malloc(ctx, sizeof(JSValue));
    if (!stored) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    *stored = JS_DupValue(ctx, obj);
    conn->user_data = stored;

    return obj;
}

/* Invalidate and free the conn JS object. Called from on_close trampoline. */
void hl_js_ws_invalidate_conn(JSContext *ctx, HlWsConn *conn)
{
    if (!conn->user_data)
        return;

    JSValue *stored = (JSValue *)conn->user_data;
    JSValue obj = *stored;

    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque(obj, js_ws_conn_class_id);
    if (ud) {
        if (!JS_IsUndefined(ud->data_val))
            JS_FreeValue(ctx, ud->data_val);
        ud->data_val = JS_UNDEFINED;
        ud->conn = NULL;
    }

    JS_FreeValue(ctx, obj);
    js_free(ctx, stored);
    conn->user_data = NULL;
}

/* ── Conn class finalizer ──────────────────────────────────────────── */

static void js_ws_conn_finalizer(JSRuntime *rt, JSValue val)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque(val, js_ws_conn_class_id);
    if (ud) {
        if (!JS_IsUndefined(ud->data_val))
            JS_FreeValueRT(rt, ud->data_val);
        js_free_rt(rt, ud);
    }
}

static void js_ws_conn_gc_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque(val, js_ws_conn_class_id);
    if (ud && !JS_IsUndefined(ud->data_val))
        JS_MarkValue(rt, ud->data_val, mark_func);
}

static JSClassDef js_ws_conn_class = {
    "WsConn",
    .finalizer = js_ws_conn_finalizer,
    .gc_mark = js_ws_conn_gc_mark,
};

/* ── Conn getters ──────────────────────────────────────────────────── */

static JSValue js_ws_conn_get_id(JSContext *ctx, JSValueConst this_val)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud || !ud->conn)
        return JS_ThrowTypeError(ctx, "WebSocket connection is closed");
    return JS_NewInt64(ctx, (int64_t)ud->conn->id);
}

static JSValue js_ws_conn_get_path(JSContext *ctx, JSValueConst this_val)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud || !ud->conn)
        return JS_ThrowTypeError(ctx, "WebSocket connection is closed");
    return JS_NewString(ctx, ud->conn->path);
}

static JSValue js_ws_conn_get_data(JSContext *ctx, JSValueConst this_val)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud)
        return JS_UNDEFINED;
    if (JS_IsUndefined(ud->data_val)) {
        ud->data_val = JS_NewObject(ctx);
    }
    return JS_DupValue(ctx, ud->data_val);
}

static JSValue js_ws_conn_set_data(JSContext *ctx, JSValueConst this_val,
                                     JSValueConst val)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud)
        return JS_UNDEFINED;
    if (!JS_IsUndefined(ud->data_val))
        JS_FreeValue(ctx, ud->data_val);
    ud->data_val = JS_DupValue(ctx, val);
    return JS_UNDEFINED;
}

/* ── Conn methods ──────────────────────────────────────────────────── */

static JSValue js_ws_conn_send(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc;
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud || !ud->conn || ud->conn->closed || !ud->conn->kl_ws)
        return JS_ThrowTypeError(ctx, "connection closed");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data) return JS_EXCEPTION;

    int rc = kl_ws_server_send_text(ud->conn->kl_ws, data, len);
    JS_FreeCString(ctx, data);

    if (rc == 0)
        return JS_TRUE;
    return JS_ThrowTypeError(ctx, "send failed");
}

static JSValue js_ws_conn_send_binary(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc;
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud || !ud->conn || ud->conn->closed || !ud->conn->kl_ws)
        return JS_ThrowTypeError(ctx, "connection closed");

    size_t len;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!buf) {
        /* Try string fallback */
        const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!data) return JS_EXCEPTION;
        int rc = kl_ws_server_send_binary(ud->conn->kl_ws, data, len);
        JS_FreeCString(ctx, data);
        return rc == 0 ? JS_TRUE : JS_ThrowTypeError(ctx, "send failed");
    }

    int rc = kl_ws_server_send_binary(ud->conn->kl_ws, (const char *)buf, len);
    return rc == 0 ? JS_TRUE : JS_ThrowTypeError(ctx, "send failed");
}

static JSValue js_ws_conn_close(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud || !ud->conn || ud->conn->closed || !ud->conn->kl_ws)
        return JS_UNDEFINED;

    uint32_t code = 1000;
    if (argc >= 1 && !JS_IsUndefined(argv[0]))
        JS_ToUint32(ctx, &code, argv[0]);

    const char *reason = NULL;
    size_t reason_len = 0;
    if (argc >= 2 && !JS_IsUndefined(argv[1]))
        reason = JS_ToCStringLen(ctx, &reason_len, argv[1]);

    kl_ws_server_close(ud->conn->kl_ws, (uint16_t)code, reason, reason_len);
    ud->conn->closed = 1;

    if (reason) JS_FreeCString(ctx, reason);
    return JS_UNDEFINED;
}

static JSValue js_ws_conn_ping(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    HlJSWsConnUD *ud = (HlJSWsConnUD *)JS_GetOpaque2(ctx, this_val,
                                                         js_ws_conn_class_id);
    if (!ud || !ud->conn || ud->conn->closed || !ud->conn->kl_ws)
        return JS_UNDEFINED;

    const char *data = NULL;
    size_t len = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0]))
        data = JS_ToCStringLen(ctx, &len, argv[0]);

    kl_ws_server_send_ping(ud->conn->kl_ws, data, len);
    if (data) JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

/* ── Conn class proto setup ────────────────────────────────────────── */

static const JSCFunctionListEntry js_ws_conn_proto_funcs[] = {
    JS_CGETSET_DEF("id", js_ws_conn_get_id, NULL),
    JS_CGETSET_DEF("path", js_ws_conn_get_path, NULL),
    JS_CGETSET_DEF("data", js_ws_conn_get_data, js_ws_conn_set_data),
    JS_CFUNC_DEF("send", 1, js_ws_conn_send),
    JS_CFUNC_DEF("sendBinary", 1, js_ws_conn_send_binary),
    JS_CFUNC_DEF("close", 0, js_ws_conn_close),
    JS_CFUNC_DEF("ping", 0, js_ws_conn_ping),
};

/* ════════════════════════════════════════════════════════════════════
 * Client WebSocket conn
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    KlWsClientConn *client;      /* NULL after free */
    JSValue         on_open;     /* DupValue'd callbacks */
    JSValue         on_message;
    JSValue         on_close;
    JSValue         on_error;
    JSValue         self_ref;    /* DupValue'd self object (prevent GC while active) */
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

    JS_FreeValueRT(rt, ud->on_open);
    JS_FreeValueRT(rt, ud->on_message);
    JS_FreeValueRT(rt, ud->on_close);
    JS_FreeValueRT(rt, ud->on_error);
    /* self_ref already freed when we clear it */
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

static JSClassDef js_ws_client_conn_class = {
    "WsClientConn",
    .finalizer = js_ws_client_conn_finalizer,
    .gc_mark = js_ws_client_conn_gc_mark,
};

/* Client conn methods — send/sendBinary/close/ping */

static JSValue js_ws_client_send(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)JS_GetOpaque2(ctx, this_val,
                                                             js_ws_client_conn_class_id);
    if (!ud || !ud->client)
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
    if (!ud || !ud->client)
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
    if (!ud || !ud->client)
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
    if (!ud || !ud->client)
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

    /* Release self-reference — allow GC */
    if (!JS_IsUndefined(ud->self_ref)) {
        JS_FreeValue(ud->ctx, ud->self_ref);
        ud->self_ref = JS_UNDEFINED;
    }
    ud->client = NULL;
}

static void js_ws_client_on_error(KlWsClientConn *ws, const char *msg,
                                    void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    if (JS_IsUndefined(ud->on_error)) {
        log_error("[hull:ws:client] error: %s", msg ? msg : "unknown");
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
}

/* ── ws module functions ───────────────────────────────────────────── */

static JSValue js_ws_broadcast(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = get_hl_js(ctx);
    HlWsRegistry *reg = js->base.ws_registry;
    if (!reg)
        return JS_NewInt32(ctx, 0);

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ws.broadcast requires (path, data)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!data) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }

    int binary = 0;
    if (argc >= 3)
        binary = JS_ToBool(ctx, argv[2]);

    int sent;
    if (binary)
        sent = hl_ws_broadcast_binary(reg, path, data, len);
    else
        sent = hl_ws_broadcast_text(reg, path, data, len);

    JS_FreeCString(ctx, data);
    JS_FreeCString(ctx, path);

    return JS_NewInt32(ctx, sent);
}

static JSValue js_ws_connections(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = get_hl_js(ctx);
    HlWsRegistry *reg = js->base.ws_registry;
    if (!reg)
        return JS_NewInt32(ctx, 0);

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ws.connections requires (path)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    size_t count = hl_ws_connections(reg, path);
    JS_FreeCString(ctx, path);

    return JS_NewInt64(ctx, (int64_t)count);
}

/* ws.connect(url, handlers, opts?) — client WebSocket */
static JSValue js_ws_connect(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
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

    if (js->base.http_cfg) {
        if (hl_http_check_host(js->base.http_cfg, parsed.host,
                                parsed.host_len) != 0) {
            JS_FreeCString(ctx, url);
            return JS_ThrowTypeError(ctx, "host not in allowlist");
        }
    }

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
    ud->ctx = ctx;
    ud->js = js;
    ud->client = NULL;

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
        /* Connection failed immediately — clean up self_ref */
        JS_FreeValue(ctx, ud->self_ref);
        ud->self_ref = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "WebSocket connect failed");
    }

    ud->client = client;

    /* Track for cleanup on runtime destroy */
    if (js->ws_client_count >= js->ws_client_cap) {
        size_t new_cap = js->ws_client_cap ? js->ws_client_cap * 2 : 4;
        if (new_cap > SIZE_MAX / sizeof(void *)) {
            JS_FreeValue(ctx, ud->self_ref);
            ud->self_ref = JS_UNDEFINED;
            JS_FreeValue(ctx, obj);
            return JS_ThrowInternalError(ctx, "too many WebSocket clients");
        }
        size_t old_sz = js->ws_client_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(js->base.alloc,
                                           js->ws_clients, old_sz, new_sz);
        if (new_arr) {
            js->ws_clients = new_arr;
            js->ws_client_cap = new_cap;
        }
    }
    if (js->ws_client_count < js->ws_client_cap) {
        js->ws_clients[js->ws_client_count++] = ud;
    }

    return obj;
}

/* ── Module init ───────────────────────────────────────────────────── */

static int js_ws_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/ws", "hull:ws") != 0) return -1;

    JSValue ws = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, ws, "broadcast",
                      JS_NewCFunction(ctx, js_ws_broadcast, "broadcast", 2));
    JS_SetPropertyStr(ctx, ws, "connections",
                      JS_NewCFunction(ctx, js_ws_connections, "connections", 1));
    JS_SetPropertyStr(ctx, ws, "connect",
                      JS_NewCFunction(ctx, js_ws_connect, "connect", 2));

    JS_SetModuleExport(ctx, m, "ws", ws);
    return 0;
}

int hl_js_init_ws_module(JSContext *ctx, HlJS *js)
{
    (void)js;

    /* Register server conn class */
    JS_NewClassID(&js_ws_conn_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_ws_conn_class_id, &js_ws_conn_class);

    /* Set prototype */
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_ws_conn_proto_funcs,
                               sizeof(js_ws_conn_proto_funcs) /
                               sizeof(js_ws_conn_proto_funcs[0]));
    JS_SetClassProto(ctx, js_ws_conn_class_id, proto);

    /* Register client conn class */
    JS_NewClassID(&js_ws_client_conn_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_ws_client_conn_class_id,
                &js_ws_client_conn_class);

    JSValue client_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, client_proto, js_ws_client_conn_proto_funcs,
                               sizeof(js_ws_client_conn_proto_funcs) /
                               sizeof(js_ws_client_conn_proto_funcs[0]));
    JS_SetClassProto(ctx, js_ws_client_conn_class_id, client_proto);

    /* Register module */
    JSModuleDef *m = JS_NewCModule(ctx, "hull:ws", js_ws_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "ws");
    return 0;
}
