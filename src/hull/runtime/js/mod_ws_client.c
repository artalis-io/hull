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

#include "refusal.h"
#include "hull/cap/vm_thread.h"   /* --vm-thread */
#include "mod_buffer.h"
#include "hull/shared/work_charge.h"   /* hl_work_send_units */
#include "internal.h"            /* async gate, instruction budget */
#include "hull/cap/http.h"
#include "hull/limits/http.h"   /* HL_WS_CLIENT_MAX_LIVE (audit 32) */
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

/* Drop the connect deadline (on_open, on_error, on_close, teardown). */
static void js_ws_client_disarm_connect(HlJSWsClientUD *ud,
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
static void js_ws_client_free_conn(HlJSWsClientUD *ud)
{
    if (ud->client) {
        kl_ws_client_free(ud->client);
        ud->client = NULL;
    }
    hl_ws_client_slot_release(&ud->slot);
}

/* Runtime teardown, the loop still alive: free the Keel connection now and
 * drop the self-reference (a pending release timer is cancelled first), so
 * the object goes with the runtime. Releasing it can run the finalizer,
 * which frees ud: nothing touches ud after that. */
static void js_ws_client_teardown_stop(HlAsyncTeardown *t, HlAsyncBackendCtx *loop)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)(void *)
        ((char *)t - offsetof(HlJSWsClientUD, teardown));
    ud->closed = 1;
    js_ws_client_disarm_connect(ud, loop);
    js_ws_client_free_conn(ud);
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
    /* A deadline still armed holds ud raw: cancelled here, or it fired into
     * freed memory (audit 30 - a close timer conn.close() armed from onClose,
     * after the connection was over). At teardown the loop goes first and
     * teardown_stop already disarmed it; the Lua finalizer does the same. */
    if (ud->js && !ud->js->base.shutting_down)
        js_ws_client_disarm_connect(ud, ud->js->base.async_ctx);
    ud->connect_timer = 0;
    js_ws_client_free_conn(ud);

    /* Mirror the gc_mark guards: callbacks may have been cleared to
     * JS_UNDEFINED by on_close before the finalizer runs. Freeing
     * UNDEFINED is a no-op in QuickJS today, but the explicit check
     * documents the invariant and keeps the two callbacks symmetric. */
    if (!JS_IsUndefined(ud->on_open))    JS_FreeValueRT(rt, ud->on_open);
    if (!JS_IsUndefined(ud->on_message)) JS_FreeValueRT(rt, ud->on_message);
    if (!JS_IsUndefined(ud->on_close))   JS_FreeValueRT(rt, ud->on_close);
    if (!JS_IsUndefined(ud->on_error))   JS_FreeValueRT(rt, ud->on_error);
    if (ud->close_reason) js_free_rt(rt, ud->close_reason);   /* audit 27 */
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
    // cppcheck-suppress knownConditionTrueFalse
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

    /* An ArrayBuffer or a TypedArray (a Uint8Array went out as its
     * toString, "1,2,3"), or a string; no app code runs (audit 22). A
     * DataView is not a byte view js_get_buffer takes: it is refused. */
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

static void js_ws_client_close_expired(void *user_data);

/* Close-handshake deadline (audit 26): Keel waits in CLOSING for the peer's
 * Close with no timer, so a peer that never answered held the client, its fd
 * and the object until teardown. Armed only once the connection is open (the
 * connect deadline still runs before that, and Keel's close does nothing
 * then) and only once. */
static void js_ws_client_arm_close(HlJSWsClientUD *ud)
{
    if (!ud->closed && ud->client && !ud->close_armed &&
        ud->connect_timer == 0 && ud->js && !ud->js->base.shutting_down &&
        ud->js->base.async_ctx) {
        const HlAsyncBackend *be = hl_async_backend();
        ud->connect_timer = be ? be->timer_add(ud->js->base.async_ctx,
                                               HL_WS_CLIENT_CLOSE_TIMEOUT_MS,
                                               js_ws_client_close_expired, ud)
                               : 0;
        ud->close_armed = ud->connect_timer != 0;
    }
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
    // cppcheck-suppress knownConditionTrueFalse
    if (!ud->client || ud->closed) {
        if (reason) JS_FreeCString(ctx, reason);
        return JS_UNDEFINED;
    }

    /* Still connecting: Keel's close does nothing before the connection is
     * open, and the connection then stayed open (audit 27). Record the
     * first close; on_open sends it instead of calling onOpen. */
    if (!ud->opened) {
        if (!ud->close_requested) {
            char *copy = NULL;
            if (reason_len) {
                copy = js_malloc(ctx, reason_len);
                if (!copy) {
                    JS_FreeCString(ctx, reason);
                    return JS_EXCEPTION;
                }
                memcpy(copy, reason, reason_len);
            }
            ud->close_requested = 1;
            ud->close_code = (uint16_t)code;
            ud->close_reason = copy;
            ud->close_reason_len = reason_len;
        }
        if (reason) JS_FreeCString(ctx, reason);
        return JS_UNDEFINED;
    }

    kl_ws_client_close(ud->client, (uint16_t)code, reason, reason_len);
    if (reason) JS_FreeCString(ctx, reason);
    js_ws_client_arm_close(ud);
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
    // cppcheck-suppress knownConditionTrueFalse
    if (!ud->client || ud->closed) {
        if (data) JS_FreeCString(ctx, data);
        return JS_UNDEFINED;
    }
    /* Charged like a send (audit 37). */
    if (hl_js_budget_charge(ctx, hl_work_send_units(len, 1))) {
        if (data) JS_FreeCString(ctx, data);
        return JS_EXCEPTION;
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
        hl_js_budget_arm_as(js, HL_RUN_CALLBACK);
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
        (void)hl_js_entry_end_txn_check(js);   /* audit 6 M1, audit 35 */
    }
}

/* --vm-thread (cap/vm_thread.h): ws-client onOpen runs on the VM thread. */
static void js_ws_client_on_open_impl(KlWsClientConn *ws, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    void *user_data;
} js_ws_client_on_open_vmt_args;
static void js_ws_client_on_open_vmt_job(void *ud)
{
    js_ws_client_on_open_vmt_args *a = (js_ws_client_on_open_vmt_args *)ud;
    js_ws_client_on_open_impl(a->ws, a->user_data);
}
static void js_ws_client_on_open(KlWsClientConn *ws, void *user_data)
{
    HlVmThread *vmt = ((HlJSWsClientUD *)user_data)->js->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        js_ws_client_on_open_vmt_args a = { ws, user_data };
        (void)hl_vmt_run(vmt, js_ws_client_on_open_vmt_job, &a, NULL, NULL);
        return;
    }
    js_ws_client_on_open_impl(ws, user_data);
}

static void js_ws_client_on_open_impl(KlWsClientConn *ws, void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    js_ws_client_disarm_connect(ud, ud->js->base.async_ctx);
    ud->opened = 1;
    /* conn.close() ran while connecting (audit 27): send that close now,
     * under the close deadline, and do not report the connection open -
     * onClose follows, once (the peer's answer, or 1006 on the deadline). */
    if (ud->close_requested) {
        kl_ws_client_close(ud->client, ud->close_code, ud->close_reason,
                           ud->close_reason_len);
        if (ud->close_reason) js_free(ud->ctx, ud->close_reason);
        ud->close_reason = NULL;
        ud->close_reason_len = 0;
        js_ws_client_arm_close(ud);
        return;
    }
    if (JS_IsUndefined(ud->on_open))
        return;

    JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
    js_ws_client_call(ud, ud->on_open, 1, &conn_obj, "on_open");
    JS_FreeValue(ud->ctx, conn_obj);
}

/* --vm-thread (cap/vm_thread.h): ws-client onMessage runs on the VM thread. */
static void js_ws_client_on_message_impl(KlWsClientConn *ws, const char *data, size_t len, int is_binary, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    const char *data;
    size_t len;
    int is_binary;
    void *user_data;
} js_ws_client_on_message_vmt_args;
static void js_ws_client_on_message_vmt_job(void *ud)
{
    js_ws_client_on_message_vmt_args *a = (js_ws_client_on_message_vmt_args *)ud;
    js_ws_client_on_message_impl(a->ws, a->data, a->len, a->is_binary, a->user_data);
}
static void js_ws_client_on_message(KlWsClientConn *ws, const char *data, size_t len, int is_binary, void *user_data)
{
    HlVmThread *vmt = ((HlJSWsClientUD *)user_data)->js->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        js_ws_client_on_message_vmt_args a = { ws, data, len, is_binary, user_data };
        (void)hl_vmt_run(vmt, js_ws_client_on_message_vmt_job, &a, NULL, NULL);
        return;
    }
    js_ws_client_on_message_impl(ws, data, len, is_binary, user_data);
}

static void js_ws_client_on_message_impl(KlWsClientConn *ws, const char *data,
                                      size_t len, int is_binary, void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    if (JS_IsUndefined(ud->on_message))
        return;

    JSValue msg_val;
    if (is_binary) {
        msg_val = JS_NewArrayBufferCopy(ud->ctx, (const uint8_t *)data, len);
    } else {
        msg_val = JS_NewStringLen(ud->ctx, data, len);
    }
    /* A message that could not be made (out of memory) is dropped, logged:
     * the exception value went to the callback as its message (audit 26). */
    if (JS_IsException(msg_val)) {
        JS_FreeValue(ud->ctx, JS_GetException(ud->ctx));
        log_error("[hull:ws:client] dropped a %zu-byte message: out of memory",
                  len);
        return;
    }
    JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
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
    /* The Keel client (a timer, not one of Keel's callbacks: safe), and its
     * slot, go now: an app that kept the closed client kept them until the
     * object was collected (audit 32). */
    js_ws_client_free_conn(ud);
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

/* --vm-thread (cap/vm_thread.h): ws-client onClose runs on the VM thread. */
static void js_ws_client_on_close_impl(KlWsClientConn *ws, uint16_t code, const char *reason, size_t reason_len, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    uint16_t code;
    const char *reason;
    size_t reason_len;
    void *user_data;
} js_ws_client_on_close_vmt_args;
static void js_ws_client_on_close_vmt_job(void *ud)
{
    js_ws_client_on_close_vmt_args *a = (js_ws_client_on_close_vmt_args *)ud;
    js_ws_client_on_close_impl(a->ws, a->code, a->reason, a->reason_len, a->user_data);
}
static void js_ws_client_on_close(KlWsClientConn *ws, uint16_t code, const char *reason, size_t reason_len, void *user_data)
{
    HlVmThread *vmt = ((HlJSWsClientUD *)user_data)->js->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        js_ws_client_on_close_vmt_args a = { ws, code, reason, reason_len, user_data };
        (void)hl_vmt_run(vmt, js_ws_client_on_close_vmt_job, &a, NULL, NULL);
        return;
    }
    js_ws_client_on_close_impl(ws, code, reason, reason_len, user_data);
}

static void js_ws_client_on_close_impl(KlWsClientConn *ws, uint16_t code,
                                    const char *reason, size_t reason_len,
                                    void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    js_ws_client_disarm_connect(ud, ud->js->base.async_ctx);
    /* The connection is over before onClose runs (audit 30): a conn.close()
     * or send() from it is a no-op / an error, as on any closed client. It
     * used to arm the close deadline here, after the disarm above, with
     * nothing left to cancel it. */
    ud->closed = 1;

    if (!JS_IsUndefined(ud->on_close)) {
        JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);
        JSValue code_val = JS_NewInt32(ud->ctx, code);
        JSValue reason_val = (reason && reason_len > 0)
                                 ? JS_NewStringLen(ud->ctx, reason, reason_len)
                                 : JS_NULL;
        if (JS_IsException(reason_val)) {   /* audit 26: reason reads as null */
            JS_FreeValue(ud->ctx, JS_GetException(ud->ctx));
            reason_val = JS_NULL;
        }

        JSValue args[3] = { conn_obj, code_val, reason_val };
        js_ws_client_call(ud, ud->on_close, 3, args, "on_close");
        JS_FreeValue(ud->ctx, conn_obj);
        JS_FreeValue(ud->ctx, code_val);
        JS_FreeValue(ud->ctx, reason_val);
    }

    js_ws_client_finish(ud);
}

/* --vm-thread (cap/vm_thread.h): ws-client onError runs on the VM thread. */
static void js_ws_client_on_error_impl(KlWsClientConn *ws, const char *msg, void *user_data);
typedef struct {
    KlWsClientConn *ws;
    const char *msg;
    void *user_data;
} js_ws_client_on_error_vmt_args;
static void js_ws_client_on_error_vmt_job(void *ud)
{
    js_ws_client_on_error_vmt_args *a = (js_ws_client_on_error_vmt_args *)ud;
    js_ws_client_on_error_impl(a->ws, a->msg, a->user_data);
}
static void js_ws_client_on_error(KlWsClientConn *ws, const char *msg, void *user_data)
{
    HlVmThread *vmt = ((HlJSWsClientUD *)user_data)->js->base.vm_thread;
    if (vmt && !hl_vmt_on_vm_thread(vmt)) {
        js_ws_client_on_error_vmt_args a = { ws, msg, user_data };
        (void)hl_vmt_run(vmt, js_ws_client_on_error_vmt_job, &a, NULL, NULL);
        return;
    }
    js_ws_client_on_error_impl(ws, msg, user_data);
}

static void js_ws_client_on_error_impl(KlWsClientConn *ws, const char *msg,
                                    void *user_data)
{
    (void)ws;
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    js_ws_client_disarm_connect(ud, ud->js->base.async_ctx);
    ud->closed = 1;   /* terminal: as in on_close (audit 30) */
    if (JS_IsUndefined(ud->on_error)) {
        log_error("[hull:ws:client] error: %s", msg ? msg : "unknown");
        js_ws_client_finish(ud);
        return;
    }

    JSValue msg_val = JS_NewString(ud->ctx, msg ? msg : "unknown");
    if (JS_IsException(msg_val)) {   /* audit 26: logged, not passed on */
        JS_FreeValue(ud->ctx, JS_GetException(ud->ctx));
        log_error("[hull:ws:client] error: %s", msg ? msg : "unknown");
        js_ws_client_finish(ud);
        return;
    }
    JSValue conn_obj = JS_DupValue(ud->ctx, ud->self_ref);

    JSValue args[2] = { conn_obj, msg_val };
    js_ws_client_call(ud, ud->on_error, 2, args, "on_error callback");
    JS_FreeValue(ud->ctx, conn_obj);
    JS_FreeValue(ud->ctx, msg_val);
    /* Terminal: Keel closed the connection and will not call on_close, so
     * this is where the client is released (it stayed pinned until VM
     * teardown). */
    js_ws_client_finish(ud);
}

/* The connect deadline passed before the upgrade completed: free the Keel
 * client (a timer callback, not one of Keel's: safe) and report it as the
 * connection's terminal error, as Keel reports a failed connect. */
static void js_ws_client_connect_expired(void *user_data)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    ud->connect_timer = 0;   /* fired */
    if (ud->closed || !ud->client) return;
    js_ws_client_free_conn(ud);
    js_ws_client_on_error(NULL, "connect timeout", ud);
}

/* The peer never answered conn.close() (audit 26): free the Keel client (a
 * timer callback, not one of Keel's: safe) and end the connection as Keel
 * would on its close - onClose once, with 1006, then finish. Keel fires no
 * callback for a freed client, and a normal close cancelled this timer. */
static void js_ws_client_close_expired(void *user_data)
{
    HlJSWsClientUD *ud = (HlJSWsClientUD *)user_data;
    ud->connect_timer = 0;   /* fired */
    if (ud->closed || !ud->client) return;
    js_ws_client_free_conn(ud);
    static const char why[] = "close timeout";
    js_ws_client_on_close(NULL, 1006, why, sizeof why - 1, ud);
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
    /* wss:// takes the client TLS config http.fetch uses (audit 24: Keel
     * needs cfg->tls for it and NULL was passed, so every wss:// failed). */
    if (parsed.is_https && !js->base.http_cfg->tls) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "ws.connect: wss:// needs TLS, and this "
                                      "process has no client TLS configuration");
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

    /* A live-client slot (audit 32), after the handlers' getters (app code)
     * and before the Keel client exists. A client the app dropped whose Keel
     * connection was not freed yet goes with a collection, so at the cap
     * collect once and look again. The object owns the slot from here: its
     * finalizer gives it back. */
    /* The connect - DNS, socket, connect, the TLS handshake for wss:// -
     * charged before it as an outbound HTTP request is (audit 40: free, so
     * a loop of refused connects ran on). */
    int gc_tripped = hl_js_budget_charge(ctx, HL_WORK_WS_CONNECT_UNITS) != 0 ||
                     (js->base.ws_clients >= HL_WS_CLIENT_MAX_LIVE &&
                      hl_js_gc_for_retry(ctx) != 0);   /* charged (audit 39) */
    if (gc_tripped || js->base.ws_clients >= HL_WS_CLIENT_MAX_LIVE) {
        JS_FreeCString(ctx, url);
        JS_FreeValue(ctx, ud->self_ref);
        ud->self_ref = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        if (gc_tripped) return JS_EXCEPTION;
        return hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowRangeError(ctx, "ws.connect: too many live WebSocket "
                                  "clients (at most %d per VM)",
                                  HL_WS_CLIENT_MAX_LIVE));
    }
    js->base.ws_clients++;
    ud->slot = &js->base.ws_clients;

    /* Connect */
    KlWsClientCallbacks cbs = {
        .on_open = js_ws_client_on_open,
        .on_message = js_ws_client_on_message,
        .on_close = js_ws_client_on_close,
        .on_error = js_ws_client_on_error,
    };

    /* Keel keeps the tls pointer for the connection's life; the runtime's
     * client TLS config outlives every client (teardown stops them first). */
    KlWsClientConfig wcfg;
    memset(&wcfg, 0, sizeof wcfg);
#ifdef HL_ENABLE_HTTP_CLIENT
    wcfg.tls = parsed.is_https ? js->base.http_cfg->tls : NULL;
#endif
    /* Keel's default frame limit is 16 MiB per client, off the heap:
     * capped, so the live-client cap bounds the total (audit 32). */
    wcfg.max_frame_size = hl_ws_client_max_frame(js->max_heap_bytes);
    KlWsClientConn *client = kl_ws_client_connect(
        &js->server->ev, &js->server->alloc_storage, &wcfg, url, &cbs, ud);

    JS_FreeCString(ctx, url);

    if (!client) {
        /* Connection failed immediately - clean up self_ref */
        hl_ws_client_slot_release(&ud->slot);
        JS_FreeValue(ctx, ud->self_ref);
        ud->self_ref = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "WebSocket connect failed");
    }

    ud->client = client;
    ud->teardown.stop = js_ws_client_teardown_stop;
    hl_async_teardown_link(&js->base.async_live, &ud->teardown);

    /* Connect + handshake deadline (audit 25): the manifest's http timeout,
     * else 30 s, as http.fetch. Without one the connect is not started. */
    {
        int ms = HL_HTTP_DEFAULT_TIMEOUT_MS;
#ifdef HL_ENABLE_HTTP_CLIENT
        ms = hl_http_timeout_resolve(0, js->base.http_cfg->timeout_ms,
                                     HL_HTTP_FETCH_MAX_TIMEOUT_MS);
#endif
        const HlAsyncBackend *be = hl_async_backend();
        ud->connect_timer = be ? be->timer_add(js->base.async_ctx,
                                               (uint64_t)ms,
                                               js_ws_client_connect_expired,
                                               ud)
                               : 0;
        if (ud->connect_timer == 0) {
            JSValue self = ud->self_ref;
            ud->self_ref = JS_UNDEFINED;
            ud->closed = 1;
            js_ws_client_free_conn(ud);      /* not inside a Keel event */
            JS_FreeValue(ctx, self);        /* obj still holds ud */
            JS_FreeValue(ctx, obj);
            return JS_ThrowTypeError(ctx, "WebSocket connect failed");
        }
    }

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
    hl_js_set_function_list(ctx, client_proto, js_ws_client_conn_proto_funcs,
                               sizeof(js_ws_client_conn_proto_funcs) /
                               sizeof(js_ws_client_conn_proto_funcs[0]));
    hl_js_set_class_proto_frozen(ctx, js_ws_client_conn_class_id, client_proto);

    JSModuleDef *m = JS_NewCModule(ctx, "hull:web:ws-client", js_ws_client_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "wsClient");
    return 0;
}
