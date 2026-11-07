/*
 * mod_ssh.c: the private bridge between the JS SSH stdlib and the outbound
 * byte stream (cap/net_stream.c). The JS twin of runtime/lua/mod_ssh.c; read
 * that file's header for the design - the same two gates hold here:
 *
 *   1. The POLICY gate (hl_cap_ssh_open, cap/ssh.c) decides where a
 *      connection may go and as whom, from manifest.ssh, and with a relay
 *      checks BOTH grants. The grants, the relay's TLS and the dial are the
 *      capability's, so this binding cannot decide them differently from the
 *      Lua one.
 *
 *   2. The CALLER gate: every entry point refuses a caller whose script is
 *      not a stdlib module (JS_GetScriptOrModuleName, hl_js_name_is_stdlib -
 *      how mod_db.c decides who may touch _hull_* tables). It is what makes
 *      "the application never holds a socket" true: the module is
 *      underscore-named (`hull:ssh:_stream`, importable only by the stdlib),
 *      and a handle that leaked to app code is inert there.
 *
 * Exposes `connect(opts)`, returning a stream handle with read / write /
 * close / deadline / wait / connectResult. The SSH protocol itself is JS
 * (stdlib/js/hull/ssh/); hull:ssh:_io wraps this handle in the stream shape
 * the protocol layers take.
 *
 * ## Waiting
 *
 * Lua parks the coroutine inside the call (lua_yieldk) and retries the same
 * step on resume. A JS call cannot be resumed, so the same protocol is split
 * at the call boundary: a step that would wait (HL_NET_E_AGAIN) parks on the
 * stream's pending op and returns a Promise that settles, to nothing, once
 * the stream moved; the caller (hull:ssh:_io) then makes the same call
 * again. The retry is safe for the reason the Lua one is: a read consumes
 * nothing when it reports AGAIN and a write is all-or-none. A step that can
 * finish now returns its result as an array, Lua's multiple returns:
 *
 *   read(n)         -> [chunk]  ("" at EOF)  | [null, message, code] | Promise
 *   write(bytes)    -> [true]                | [null, message, code] | Promise
 *   connectResult() -> [true]                | [null, message, code] | Promise
 *   connect(opts)   -> [handle]              | [null, message, code]
 *
 * Bytes are byte strings (one code unit per byte, hull:ssh:bytes) both ways;
 * write also takes an ArrayBuffer / typed array.
 *
 * Every wait goes through the runtime's rules for JS async ops: the async
 * gate (no wait in middleware or under a parked multipart read), no wait
 * inside a transaction (hl_js_db_refuse_wait, both refusals coded
 * wait_refused), a continuation made by hl_js_async_cont_create (the run-tag
 * rules), the request's connection suspended through hl_js_op_suspend, and a
 * detached wait (a timer, a ws callback, app.main, or a second op in a
 * request) tracked on the runtime's teardown list with an on_shutdown.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"        /* get_hl_js, js_get_buffer, HlBufferView */

#ifdef HL_ENABLE_HTTP_CLIENT

#include "internal.h"          /* hl_js_name_is_stdlib, hl_js_async_gate, ... */
#include "db_wait.h"           /* hl_js_db_refuse_wait */
#include "refusal.h"

#include "hull/cap/ssh.h"         /* hl_cap_ssh_open */
#include "hull/cap/net_stream.h"
#include "hull/shared/errno_name.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/shared/refusal.h"  /* HL_REFUSAL_BUSY */
#include "hull/shared/work_charge.h"
#include "hull/net_backend.h"     /* hl_net_op_complete, HlSuspendOp */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern HlAsyncCont *hl_js_async_cont_create(HlJS *js, JSValue resolve,
    JSValue reject, HlAllocator *alloc,
    JSValue (*push_result)(JSContext *, void *));

/* Default connect budget, as the Lua binding's. */
#define SSH_CONNECT_MS_DEF 10000

static JSClassID hull_ssh_stream_class_id;

typedef struct {
    HlNetStream       *s;     /* owned; NULL after close (idempotent)       */
    HlAsyncCtx        *ctx;   /* live only while parked (or a wake pends)   */
    HlAsyncBackendCtx *loop;  /* the loop a pending wake timer is on        */
} HlJsSshStream;

/* ── gates ──────────────────────────────────────────────────────────── */

/* The immediate JS caller (level 1; level 0 is this C function) must be a
 * stdlib module. As runtime/lua/mod_ssh.c's require_stdlib_caller, on EVERY
 * entry point: a handle that leaks to application code must be inert there. */
static int caller_is_stdlib(JSContext *ctx)
{
    JSAtom name = JS_GetScriptOrModuleName(ctx, 1);
    if (name == JS_ATOM_NULL) return 0;
    const char *str = JS_AtomToCString(ctx, name);
    JS_FreeAtom(ctx, name);
    if (!str) return 0;
    int ok = hl_js_name_is_stdlib(str);
    JS_FreeCString(ctx, str);
    return ok;
}

static JSValue refuse_caller(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "ssh: the byte stream is internal to the "
                             "SSH module; use import { ssh } from \"hull:ssh\"");
}

/* The handle behind `this`, or NULL with a pending TypeError. */
static HlJsSshStream *this_stream(JSContext *ctx, JSValueConst this_val)
{
    HlJsSshStream *o = JS_GetOpaque(this_val, hull_ssh_stream_class_id);
    if (!o) JS_ThrowTypeError(ctx, "ssh: not a stream handle");
    return o;
}

/* ── results ────────────────────────────────────────────────────────── */

/* [a, b, c] with up to three slots; NULL strings are left out. */
static JSValue result3(JSContext *ctx, JSValue a, const char *msg, const char *code)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) { JS_FreeValue(ctx, a); return arr; }
    if (JS_DefinePropertyValueUint32(ctx, arr, 0, a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    if (msg) {
        JSValue m = JS_NewString(ctx, msg);
        if (JS_IsException(m) ||
            JS_DefinePropertyValueUint32(ctx, arr, 1, m, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    if (code) {
        JSValue c = JS_NewString(ctx, code);
        if (JS_IsException(c) ||
            JS_DefinePropertyValueUint32(ctx, arr, 2, c, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue push_err(JSContext *ctx, const char *msg)
{
    return result3(ctx, JS_NULL, msg, NULL);
}

/* The short code for a transport failure, as the Lua binding's. */
static const char *net_err_code(int rc)
{
    switch (rc) {
    case HL_NET_E_TIMEOUT:  return "timeout";
    case HL_NET_E_DEADLINE: return "deadline";
    case HL_NET_E_CLOSED:   return "closed";
    case HL_NET_E_LIMIT:    return "too_many_streams";
    case HL_NET_E_RESOLVE:
    case HL_NET_E_CONNECT:
    case HL_NET_E_TLS:      return "connect_failed";
    default:                return "io_error";
    }
}

static JSValue push_net_err(JSContext *ctx, int rc)
{
    return result3(ctx, JS_NULL, hl_net_stream_strerror(rc), net_err_code(rc));
}

/* [null, message, code, errnoName] for a dial that failed with a socket
 * errno, as the Lua binding's push_net_connect_err: the name ("EHOSTUNREACH")
 * becomes the stdlib's `e.errno`, and the message gains it with the C strerror
 * text. No host or port is added - the app knows what it dialled. Without an
 * errno (DNS, TLS) this is push_net_err. */
static JSValue push_net_connect_err(JSContext *ctx, int rc, int en)
{
    if (en == 0) return push_net_err(ctx, rc);
    char nbuf[HL_ERRNO_NAME_MAX];
    const char *name = hl_errno_name(en, nbuf, sizeof nbuf);
    char msg[256];
    snprintf(msg, sizeof msg, "%s: %s (%s)", hl_net_stream_strerror(rc), name,
             strerror(en));
    JSValue arr = result3(ctx, JS_NULL, msg, net_err_code(rc));
    if (JS_IsException(arr)) return arr;
    JSValue nm = JS_NewString(ctx, name);
    if (JS_IsException(nm) ||
        JS_DefinePropertyValueUint32(ctx, arr, 3, nm, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

/* ── byte strings ───────────────────────────────────────────────────── */

/* A byte string from raw bytes: every byte one code unit. JS_NewStringLen
 * takes UTF-8, so the bytes are written as the UTF-8 of U+0000..U+00FF and
 * QuickJS stores the result as an 8-bit string. */
static JSValue new_byte_string(JSContext *ctx, const uint8_t *p, size_t n)
{
    size_t hi = 0;
    for (size_t i = 0; i < n; i++) hi += p[i] >> 7;
    if (!hi) return JS_NewStringLen(ctx, (const char *)p, n);
    char *u = js_malloc(ctx, n + hi);
    if (!u) return JS_EXCEPTION;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c < 0x80) {
            u[k++] = (char)c;
        } else {
            u[k++] = (char)(0xC0 | (c >> 6));
            u[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    JSValue s = JS_NewStringLen(ctx, u, k);
    js_free(ctx, u);
    return s;
}

/* The bytes of a write argument: a byte string (its UTF-8, read back to one
 * byte per code unit; a code unit above 0xFF is refused), or any buffer.
 * *out is js_malloc'd (free with js_free). 0, or -1 with a pending
 * exception. */
static int take_bytes(JSContext *ctx, JSValueConst v, uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    if (JS_IsString(v)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s) return -1;
        uint8_t *b = js_malloc(ctx, n ? n : 1);
        if (!b) { JS_FreeCString(ctx, s); return -1; }
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            uint8_t c = (uint8_t)s[i];
            if (c < 0x80) {
                b[k++] = c;
            } else if ((c == 0xC2 || c == 0xC3) && i + 1 < n &&
                       ((uint8_t)s[i + 1] & 0xC0) == 0x80) {
                b[k++] = (uint8_t)(((c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F));
                i++;
            } else {
                JS_FreeCString(ctx, s);
                js_free(ctx, b);
                JS_ThrowTypeError(ctx, "ssh: a byte string holds one byte per "
                                  "character (0..255)");
                return -1;
            }
        }
        JS_FreeCString(ctx, s);
        *out = b;
        *len = k;
        return 0;
    }
    HlBufferView view = {0};
    const char *str = NULL;
    int needs_free = 0;
    if (!JS_IsObject(v) || !js_get_buffer(ctx, v, &view, &str, &needs_free)) {
        JS_ThrowTypeError(ctx, "ssh: write takes a byte string or a buffer");
        return -1;
    }
    uint8_t *b = js_malloc(ctx, view.len ? view.len : 1);
    if (b && view.len) memcpy(b, view.data, view.len);
    if (needs_free) JS_FreeCString(ctx, str);
    if (!b) return -1;
    *out = b;
    *len = view.len;
    return 0;
}

/* ── parking ────────────────────────────────────────────────────────── */

/* Hand a parked waiter back, once, from the loop: the stream's own op
 * resuming (ssh_on_resume) or the wake a close scheduled (ssh_wake_fire).
 * Never from inside another run (see the Lua twin, audit 18 M3). */
static void ssh_unpark(HlJsSshStream *o)
{
    HlAsyncCtx *ctx = o->ctx;
    if (!ctx) return;
    o->ctx = NULL;                 /* one resume per park */
    ctx->detached_timer = 0;
    ctx->driver = NULL;            /* the continuation pushes nothing */
    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
}

/* The deferred half of a close that had a waiter parked on the stream. */
static void ssh_wake_fire(void *user_data)
{
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlJsSshStream *o = (HlJsSshStream *)ctx->driver;
    ctx->detached_timer = 0;
    if (o && o->ctx == ctx) ssh_unpark(o);
}

static void ssh_cancel_wake(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop)
{
    if (ctx->detached_timer > 0 && loop)
        hl_async_backend()->timer_cancel(loop, (uint64_t)ctx->detached_timer);
    ctx->detached_timer = 0;
}

/* The loop finished what the stream was waiting on. */
static void ssh_on_resume(HlAsyncOp *op)
{
    HlNetStream *s = hl_net_stream_from_op(op);
    HlJsSshStream *o = s ? hl_net_stream_user(s) : NULL;
    if (!o) return;
    ssh_unpark(o);
}

/* The request a waiter was attached to is going away (Keel cancels the op;
 * hl_async_ctx_cancel frees the ctx). The stream stops pointing at the ctx
 * and is closed: the waiter was stopped part-way through an SSH exchange, so
 * nothing can vouch for the connection's protocol state (as in Lua). */
static void ssh_on_request_cancel(KlAsyncOp *kop, void *user_data)
{
    (void)kop;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlJsSshStream *o = (HlJsSshStream *)ctx->driver;
    if (o && o->ctx == ctx) {
        o->ctx = NULL;
        ssh_cancel_wake(ctx, o->loop);
        if (o->s) hl_net_stream_close(o->s);
    }
    ctx->driver = NULL;
    hl_async_ctx_cancel(ctx);
}

/* A detached wait still pending at teardown (HlAsyncCtx.on_shutdown):
 * the same as a cancelled request, before hl_async_ctx_cancel releases the
 * continuation. */
static void ssh_on_shutdown(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop)
{
    HlJsSshStream *o = (HlJsSshStream *)ctx->driver;
    if (o && o->ctx == ctx) {
        o->ctx = NULL;
        ssh_cancel_wake(ctx, loop);
        if (o->s) hl_net_stream_close(o->s);
    }
    ctx->driver = NULL;
}

/* Park on the stream's pending op and return the Promise the caller waits
 * on; JS_EXCEPTION with a pending exception when the runtime refuses the
 * wait (both refusals are coded wait_refused); a [null, message] result when
 * the park could not be armed (nothing would ever settle it). One park per
 * stream: the entry points refuse a second waiter first (ssh_busy). */
static JSValue ssh_park(JSContext *ctx, HlJsSshStream *o, const char *what,
                        const char *cannot)
{
    HlJS *js = get_hl_js(ctx);
    if (hl_js_async_gate(ctx, js, what) != 0) return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, what)) return JS_EXCEPTION;
    if (!js || !js->base.async_ctx || o->ctx) return push_err(ctx, cannot);

    HlAsyncOp *op = hl_net_stream_pending_op(o->s);
    if (!op) return push_err(ctx, cannot);   /* resuming would hang */
    const HlAsyncBackend *be = hl_async_backend();
    if (!be || !be->op_suspend || !be->op_cancel) return push_err(ctx, cannot);

    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx,
                                           js->base.alloc);
    if (!actx) return push_err(ctx, cannot);

    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) {
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }
    /* The continuation pushes nothing: the caller makes the same call again
     * once the promise settles, so the result comes from that retry. */
    HlAsyncCont *cont = hl_js_async_cont_create(js, rf[0], rf[1],
                                                js->base.alloc, NULL);
    if (!cont) {
        JS_FreeValue(ctx, rf[0]);
        JS_FreeValue(ctx, rf[1]);
        JS_FreeValue(ctx, promise);
        hl_async_ctx_free(actx);
        return push_err(ctx, cannot);
    }
    actx->cont = cont;
    actx->driver = o;                      /* not owned; see the finalizer */
    actx->op.on_cancel = ssh_on_request_cancel;
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);

    hl_net_stream_set_user(o->s, o);
    op->on_resume = ssh_on_resume;
    op->on_cancel = ssh_on_resume;         /* a cancel must still un-park */

    /* The stream's op with the async backend FIRST (silently retractable),
     * the request's connection LAST (Keel owns an op once suspended), as in
     * the Lua binding. */
    if (be->op_suspend(js->base.async_ctx, op) != 0) {
        actx->cont = NULL;
        cont->destroy(cont);               /* frees resolve / reject */
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return push_err(ctx, cannot);
    }
    if (!actx->detached && hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
        be->op_cancel(js->base.async_ctx, op);
        actx->cont = NULL;
        cont->destroy(cont);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return push_err(ctx, cannot);
    }

    o->ctx = actx;
    o->loop = js->base.async_ctx;
    if (actx->detached) {
        actx->on_shutdown = ssh_on_shutdown;
        hl_async_ctx_track(actx, &js->base.async_live);
    }
    return promise;
}

/* Whether `d` is an integer in [lo, hi]. The range is tested first: casting a
 * double outside int64_t's range (1e300, an infinity, NaN) to it is undefined
 * behaviour, which the old `d != (double)(int64_t)d` test hit first. */
static int ssh_int_in(double d, double lo, double hi)
{
    return d >= lo && d <= hi && d == (double)(int64_t)d;
}

/* A second waiter on a stream that already has one parked: refused before
 * anything is touched, coded "busy" (audit 36), as in Lua. NULL when free. */
static int ssh_busy(const HlJsSshStream *o)
{
    return o->ctx && o->s;
}

static JSValue busy_result(JSContext *ctx)
{
    return result3(ctx, JS_NULL,
                   "busy: another caller is waiting on this connection",
                   HL_REFUSAL_BUSY);
}

/* ── read / write ───────────────────────────────────────────────────── */

static JSValue js_ssh_read(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (!caller_is_stdlib(ctx)) return refuse_caller(ctx);
    HlJsSshStream *o = this_stream(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    if (ssh_busy(o)) return busy_result(ctx);

    double want = 4096;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (!JS_IsNumber(argv[0]))
            return JS_ThrowTypeError(ctx, "ssh: read size must be an integer");
        if (JS_ToFloat64(ctx, &want, argv[0])) return JS_EXCEPTION;
        if (!ssh_int_in(want, -9007199254740991.0, 9007199254740991.0))
            return JS_ThrowTypeError(ctx, "ssh: read size must be an integer");
    }
    if (!(want > 0)) return push_err(ctx, "read size must be positive");
    /* Sized before a byte arrives: never larger than the stream's read cap,
     * so a size taken from a peer's length field is no allocation. */
    if (want > (double)HL_NET_READ_CAP_DEFAULT) want = HL_NET_READ_CAP_DEFAULT;

    if (!o->s) return push_net_err(ctx, HL_NET_E_CLOSED);
    size_t n_want = (size_t)want;
    uint8_t *buf = js_malloc(ctx, n_want);
    if (!buf) return JS_EXCEPTION;
    long n = hl_net_stream_read(o->s, buf, n_want);
    if (n >= 0) {
        /* The byte-string conversion is this binding's own work. */
        if (hl_js_budget_charge(ctx, hl_work_bytes_units((size_t)n))) {
            js_free(ctx, buf);
            return JS_EXCEPTION;
        }
        JSValue s = new_byte_string(ctx, buf, (size_t)n);   /* "" = EOF */
        js_free(ctx, buf);
        if (JS_IsException(s)) return s;
        return result3(ctx, s, NULL, NULL);
    }
    js_free(ctx, buf);
    if (n != HL_NET_E_AGAIN) return push_net_err(ctx, (int)n);
    return ssh_park(ctx, o, "ssh read", "cannot wait for data");
}

static JSValue js_ssh_write(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (!caller_is_stdlib(ctx)) return refuse_caller(ctx);
    HlJsSshStream *o = this_stream(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    if (ssh_busy(o)) return busy_result(ctx);
    if (argc < 1) return JS_ThrowTypeError(ctx, "ssh: write requires (bytes)");

    uint8_t *data = NULL;
    size_t len = 0;
    if (take_bytes(ctx, argv[0], &data, &len) != 0) return JS_EXCEPTION;
    if (!len) { js_free(ctx, data); return result3(ctx, JS_TRUE, NULL, NULL); }
    if (hl_js_budget_charge(ctx, hl_work_bytes_units(len))) {
        js_free(ctx, data);
        return JS_EXCEPTION;
    }
    /* Resolved again after the conversion (app code can run in it). */
    o = this_stream(ctx, this_val);
    if (!o || !o->s) {
        js_free(ctx, data);
        return o ? push_net_err(ctx, HL_NET_E_CLOSED) : JS_EXCEPTION;
    }
    if (ssh_busy(o)) { js_free(ctx, data); return busy_result(ctx); }
    int rc = hl_net_stream_write(o->s, data, len);
    js_free(ctx, data);
    if (rc == HL_NET_OK) return result3(ctx, JS_TRUE, NULL, NULL);
    if (rc != HL_NET_E_AGAIN) return push_net_err(ctx, rc);
    return ssh_park(ctx, o, "ssh write", "cannot wait for send capacity");
}

/* ── close / bounds ─────────────────────────────────────────────────── */

static JSValue js_ssh_close(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (!caller_is_stdlib(ctx)) return refuse_caller(ctx);
    HlJsSshStream *o = this_stream(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    if (o->s) {
        /* A waiter parked on this stream is handed back from the loop, never
         * from inside this caller's run (audit 18 M3); scheduled before
         * anything is touched, so a failure leaves the stream as it was. */
        if (o->ctx && o->ctx->detached_timer == 0) {
            uint64_t tid = o->loop
                ? hl_async_backend()->timer_add(o->loop, 0, ssh_wake_fire, o->ctx)
                : 0;
            if (tid == 0)
                return JS_ThrowInternalError(ctx, "ssh: failed to schedule the close");
            o->ctx->detached_timer = (int64_t)tid;
        }
        /* The waiter's retry finds the stream gone: "connection closed". */
        hl_net_stream_close(o->s);
        hl_net_stream_set_user(o->s, NULL);
        hl_net_stream_free(o->s);
        o->s = NULL;
    }
    return JS_UNDEFINED;
}

/* deadline(ms) / wait(ms): the two bounds in cap/net_stream.h. Neither ends
 * the stream; an expiry comes back from the read or write it interrupted as
 * [null, message, "deadline" | "timeout"]. 0 clears. */
static int bound_arg(JSContext *ctx, int argc, JSValueConst *argv, int *ms)
{
    double v = 0;
    if (argc < 1 || !JS_IsNumber(argv[0]) || JS_ToFloat64(ctx, &v, argv[0]) ||
        !ssh_int_in(v, 0, 24.0 * 3600 * 1000)) {
        if (!JS_HasException(ctx))
            JS_ThrowRangeError(ctx, "ssh: a wait bound must be 0..86400000 ms");
        return -1;
    }
    *ms = (int)v;
    return 0;
}

static JSValue js_ssh_deadline(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (!caller_is_stdlib(ctx)) return refuse_caller(ctx);
    HlJsSshStream *o = this_stream(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    int ms = 0;
    if (bound_arg(ctx, argc, argv, &ms)) return JS_EXCEPTION;
    if (o->s) hl_net_stream_deadline(o->s, ms);
    return JS_UNDEFINED;
}

static JSValue js_ssh_wait(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (!caller_is_stdlib(ctx)) return refuse_caller(ctx);
    HlJsSshStream *o = this_stream(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    int ms = 0;
    if (bound_arg(ctx, argc, argv, &ms)) return JS_EXCEPTION;
    if (o->s) hl_net_stream_wait(o->s, ms);
    return JS_UNDEFINED;
}

/* ── connect ────────────────────────────────────────────────────────── */

/* connectResult(): the dial's outcome, or a Promise to wait on first. A
 * terminal failure releases the stream at once (the caller is getting an
 * error, not a connection), as in Lua. */
static JSValue js_ssh_connect_result(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (!caller_is_stdlib(ctx)) return refuse_caller(ctx);
    HlJsSshStream *o = this_stream(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    if (ssh_busy(o)) return busy_result(ctx);
    if (!o->s) return push_net_err(ctx, HL_NET_E_CLOSED);
    int rc = hl_net_stream_connect_result(o->s);
    if (rc == HL_NET_OK) return result3(ctx, JS_TRUE, NULL, NULL);
    if (rc == HL_NET_E_AGAIN)
        return ssh_park(ctx, o, "ssh connect", "cannot wait for the connection");
    int en = hl_net_stream_connect_errno(o->s);
    hl_net_stream_set_user(o->s, NULL);
    hl_net_stream_free(o->s);
    o->s = NULL;
    return push_net_connect_err(ctx, rc, en);
}

/* An optional integer option, within [lo, hi]: the value, def when absent
 * (undefined / null), or -1 when present and not an integer in range. A JS
 * number only (audit 39, as Lua's integer-only rule): a string "22e2" was the
 * port the Lua grant had been checked against. -2 with a pending exception
 * when reading it ran app code that threw. */
static long opt_int_in(JSContext *ctx, JSValueConst obj, const char *name,
                       long def, long lo, long hi)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsException(v)) return -2;
    long out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        double d = 0;
        if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) ||
            !ssh_int_in(d, (double)lo, (double)hi))
            out = -1;
        else
            out = (long)d;
    }
    JS_FreeValue(ctx, v);
    return out;
}

/* An optional string option as a C string (JS_FreeCString it), NULL when
 * absent or not a string - a non-string is no name at all, which the policy
 * refuses (Lua's lua_tostring of a table is NULL too). -1 with a pending
 * exception when the read threw. */
static int opt_str(JSContext *ctx, JSValueConst obj, const char *name,
                   const char **out)
{
    *out = NULL;
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsException(v)) return -1;
    if (JS_IsString(v)) {
        *out = JS_ToCString(ctx, v);
        if (!*out) { JS_FreeValue(ctx, v); return -1; }
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static void js_ssh_stream_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJsSshStream *o = JS_GetOpaque(val, hull_ssh_stream_class_id);
    if (!o) return;
    /* While a waiter is parked, its continuation holds the promise chain
     * that holds this handle, so this runs then only as the runtime itself
     * goes; the ctx's owner (Keel, or the teardown list) still cancels it,
     * and must not reach this struct through driver. */
    if (o->ctx) o->ctx->driver = NULL;
    if (o->s) {
        hl_net_stream_set_user(o->s, NULL);
        hl_net_stream_close(o->s);
        hl_net_stream_free(o->s);
        o->s = NULL;
    }
    free(o);
}

static const JSClassDef js_ssh_stream_class = {
    "hull.ssh.stream",
    .finalizer = js_ssh_stream_finalizer,
};

/*
 * connect({ host, port, user, timeoutMs,
 *           via: { host, port, tls, tlsHostname } }) -> [handle] | [null, message, code]
 *
 * `host` / `port` / `user` always name the SSH DESTINATION; `via` names the
 * relay a socket is actually opened to. The caller gate first, so an
 * application probing this bridge learns nothing about the grant.
 */
static JSValue js_ssh_connect(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!caller_is_stdlib(ctx)) return refuse_caller(ctx);
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "ssh: connect requires an options object");
    JSValueConst opts = argv[0];

    const char *host = NULL, *user = NULL, *via_host = NULL, *via_sni = NULL;
    JSValue via = JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    HlNetStream *s = NULL;

    long port = opt_int_in(ctx, opts, "port", 22, 1, 65535);
    long timeout = port == -2 ? -2
        : opt_int_in(ctx, opts, "timeoutMs", SSH_CONNECT_MS_DEF, 1, 24L * 3600 * 1000);
    if (port == -2 || timeout == -2) goto out;
    if (port < 0 || timeout < 0) {
        ret = push_err(ctx, port < 0 ? "ssh: port must be an integer 1..65535"
                                     : "ssh: timeoutMs must be an integer 1..86400000");
        goto out;
    }
    /* The dial's own deadline is clamped (audit 35); the stdlib's handshake
     * deadline keeps the caller's full value. */
    if (timeout > HL_SSH_CONNECT_MS_MAX) timeout = HL_SSH_CONNECT_MS_MAX;
    if (opt_str(ctx, opts, "host", &host) || opt_str(ctx, opts, "user", &user))
        goto out;

    int via_port = 0, via_tls = 0, has_via = 0;
    via = JS_GetPropertyStr(ctx, opts, "via");
    if (JS_IsException(via)) goto out;
    if (!JS_IsUndefined(via) && !JS_IsNull(via)) {
        /* A `via` that is present but not an object is refused, never read
         * as "connect directly": the caller's credentials may live in that
         * relay's headers (as in Lua). */
        if (!JS_IsObject(via)) {
            ret = push_err(ctx, "ssh: via must be an object");
            goto out;
        }
        has_via = 1;
        if (opt_str(ctx, via, "host", &via_host)) goto out;
        long vp = opt_int_in(ctx, via, "port", 443, 1, 65535);
        if (vp == -2) goto out;
        if (vp < 0) {
            ret = push_err(ctx, "ssh: via.port must be an integer 1..65535");
            goto out;
        }
        via_port = (int)vp;
        JSValue t = JS_GetPropertyStr(ctx, via, "tls");
        if (JS_IsException(t)) goto out;
        via_tls = JS_ToBool(ctx, t);
        JS_FreeValue(ctx, t);
        if (opt_str(ctx, via, "tlsHostname", &via_sni)) goto out;
    }

    HlJS *js = get_hl_js(ctx);
    if (!js) { ret = push_err(ctx, "no runtime"); goto out; }

    HlSshOpen req = {
        .host = host, .port = (int)port, .user = user,
        .connect_ms = (int)timeout,
        .via = has_via, .via_host = via_host, .via_port = via_port,
        .via_tls = via_tls, .via_tls_hostname = via_sni,
    };
    HlSshEnv env = {
        .policy     = js->base.ssh_policy,
        .client_tls = js->base.client_tls,
        .alloc      = js->base.alloc,
        .async      = js->base.async_ctx,
        .pool       = js->base.thread_pool,
    };
    const char *why = NULL;
    int rc = hl_cap_ssh_open(&env, &req, &s, &why);
    if (rc == HL_NET_E_LIMIT) {
        /* Live streams are capped (HL_SSH_MAX_LIVE_STREAMS, audit 35), and a
         * stream dropped without close() holds its slot until collected:
         * collect once (charged, audit 39) and retry, as in Lua. The option
         * strings are C copies, untouched by the collection. */
        if (hl_js_gc_for_retry(ctx)) goto out;
        why = NULL;
        rc = hl_cap_ssh_open(&env, &req, &s, &why);
    }
    if (!s) {
        if (rc == HL_NET_E_LIMIT)
            ret = result3(ctx, JS_NULL, why ? why : hl_net_stream_strerror(rc),
                          "too_many_streams");
        else if (rc == HL_NET_E_DENIED)
            ret = result3(ctx, JS_NULL, why ? why : "denied", "denied");
        else if (why)
            ret = push_err(ctx, why);
        else
            ret = push_net_err(ctx, rc);
        goto out;
    }

    {
        HlJsSshStream *o = calloc(1, sizeof *o);
        JSValue h = o ? JS_NewObjectClass(ctx, (int)hull_ssh_stream_class_id)
                      : JS_EXCEPTION;
        if (JS_IsException(h)) {
            free(o);
            hl_net_stream_close(s);
            hl_net_stream_free(s);
            s = NULL;
            if (!o) JS_ThrowOutOfMemory(ctx);
            goto out;
        }
        o->s = s;
        s = NULL;
        hl_net_stream_set_user(o->s, o);
        JS_SetOpaque(h, o);
        ret = result3(ctx, h, NULL, NULL);
    }

out:
    if (host) JS_FreeCString(ctx, host);
    if (user) JS_FreeCString(ctx, user);
    if (via_host) JS_FreeCString(ctx, via_host);
    if (via_sni) JS_FreeCString(ctx, via_sni);
    JS_FreeValue(ctx, via);
    return ret;
}

/* ── registration ───────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_ssh_stream_methods[] = {
    JS_CFUNC_DEF("read", 1, js_ssh_read),
    JS_CFUNC_DEF("write", 1, js_ssh_write),
    JS_CFUNC_DEF("close", 0, js_ssh_close),
    JS_CFUNC_DEF("deadline", 1, js_ssh_deadline),
    JS_CFUNC_DEF("wait", 1, js_ssh_wait),
    JS_CFUNC_DEF("connectResult", 0, js_ssh_connect_result),
};

static int js_ssh_module_init(JSContext *ctx, JSModuleDef *m)
{
    /* Registered per runtime, the id once per process (as mod_kv.c). */
    JS_NewClassID(&hull_ssh_stream_class_id);
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), hull_ssh_stream_class_id)) {
        JS_NewClass(JS_GetRuntime(ctx), hull_ssh_stream_class_id,
                    &js_ssh_stream_class);
        JSValue proto = JS_NewObject(ctx);
        hl_js_set_function_list(ctx, proto, js_ssh_stream_methods,
            (int)(sizeof js_ssh_stream_methods / sizeof js_ssh_stream_methods[0]));
        hl_js_set_class_proto_frozen(ctx, hull_ssh_stream_class_id, proto);
    }
    hl_js_export_frozen(ctx, m, "connect",
                        JS_NewCFunction(ctx, js_ssh_connect, "connect", 1));
    return 0;
}

int hl_js_init_ssh_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:ssh:_stream", js_ssh_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "connect");
    return 0;
}

#else  /* !HL_ENABLE_HTTP_CLIENT */

/* Nothing to bind on a pure-compute build (cap/net_stream.c is filtered out;
 * the resolver refuses hull/ssh@1 there). The object is still produced: the
 * http-js feature archive names it. */
typedef int hl_js_mod_ssh_unused;

#endif /* HL_ENABLE_HTTP_CLIENT */
