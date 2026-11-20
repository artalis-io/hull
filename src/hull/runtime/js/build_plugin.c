/*
 * build_plugin.c - `app.transform` for a JavaScript build plugin
 * (docs/buildcontext_design.md §9.1, include/hull/shared/build_plugin.h).
 *
 * The twin of runtime/lua/build_plugin.c: the decoration evaluates the
 * stdlib's hull/_build_plugin.js (a function of the app and the channel)
 * and calls it; the loop and the ctx API are JavaScript. The channel calls
 * refuse a caller that is not that stdlib script.
 *
 *   chan.next()          -> the next action, or null when the host is done;
 *                           app.main's own run only, and a new run (budget
 *                           and deadline) each time
 *   chan.call(op, args)  -> the host's reply: { ok } or { error }
 *   chan.done(err?)      -> the transform ended (err: its message)
 *   chan.bytes(s)        -> a string's UTF-8 bytes (a write is bytes)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "plain_value.h"

#include "hull/cap/run_watchdog.h"
#include "hull/shared/build_plugin.h"
#include "hull/shared/dispatch_meter.h"
#include "hull/shared/term_escape.h"   /* hl_utf8_valid */
#include "hull/shared/work_charge.h"
#include "hull/vfs.h"

#include <stdlib.h>
#include <string.h>

#define BP_SCRIPT "hull:_build_plugin"

static uint32_t g_call_id;

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

static JSValue gate(JSContext *ctx)
{
    if (!caller_is_stdlib(ctx))
        return JS_ThrowTypeError(ctx, "build plugin: the channel is internal "
                                 "to app.transform");
    if (hl_build_plugin_fd() < 0)
        return JS_ThrowTypeError(ctx, "build plugin: this process has no "
                                 "channel to a build host (run it through "
                                 "`hull build`)");
    return JS_UNDEFINED;
}

static int recv_frame(HlChanFrame *f, uint64_t deadline)
{
    uint64_t tok = hl_meter_exclude_begin();
    int rc = hl_build_plugin_recv(f, deadline);
    hl_meter_exclude_end(tok);
    return rc;
}

static JSValue decoded(JSContext *ctx, HlChanFrame *f, const char *what)
{
    int err = 0;
    JSValue v = hl_js_pv_decode(ctx, f->buf, f->len, &err);
    size_t len = f->len;
    hl_chan_frame_reset(f);
    if (err == HL_PV_E_NOMEM) return JS_EXCEPTION;
    if (err != HL_PV_OK) {
        hl_build_plugin_end();
        return JS_ThrowTypeError(ctx, "build plugin: the host sent a %s %s",
                                 hl_pv_error_name(err), what);
    }
    if (hl_js_budget_charge(ctx, len / 8) != 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    return v;
}

static JSValue chan_next(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue g = gate(ctx);
    if (JS_IsException(g)) return g;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->active_cli_main)
        return JS_ThrowTypeError(ctx, "build plugin: actions are read by "
                                 "app.main's run");
    HlChanFrame f;
    int rc = recv_frame(&f, 0);
    if (rc != HL_CHAN_DONE || f.type == HL_BP_STOP) {
        int stop = rc == HL_CHAN_DONE || rc == HL_CHAN_EOF;
        hl_chan_frame_reset(&f);
        if (stop) return JS_NULL;
        hl_build_plugin_end();
        return JS_ThrowTypeError(ctx, "build plugin: the channel to the host broke");
    }
    if (f.type != HL_BP_ACTION) {
        hl_chan_frame_reset(&f);
        hl_build_plugin_end();
        return JS_ThrowTypeError(ctx, "build plugin: the host sent a frame out of turn");
    }
    /* An action is a run of its own, as a tui.pollSync event is. */
    hl_js_budget_arm_kind(js, HL_RUN_MAIN);
    return decoded(ctx, &f, "action");
}

static JSValue chan_call(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv)
{
    (void)this_val;
    JSValue g = gate(ctx);
    if (JS_IsException(g)) return g;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "build plugin: an operation name");
    if (hl_js_budget_charge(ctx, HL_WORK_PLUGIN_CALL_UNITS) != 0)
        return JS_EXCEPTION;
    int64_t left = hl_run_deadline_left_ms();
    if (left == 0)
        return JS_ThrowTypeError(ctx, "build plugin: the run's time is up");
    uint64_t ms = (left < 0 || left > (int64_t)HL_BP_REPLY_TIMEOUT_MS)
                      ? HL_BP_REPLY_TIMEOUT_MS : (uint64_t)left;
    uint64_t deadline = hl_chan_mono_ms() + ms;

    size_t oplen = 0;
    const char *op = JS_ToCStringLen(ctx, &oplen, argv[0]);
    if (!op) return JS_EXCEPTION;
    HlPvWriter w;
    hl_pv_writer_init(&w, HL_BP_MAX_PAYLOAD);
    hl_pv_put_map(&w, 2);
    hl_pv_put_key(&w, "args", 4);
    w.depth = 1;   /* inside the envelope, as the host's check counts it */
    hl_js_pv_encode(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &w);
    w.depth = 0;
    hl_pv_put_key(&w, "op", 2);
    hl_pv_put_text(&w, op, oplen);
    if (w.err != HL_PV_OK) {
        int e = w.err;
        hl_pv_writer_free(&w);
        JSValue ex = JS_ThrowTypeError(ctx, "build plugin: %s: arguments are %s",
                                       op, hl_pv_error_name(e));
        JS_FreeCString(ctx, op);
        return ex;
    }
    uint32_t id = ++g_call_id;
    size_t sent = w.len;
    int rc = hl_build_plugin_send(HL_BP_CALL, id, w.buf, w.len, deadline);
    hl_pv_writer_free(&w);
    if (rc != 0 || hl_js_budget_charge(ctx, sent / 8) != 0) {
        JS_FreeCString(ctx, op);
        return rc != 0 ? JS_ThrowTypeError(ctx, "build plugin: the channel to "
                                           "the host broke")
                       : JS_EXCEPTION;
    }
    HlChanFrame f;
    rc = recv_frame(&f, deadline);
    if (rc != HL_CHAN_DONE || f.type != HL_BP_REPLY || f.id != id) {
        hl_chan_frame_reset(&f);
        hl_build_plugin_end();
        JSValue ex = JS_ThrowTypeError(ctx, rc == HL_CHAN_MORE
                                           ? "build plugin: %s: the host did not answer"
                                           : "build plugin: %s: the channel to the host broke",
                                       op);
        JS_FreeCString(ctx, op);
        return ex;
    }
    JS_FreeCString(ctx, op);
    return decoded(ctx, &f, "reply");
}

static JSValue chan_done(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv)
{
    (void)this_val;
    JSValue g = gate(ctx);
    if (JS_IsException(g)) return g;
    const char *msg = NULL;
    size_t n = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        msg = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!msg) return JS_EXCEPTION;
    }
    HlPvWriter w;
    hl_pv_writer_init(&w, 64 * 1024);
    hl_build_plugin_put_done(&w, msg, n);   /* bytes: a lone surrogate is not text */
    if (msg) JS_FreeCString(ctx, msg);
    int rc = w.err ? -1 : hl_build_plugin_send(HL_BP_DONE, 0, w.buf, w.len,
                                              hl_chan_mono_ms() + HL_BP_REPLY_TIMEOUT_MS);
    hl_pv_writer_free(&w);
    return JS_NewBool(ctx, rc == 0);
}

/* A string's UTF-8 bytes, as an ArrayBuffer (a write is bytes; a string
 * would cross as text). A lone surrogate has no UTF-8 form: refused. */
static JSValue chan_bytes(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    (void)this_val;
    if (!caller_is_stdlib(ctx))
        return JS_ThrowTypeError(ctx, "build plugin: the channel is internal "
                                 "to app.transform");
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "build plugin: bytes from a string");
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    if (hl_js_budget_charge(ctx, n / 8) != 0) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JSValue r = hl_utf8_valid((const unsigned char *)s, n)
                    ? JS_NewArrayBufferCopy(ctx, (const uint8_t *)s, n)
                    : JS_ThrowTypeError(ctx, "build plugin: a string with a "
                                        "lone surrogate is not text");
    JS_FreeCString(ctx, s);
    return r;
}

static const JSCFunctionListEntry chan_funcs[] = {
    JS_CFUNC_DEF("next", 0, chan_next),
    JS_CFUNC_DEF("call", 2, chan_call),
    JS_CFUNC_DEF("done", 1, chan_done),
    JS_CFUNC_DEF("bytes", 1, chan_bytes),
};

/* Evaluate the stdlib script and call it with the app and the channel. 0, or
 * -1 with an exception pending (the manifest call fails). */
#define BP_STUB_KEY "__hull_transform_stub"

int hl_js_install_app_transform(JSContext *ctx, JSValueConst app)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    const HlEntry *e = (js && js->base.platform_vfs)
                           ? hl_vfs_find(js->base.platform_vfs, BP_SCRIPT) : NULL;
    if (!e) {
        JS_ThrowInternalError(ctx, "build plugin: %s is not embedded", BP_SCRIPT);
        return -1;
    }
    char *src = malloc((size_t)e->len + 1);   /* the lexer wants a NUL */
    if (!src) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(src, e->data, e->len);
    src[e->len] = '\0';
    JSValue init = JS_Eval(ctx, src, e->len, BP_SCRIPT, JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(init)) return -1;
    if (!JS_IsFunction(ctx, init)) {
        JS_FreeValue(ctx, init);
        JS_ThrowInternalError(ctx, "build plugin: %s is not a function", BP_SCRIPT);
        return -1;
    }
    JSValue chan = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(chan) ||
        hl_js_set_function_list(ctx, chan, chan_funcs,
                                (int)(sizeof chan_funcs / sizeof chan_funcs[0])) != 0 ||
        hl_js_freeze_value(ctx, chan) != 0) {
        JS_FreeValue(ctx, chan);
        JS_FreeValue(ctx, init);
        return -1;
    }
    JSValueConst args[2] = { app, chan };
    JSValue r = JS_Call(ctx, init, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, chan);
    JS_FreeValue(ctx, init);
    if (JS_IsException(r)) return -1;
    /* The stub runner test.runTransform calls: kept in the runtime's internal
     * store, which app code cannot reach. */
    JSValue store = hl_js_internals(ctx);
    int rc = JS_SetPropertyStr(ctx, store, BP_STUB_KEY, r) < 0 ? -1 : 0;   /* takes r */
    JS_FreeValue(ctx, store);
    return rc;
}

JSValue hl_js_transform_stub(JSContext *ctx)
{
    JSValue store = hl_js_internals(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, store, BP_STUB_KEY);
    JS_FreeValue(ctx, store);
    return fn;
}
