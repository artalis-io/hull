/*
 * mod_ws_server.c — hull:web:ws-server module (server-side WebSocket helpers + conn class)
 *
 * Exposes: ws.broadcast(path, data [, binary])
 *          ws.connections(path)
 *          per-connection methods: send / sendBinary / close / ping
 *          conn.data (per-connection storage)
 *
 * Client-side WebSocket (ws.connect, outbound) lives in mod_ws_client.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/ws.h"
#include "hull/cap/http.h"
#include "hull/utils/alloc.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>
#include <keel/websocket_client.h>
#include <keel/url.h>

#include "log.h"

#include <string.h>

/* ── JS class IDs ──────────────────────────────────────────────────── */

JSClassID js_ws_conn_class_id;

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


/* ── Module functions ──────────────────────────────────────────────── */

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

/* ── Module init ───────────────────────────────────────────────────── */

static int js_ws_server_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/web/ws-server",
                                    "hull:web:ws-server") != 0) return -1;

    JSValue ws = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ws, "broadcast",
                      JS_NewCFunction(ctx, js_ws_broadcast, "broadcast", 2));
    JS_SetPropertyStr(ctx, ws, "connections",
                      JS_NewCFunction(ctx, js_ws_connections, "connections", 1));
    JS_SetModuleExport(ctx, m, "wsServer", ws);
    return 0;
}

int hl_js_init_ws_server_module(JSContext *ctx, HlJS *js)
{
    (void)js;

    JS_NewClassID(&js_ws_conn_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_ws_conn_class_id, &js_ws_conn_class);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_ws_conn_proto_funcs,
                               sizeof(js_ws_conn_proto_funcs) /
                               sizeof(js_ws_conn_proto_funcs[0]));
    JS_SetClassProto(ctx, js_ws_conn_class_id, proto);

    JSModuleDef *m = JS_NewCModule(ctx, "hull:web:ws-server", js_ws_server_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "wsServer");
    return 0;
}
