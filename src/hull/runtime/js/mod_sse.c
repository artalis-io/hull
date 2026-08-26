/*
 * mod_sse.c - SSE stream JS class (QuickJS)
 *
 * Provides:
 *   - stream.event(name, data, id?)
 *   - stream.comment(text)
 *   - stream.close()
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

#include <keel/sse.h>

#include <string.h>

/* ── JS class ID ───────────────────────────────────────────────────── */

JSClassID js_sse_stream_class_id;

/* ── SSE stream opaque ─────────────────────────────────────────────── */

typedef struct {
    KlSse    sse;
    int      closed;
} HlJSSseStreamUD;

/* ── Methods ───────────────────────────────────────────────────────── */

static JSValue js_sse_event(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    HlJSSseStreamUD *ud = (HlJSSseStreamUD *)JS_GetOpaque2(ctx, this_val,
                                                              js_sse_stream_class_id);
    if (!ud || ud->closed)
        return JS_ThrowTypeError(ctx, "SSE stream is closed");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "stream.event requires (name, data)");

    const char *event_name = NULL;
    if (!JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        event_name = JS_ToCString(ctx, argv[0]);

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[1]);
    if (!data) {
        if (event_name) JS_FreeCString(ctx, event_name);
        return JS_EXCEPTION;
    }

    const char *id = NULL;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
        id = JS_ToCString(ctx, argv[2]);

    int rc = kl_sse_event(&ud->sse, event_name, data, data_len, id);

    if (event_name) JS_FreeCString(ctx, event_name);
    JS_FreeCString(ctx, data);
    if (id) JS_FreeCString(ctx, id);

    if (rc < 0)
        return JS_ThrowTypeError(ctx, "SSE write failed");
    return JS_UNDEFINED;
}

static JSValue js_sse_comment(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)argc;
    HlJSSseStreamUD *ud = (HlJSSseStreamUD *)JS_GetOpaque2(ctx, this_val,
                                                              js_sse_stream_class_id);
    if (!ud || ud->closed)
        return JS_ThrowTypeError(ctx, "SSE stream is closed");

    size_t len;
    const char *text = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!text) return JS_EXCEPTION;

    int rc = kl_sse_comment(&ud->sse, text, len);
    JS_FreeCString(ctx, text);

    if (rc < 0)
        return JS_ThrowTypeError(ctx, "SSE write failed");
    return JS_UNDEFINED;
}

static JSValue js_sse_close(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJSSseStreamUD *ud = (HlJSSseStreamUD *)JS_GetOpaque2(ctx, this_val,
                                                              js_sse_stream_class_id);
    if (!ud || ud->closed)
        return JS_UNDEFINED;

    kl_sse_end(&ud->sse);
    ud->closed = 1;
    return JS_UNDEFINED;
}

/* ── Class definition ──────────────────────────────────────────────── */

static void js_sse_stream_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJSSseStreamUD *ud = (HlJSSseStreamUD *)JS_GetOpaque(val,
                                                             js_sse_stream_class_id);
    if (ud)
        js_free_rt(rt, ud);
}

static JSClassDef js_sse_stream_class = {
    "SseStream",
    .finalizer = js_sse_stream_finalizer,
};

static const JSCFunctionListEntry js_sse_stream_proto_funcs[] = {
    JS_CFUNC_DEF("event", 2, js_sse_event),
    JS_CFUNC_DEF("comment", 1, js_sse_comment),
    JS_CFUNC_DEF("close", 0, js_sse_close),
};

/* ── Registration + factory ────────────────────────────────────────── */

void hl_js_sse_register_class(JSContext *ctx)
{
    JS_NewClassID(&js_sse_stream_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_sse_stream_class_id, &js_sse_stream_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_sse_stream_proto_funcs,
                               sizeof(js_sse_stream_proto_funcs) /
                               sizeof(js_sse_stream_proto_funcs[0]));
    JS_SetClassProto(ctx, js_sse_stream_class_id, proto);
}

/* Create and return an SSE stream JS object. Calls kl_sse_begin.
 * Returns JS_EXCEPTION on error. */
JSValue hl_js_sse_create_stream(JSContext *ctx, KlResponse *res)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)js_sse_stream_class_id);
    if (JS_IsException(obj))
        return obj;

    HlJSSseStreamUD *ud = js_malloc(ctx, sizeof(HlJSSseStreamUD));
    if (!ud) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    ud->closed = 0;

    if (kl_sse_begin(res, &ud->sse) < 0) {
        js_free(ctx, ud);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    JS_SetOpaque(obj, ud);
    return obj;
}

/* Check if stream is closed (used by runtime.c for auto-close on handler return). */
int hl_js_sse_stream_is_closed(JSContext *ctx, JSValueConst val)
{
    (void)ctx;
    HlJSSseStreamUD *ud = (HlJSSseStreamUD *)JS_GetOpaque(val,
                                                             js_sse_stream_class_id);
    return ud ? ud->closed : 1;
}

/* Force-close the stream if still open. */
void hl_js_sse_stream_force_close(JSContext *ctx, JSValueConst val)
{
    (void)ctx;
    HlJSSseStreamUD *ud = (HlJSSseStreamUD *)JS_GetOpaque(val,
                                                             js_sse_stream_class_id);
    if (ud && !ud->closed) {
        kl_sse_end(&ud->sse);
        ud->closed = 1;
    }
}
