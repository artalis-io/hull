/*
 * mod_compute.c — hull:compute module (WASM compute plugins)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "mod_buffer.h"
#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/cap/wasm_stream.h"
#include "hull/cap/fs.h"
#include "hull/vfs.h"
#include "hull/worker_wasm.h"
#include "hull/async.h"
#include "hull/net_backend.h"
#include "hull/alloc.h"

#include <keel/server.h>
#include <stdatomic.h>

/* ── WasmBuffer JS class ──────────────────────────────────────────── */

static void js_wasm_buf_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlWasmBuffer *buf = JS_GetOpaque(val, js_wasm_buf_class_id);
    if (buf) {
        HlAllocator *a = buf->alloc;
        hl_wasm_buffer_destroy(buf);
        hl_alloc_free(a, buf, sizeof(HlWasmBuffer));
    }
}

static JSClassDef js_wasm_buf_class = {
    "WasmBuffer",
    .finalizer = js_wasm_buf_finalizer,
};

static JSValue js_wasm_buf_close(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmBuffer *buf = JS_GetOpaque2(ctx, this_val, js_wasm_buf_class_id);
    if (buf) {
        HlAllocator *a = buf->alloc;
        hl_wasm_buffer_destroy(buf);
        hl_alloc_free(a, buf, sizeof(HlWasmBuffer));
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue js_wasm_buf_bytes(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmBuffer *buf = JS_GetOpaque2(ctx, this_val, js_wasm_buf_class_id);
    if (!buf || buf->closed)
        return JS_NULL;
    const void *data = hl_wasm_buffer_data(buf);
    size_t len = hl_wasm_buffer_len(buf);
    if (data && len > 0)
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)data, len);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_wasm_buf_get_length(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmBuffer *buf = JS_GetOpaque2(ctx, this_val, js_wasm_buf_class_id);
    if (!buf || buf->closed) return JS_NewInt32(ctx, 0);
    return JS_NewInt64(ctx, (int64_t)hl_wasm_buffer_len(buf));
}

/* Helper: wrap HlWasmBuffer* as JS object. Takes ownership. */
static JSValue js_push_wasm_buffer(JSContext *ctx, HlWasmBuffer *buf)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)js_wasm_buf_class_id);
    if (JS_IsException(obj)) {
        HlAllocator *a = buf->alloc;
        hl_wasm_buffer_destroy(buf);
        hl_alloc_free(a, buf, sizeof(HlWasmBuffer));
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, buf);
    return obj;
}

static JSValue js_compute_buffer(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compute.buffer requires (input)");

    size_t len = 0;
    const uint8_t *data = JS_GetArrayBuffer(ctx, &len, argv[0]);
    int is_string = 0;
    if (!data) {
        data = (const uint8_t *)JS_ToCStringLen(ctx, &len, argv[0]);
        if (!data)
            return JS_ThrowTypeError(ctx, "compute.buffer: input must be a string or ArrayBuffer");
        is_string = 1;
    }

    HlJS *js_ctx = (HlJS *)JS_GetContextOpaque(ctx);
    HlWasmBuffer *buf = hl_wasm_buffer_create_owned(data, len,
                                                      js_ctx ? js_ctx->base.alloc : NULL);
    if (is_string) JS_FreeCString(ctx, (const char *)data);

    if (!buf)
        return JS_ThrowInternalError(ctx, "compute.buffer: out of memory");

    return js_push_wasm_buffer(ctx, buf);
}

static void js_register_wasm_buf_class(JSContext *ctx)
{
    JS_NewClassID(&js_wasm_buf_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_wasm_buf_class_id, &js_wasm_buf_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "close",
                      JS_NewCFunction(ctx, js_wasm_buf_close, "close", 0));
    JS_SetPropertyStr(ctx, proto, "bytes",
                      JS_NewCFunction(ctx, js_wasm_buf_bytes, "bytes", 0));

    JSAtom length_atom = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, proto, length_atom,
                            JS_NewCFunction(ctx, js_wasm_buf_get_length, "length", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, length_atom);

    JS_SetClassProto(ctx, js_wasm_buf_class_id, proto);
}

/* Clamp per-call opts to runtime ceiling */
static void js_wasm_clamp_opts(HlWasmCallOpts *opts, const HlRuntime *base)
{
    hl_cap_wasm_clamp_opts(opts,
        base->wasm_config.max_input, base->wasm_config.max_output,
        base->wasm_config.heap_size, base->wasm_config.stack_size,
        base->wasm_config.gas);
}

static JSValue js_compute_call(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.call: WASM runtime not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.call requires (name, input [, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    /* Input can be a string, ArrayBuffer, WasmBuffer, or MappedBuffer */
    size_t input_len = 0;
    const uint8_t *input = NULL;
    int input_is_string = 0;

    /* Check for WasmBuffer first */
    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[1], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        /* Check for MappedBuffer */
        HlMappedBuffer *mmap_buf = JS_GetOpaque2(ctx, argv[1], js_mmap_class_id);
        if (mmap_buf && !mmap_buf->closed) {
            input = (const uint8_t *)mmap_buf->addr;
            input_len = mmap_buf->len;
        } else {
            input = JS_GetArrayBuffer(ctx, &input_len, argv[1]);
            if (!input) {
                /* Try as string */
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[1]);
                if (!input) {
                    JS_FreeCString(ctx, name);
                    return JS_ThrowTypeError(ctx, "compute.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                }
                input_is_string = 1;
            }
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;

    /* Parse opts object if provided */
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue val;

        val = JS_GetPropertyStr(ctx, argv[2], "maxInput");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "maxOutput");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "heap");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.heap_size = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "stack");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.stack_size = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "gas");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }

    js_wasm_clamp_opts(&opts, &js->base);

    if (want_buffer) {
        HlWasmBuffer *out_buf = NULL;
        const char *err_msg = NULL;

        int rc = hl_cap_wasm_call_buf(js->base.wasm_cache, name,
                                       input, input_len,
                                       &out_buf, &opts, NULL, NULL,
                                       js->base.app_vfs,
                                       js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                       js->base.alloc, &err_msg);

        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        JS_FreeCString(ctx, name);

        if (rc != 0)
            return JS_ThrowInternalError(ctx, "compute.call: %s",
                                         err_msg ? err_msg : "unknown error");

        return js_push_wasm_buffer(ctx, out_buf);
    }

    /* Non-buffer path (original behavior) */
    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    int rc = hl_cap_wasm_call(js->base.wasm_cache, name,
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               js->base.app_vfs,
                               js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                               js->base.alloc, &err_msg);

    if (input_is_string)
        JS_FreeCString(ctx, (const char *)input);
    JS_FreeCString(ctx, name);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "compute.call: %s",
                                     err_msg ? err_msg : "unknown error");

    /* Return result as ArrayBuffer */
    if (output && output_len > 0) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, output, output_len);
        hl_alloc_free(js->base.alloc, output, output_len);
        return ab;
    }
    hl_alloc_free(js->base.alloc, output, output_len);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_compute_load(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.load: WASM runtime not initialized");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compute.load requires (name)");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    int rc = hl_cap_wasm_load(js->base.wasm_cache, name,
                               js->base.app_vfs,
                               js->base.app_vfs ? js->base.app_vfs->root_dir : NULL);
    JS_FreeCString(ctx, name);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "compute.load: %s",
                                     rc == HL_WASM_ERR_NOT_FOUND ? "not_found" : "load_failed");

    return JS_TRUE;
}

/* ── compute.async.call (Promise-based, thread pool dispatch) ──────── */

static JSValue js_push_worker_wasm_result(JSContext *ctx, void *driver)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)driver;

    if (op->error)
        return JS_ThrowInternalError(ctx, "compute.async.call: %s", op->error_msg);

    if (op->output_buf) {
        /* Buffer mode: wrap as WasmBuffer, transfer ownership */
        HlWasmBuffer *buf = op->output_buf;
        op->output_buf = NULL; /* prevent hl_worker_wasm_op_free from destroying */
        return js_push_wasm_buffer(ctx, buf);
    }

    if (op->output && op->output_len > 0)
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)op->output, op->output_len);

    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_compute_async_call(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "compute.async not available (no thread pool)");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx, "compute.async can only be called from a request handler");
    if (!js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.async.call: WASM runtime not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.async.call requires (name, input [, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    /* Get input (string, ArrayBuffer, or WasmBuffer) */
    const uint8_t *input = NULL;
    size_t input_len = 0;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[1], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque2(ctx, argv[1], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            size_t ab_len;
            input = JS_GetArrayBuffer(ctx, &ab_len, argv[1]);
            if (input) {
                input_len = ab_len;
            } else {
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[1]);
                if (!input) {
                    JS_FreeCString(ctx, name);
                    return JS_ThrowTypeError(ctx, "compute.async.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                }
                input_is_string = 1;
            }
        }
    }

    /* Parse opts */
    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[2], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "heap");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.heap_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "stack");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.stack_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }

    js_wasm_clamp_opts(&opts, &js->base);

    /* Pre-load module on event loop thread (cache writes are not thread-safe) */
    int rc = hl_cap_wasm_load(js->base.wasm_cache, name,
                               js->base.app_vfs,
                               js->base.app_vfs ? js->base.app_vfs->root_dir : NULL);
    if (rc != 0) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "compute.async.call: %s",
                                     rc == HL_WASM_ERR_NOT_FOUND ? "not_found" : "load_failed");
    }

    /* Allocate op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
    }

    op->server = js->server;
    op->wasm_cache = js->base.wasm_cache;
    op->app_vfs = js->base.app_vfs;
    op->app_dir = js->base.app_vfs ? js->base.app_vfs->root_dir : NULL;
    op->alloc = js->base.alloc;
    snprintf(op->name, sizeof(op->name), "%s", name);
    op->opts = opts;
    op->want_buffer = want_buffer;

    /* Deep-copy input */
    if (input_len > 0) {
        op->input = malloc(input_len);
        if (!op->input) {
            if (input_is_string) JS_FreeCString(ctx, (const char *)input);
            JS_FreeCString(ctx, name);
            free(op);
            return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
        }
        memcpy(op->input, input, input_len);
    }
    op->input_len = input_len;

    if (input_is_string) JS_FreeCString(ctx, (const char *)input);
    JS_FreeCString(ctx, name);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx, js->base.alloc);
    if (!actx) {
        hl_worker_wasm_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    /* Create JS continuation */
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_wasm_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_wasm_op_free_all;
    actx->op.on_cancel = hl_worker_wasm_async_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_worker_wasm_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "compute.async.call: thread pool full");
    }

    /* Suspend connection */
    if (hl_net_op_suspend(js->base.net_ctx, (HlReqHandle *)js->active_conn, (HlSuspendOp *)&actx->op) < 0) {
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "compute.async.call: failed to suspend");
    }

    return promise;
}

/* ── WasmInstance JS class ──────────────────────────────────────────── */

static JSClassID js_wasm_inst_class_id;

static void js_wasm_inst_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlWasmInstance *pi = JS_GetOpaque(val, js_wasm_inst_class_id);
    if (pi)
        hl_cap_wasm_instance_destroy(pi);
}

static JSClassDef js_wasm_inst_class = {
    "WasmInstance",
    .finalizer = js_wasm_inst_finalizer,
};

static JSValue js_wasm_inst_call(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (!pi)
        return JS_ThrowInternalError(ctx, "WasmInstance.call: invalid instance");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WasmInstance.call requires (input [, opts])");

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);

    /* Parse input */
    size_t input_len = 0;
    const uint8_t *input = NULL;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[0], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque2(ctx, argv[0], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            input = JS_GetArrayBuffer(ctx, &input_len, argv[0]);
            if (!input) {
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[0]);
                if (!input)
                    return JS_ThrowTypeError(ctx, "WasmInstance.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                input_is_string = 1;
            }
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[1], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }

    if (js) js_wasm_clamp_opts(&opts, &js->base);

    if (want_buffer) {
        HlWasmBuffer *out_buf = NULL;
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_instance_call_buf(pi, input, input_len,
                                                &out_buf, &opts, NULL, NULL,
                                                js ? js->base.alloc : NULL, &err_msg);
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        if (rc != 0)
            return JS_ThrowInternalError(ctx, "WasmInstance.call: %s",
                                         err_msg ? err_msg : "unknown error");
        return js_push_wasm_buffer(ctx, out_buf);
    }

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;
    int rc = hl_cap_wasm_instance_call(pi, input, input_len,
                                        &output, &output_len,
                                        &opts, NULL, NULL,
                                        js ? js->base.alloc : NULL, &err_msg);
    if (input_is_string) JS_FreeCString(ctx, (const char *)input);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "WasmInstance.call: %s",
                                     err_msg ? err_msg : "unknown error");

    if (output && output_len > 0) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, output, output_len);
        if (js) hl_alloc_free(js->base.alloc, output, output_len);
        else free(output);
        return ab;
    }
    if (js) hl_alloc_free(js->base.alloc, output, output_len);
    else free(output);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_wasm_inst_async_call(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv);

static JSValue js_wasm_inst_close(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (pi) {
        hl_cap_wasm_instance_destroy(pi);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue js_wasm_inst_get_name(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (!pi || pi->closed) return JS_NULL;
    return JS_NewString(ctx, pi->name);
}

static JSValue js_wasm_inst_get_closed(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    return JS_NewBool(ctx, !pi || pi->closed);
}

static JSValue js_push_wasm_instance(JSContext *ctx, HlWasmInstance *pi)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)js_wasm_inst_class_id);
    if (JS_IsException(obj)) {
        hl_cap_wasm_instance_destroy(pi);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, pi);
    return obj;
}

/* inst.async.call proxy */
static JSValue js_wasm_inst_async_proxy_call(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              int magic, JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    return js_wasm_inst_async_call(ctx, func_data[0], argc, argv);
}

static JSValue js_wasm_inst_get_async(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue async_obj = JS_NewObject(ctx);
    JSValue inst = JS_DupValue(ctx, this_val);
    JS_SetPropertyStr(ctx, async_obj, "call",
                      JS_NewCFunctionData(ctx, js_wasm_inst_async_proxy_call, 2, 0, 1, &inst));
    JS_FreeValue(ctx, inst);
    return async_obj;
}

static JSValue js_wasm_inst_async_call(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: no thread pool");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: can only be called from a request handler");

    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (!pi || pi->closed)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance closed");
    if (atomic_load(&pi->busy))
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance busy");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WasmInstance.asyncCall requires (input [, opts])");

    /* Parse input */
    const uint8_t *input = NULL;
    size_t input_len = 0;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[0], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque2(ctx, argv[0], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            input = JS_GetArrayBuffer(ctx, &input_len, argv[0]);
            if (!input) {
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[0]);
                if (!input)
                    return JS_ThrowTypeError(ctx, "WasmInstance.async.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                input_is_string = 1;
            }
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[1], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }
    js_wasm_clamp_opts(&opts, &js->base);

    /* Allocate op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
    }

    op->server = js->server;
    op->persistent_inst = pi;
    op->alloc = js->base.alloc;
    snprintf(op->name, sizeof(op->name), "%s", pi->name);
    op->opts = opts;
    op->want_buffer = want_buffer;

    /* Deep-copy input */
    if (input_len > 0) {
        op->input = malloc(input_len);
        if (!op->input) {
            if (input_is_string) JS_FreeCString(ctx, (const char *)input);
            free(op);
            return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
        }
        memcpy(op->input, input, input_len);
    }
    op->input_len = input_len;

    if (input_is_string) JS_FreeCString(ctx, (const char *)input);

    /* Set busy before dispatch */
    atomic_store(&pi->busy, 1);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx, js->base.alloc);
    if (!actx) {
        atomic_store(&pi->busy, 0);
        hl_worker_wasm_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        atomic_store(&pi->busy, 0);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_wasm_result);
    if (!cont) {
        atomic_store(&pi->busy, 0);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_wasm_op_free_all;
    actx->op.on_cancel = hl_worker_wasm_async_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    if (hl_worker_wasm_submit(js->base.thread_pool, op) != 0) {
        atomic_store(&pi->busy, 0);
        actx->cont->destroy(actx->cont);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: thread pool full");
    }

    if (hl_net_op_suspend(js->base.net_ctx, (HlReqHandle *)js->active_conn, (HlSuspendOp *)&actx->op) < 0) {
        atomic_store(&pi->busy, 0);
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: failed to suspend");
    }

    return promise;
}

static void js_register_wasm_inst_class(JSContext *ctx)
{
    JS_NewClassID(&js_wasm_inst_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_wasm_inst_class_id, &js_wasm_inst_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "call",
                      JS_NewCFunction(ctx, js_wasm_inst_call, "call", 2));
    JS_SetPropertyStr(ctx, proto, "close",
                      JS_NewCFunction(ctx, js_wasm_inst_close, "close", 0));

    JSAtom async_atom = JS_NewAtom(ctx, "async");
    JS_DefinePropertyGetSet(ctx, proto, async_atom,
                            JS_NewCFunction(ctx, js_wasm_inst_get_async, "async", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, async_atom);

    JSAtom name_atom = JS_NewAtom(ctx, "name");
    JS_DefinePropertyGetSet(ctx, proto, name_atom,
                            JS_NewCFunction(ctx, js_wasm_inst_get_name, "name", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, name_atom);

    JSAtom closed_atom = JS_NewAtom(ctx, "closed");
    JS_DefinePropertyGetSet(ctx, proto, closed_atom,
                            JS_NewCFunction(ctx, js_wasm_inst_get_closed, "closed", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, closed_atom);

    JS_SetClassProto(ctx, js_wasm_inst_class_id, proto);
}

/* compute.instance(name, opts?) -> WasmInstance | throws */
static JSValue js_compute_instance(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.instance: WASM runtime not initialized");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compute.instance requires (name [, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    HlWasmCallOpts opts = {0};
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[1], "heap");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.heap_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "stack");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.stack_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
    }

    js_wasm_clamp_opts(&opts, &js->base);

    const char *err_msg = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        js->base.wasm_cache, name, &opts,
        js->base.app_vfs,
        js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
        js->base.alloc, &err_msg);

    JS_FreeCString(ctx, name);

    if (!pi)
        return JS_ThrowInternalError(ctx, "compute.instance: %s",
                                     err_msg ? err_msg : "unknown error");

    return js_push_wasm_instance(ctx, pi);
}

/* compute.segment(module, segment, data) */
static JSValue js_compute_segment(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.segment: WASM runtime not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.segment requires (module, segment [, data])");

    const char *module_name = JS_ToCString(ctx, argv[0]);
    if (!module_name)
        return JS_EXCEPTION;

    /* compute.segment(module, null) -> remove all */
    if (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_data_load(js->base.wasm_cache, module_name,
                                        NULL, NULL, 0, NULL,
                                        js->base.app_vfs,
                                        js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                        &err_msg);
        JS_FreeCString(ctx, module_name);
        if (rc != 0)
            return JS_ThrowInternalError(ctx, "compute.segment: %s",
                                         err_msg ? err_msg : "unknown error");
        return JS_TRUE;
    }

    const char *segment_name = JS_ToCString(ctx, argv[1]);
    if (!segment_name) {
        JS_FreeCString(ctx, module_name);
        return JS_EXCEPTION;
    }

    /* compute.segment(module, segment, null/undefined) -> remove segment */
    if (argc < 3 || JS_IsNull(argv[2]) || JS_IsUndefined(argv[2])) {
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_data_load(js->base.wasm_cache, module_name,
                                        segment_name, NULL, 0, NULL,
                                        js->base.app_vfs,
                                        js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                        &err_msg);
        JS_FreeCString(ctx, module_name);
        JS_FreeCString(ctx, segment_name);
        if (rc != 0)
            return JS_ThrowInternalError(ctx, "compute.segment: %s",
                                         err_msg ? err_msg : "unknown error");
        return JS_TRUE;
    }

    /* compute.segment(module, segment, data) -> add/replace */
    const void *data = NULL;
    size_t data_len = 0;
    void *pre_alloc = NULL;
    uint8_t *ab_buf = NULL;

    if (JS_IsString(argv[2])) {
        data = JS_ToCStringLen(ctx, &data_len, argv[2]);
        if (!data) {
            JS_FreeCString(ctx, module_name);
            JS_FreeCString(ctx, segment_name);
            return JS_EXCEPTION;
        }
    } else {
        /* Try MappedBuffer (zero-copy) */
        HlMappedBuffer *mmap_buf = JS_GetOpaque2(ctx, argv[2], js_mmap_class_id);
        if (mmap_buf && !mmap_buf->closed) {
            data = mmap_buf->addr;
            data_len = mmap_buf->len;
            pre_alloc = mmap_buf->addr;
        } else {
            /* Try ArrayBuffer */
            ab_buf = JS_GetArrayBuffer(ctx, &data_len, argv[2]);
            if (ab_buf) {
                data = ab_buf;
            } else {
                JS_FreeCString(ctx, module_name);
                JS_FreeCString(ctx, segment_name);
                return JS_ThrowTypeError(ctx, "compute.segment: data must be a string, ArrayBuffer, or MappedBuffer");
            }
        }
    }

    const char *err_msg = NULL;
    int rc = hl_cap_wasm_data_load(js->base.wasm_cache, module_name,
                                    segment_name, data, data_len, pre_alloc,
                                    js->base.app_vfs,
                                    js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                    &err_msg);

    if (JS_IsString(argv[2]))
        JS_FreeCString(ctx, (const char *)data);
    JS_FreeCString(ctx, module_name);
    JS_FreeCString(ctx, segment_name);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "compute.segment: %s",
                                     err_msg ? err_msg : "unknown error");
    return JS_TRUE;
}

/* ── compute.stream ────────────────────────────────────────────────── */

typedef struct {
    JSContext  *ctx;
    JSValue     func;
    int         error;
} JsStreamCbCtx;

static int js_stream_cb_trampoline(const void *data, size_t len,
                                    uint32_t index, int is_last,
                                    void *user_data)
{
    JsStreamCbCtx *cb = (JsStreamCbCtx *)user_data;
    if (cb->error) return -1;

    JSValue args[3];
    args[0] = JS_NewArrayBufferCopy(cb->ctx, (const uint8_t *)data, len);
    args[1] = JS_NewUint32(cb->ctx, index);
    args[2] = JS_NewBool(cb->ctx, is_last);

    JSValue ret = JS_Call(cb->ctx, cb->func, JS_UNDEFINED, 3, args);
    JS_FreeValue(cb->ctx, args[0]);
    JS_FreeValue(cb->ctx, args[1]);
    JS_FreeValue(cb->ctx, args[2]);

    if (JS_IsException(ret)) {
        cb->error = 1;
        JS_FreeValue(cb->ctx, ret);
        return -1;
    }
    JS_FreeValue(cb->ctx, ret);
    return 0;
}

/* compute.stream(name, input, [output], [opts]) -> ArrayBuffer | true */
static JSValue js_compute_stream(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.stream: WASM runtime not initialized");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.stream requires (name, input [, output, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    /* ── Parse input (arg 1) ─────────────────────────────────────── */
    HlStreamInput input = {0};
    const char *in_file_str = NULL;
    int input_is_string = 0;

    if (JS_IsObject(argv[1])) {
        JSValue fv = JS_GetPropertyStr(ctx, argv[1], "file");
        if (JS_IsString(fv)) {
            in_file_str = JS_ToCString(ctx, fv);
            if (in_file_str) {
                input.kind = HL_STREAM_IN_FILE;
                input.path = in_file_str;
            }
        }
        JS_FreeValue(ctx, fv);
    }
    if (input.kind != HL_STREAM_IN_FILE) {
        /* Try buffer protocol / ArrayBuffer / string */
        size_t ilen = 0;
        const uint8_t *idata = JS_GetArrayBuffer(ctx, &ilen, argv[1]);
        if (idata) {
            input.kind = HL_STREAM_IN_BUFFER;
            input.buffer.data = idata;
            input.buffer.len = ilen;
        } else {
            HlWasmBuffer *wbuf = JS_GetOpaque2(ctx, argv[1], js_wasm_buf_class_id);
            if (wbuf && !wbuf->closed) {
                input.kind = HL_STREAM_IN_BUFFER;
                input.buffer.data = hl_wasm_buffer_data(wbuf);
                input.buffer.len = hl_wasm_buffer_len(wbuf);
            } else {
                HlMappedBuffer *mmap = JS_GetOpaque2(ctx, argv[1], js_mmap_class_id);
                if (mmap && !mmap->closed) {
                    input.kind = HL_STREAM_IN_BUFFER;
                    input.buffer.data = mmap->addr;
                    input.buffer.len = mmap->len;
                } else {
                    const char *s = JS_ToCStringLen(ctx, &ilen, argv[1]);
                    if (!s) {
                        JS_FreeCString(ctx, name);
                        return JS_ThrowTypeError(ctx, "compute.stream: invalid input");
                    }
                    input.kind = HL_STREAM_IN_BUFFER;
                    input.buffer.data = s;
                    input.buffer.len = ilen;
                    input_is_string = 1;
                }
            }
        }
    }

    /* ── Parse output (arg 2) and opts (arg 2 or 3) ──────────────── */
    HlStreamOutput out_storage = {0};
    HlStreamOutput *out_ptr = NULL;
    void *out_data = NULL;
    size_t out_len = 0;
    JsStreamCbCtx cb_ctx = {0};
    int has_output = 0;
    int opts_idx = 3;
    const char *out_file_str = NULL;

    if (argc > 2 && JS_IsFunction(ctx, argv[2])) {
        has_output = 1;
        cb_ctx.ctx = ctx;
        cb_ctx.func = JS_DupValue(ctx, argv[2]);
        out_storage.kind = HL_STREAM_OUT_CALLBACK;
        out_storage.callback.fn = js_stream_cb_trampoline;
        out_storage.callback.user_data = &cb_ctx;
        out_ptr = &out_storage;
    } else if (argc > 2 && JS_IsObject(argv[2]) && !JS_IsNull(argv[2])) {
        JSValue fv = JS_GetPropertyStr(ctx, argv[2], "file");
        if (JS_IsString(fv)) {
            out_file_str = JS_ToCString(ctx, fv);
            if (out_file_str) {
                has_output = 1;
                out_storage.kind = HL_STREAM_OUT_FILE;
                out_storage.path = out_file_str;
                out_ptr = &out_storage;
            }
        }
        JS_FreeValue(ctx, fv);
        if (!has_output) {
            /* It's the opts object */
            opts_idx = 2;
        }
    } else if (argc > 2 && (JS_IsNull(argv[2]) || JS_IsUndefined(argv[2]))) {
        opts_idx = 3;
    }

    if (!has_output) {
        out_storage.kind = HL_STREAM_OUT_BUFFER;
        out_storage.buffer.data = &out_data;
        out_storage.buffer.len = &out_len;
        out_ptr = &out_storage;
    }

    /* ── Parse opts ──────────────────────────────────────────────── */
    HlStreamOpts stream_opts = {0};
    if (opts_idx < argc && JS_IsObject(argv[opts_idx])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[opts_idx], "chunkSize");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val);
            stream_opts.chunk_size = (size_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[opts_idx], "gas");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val);
            stream_opts.call_opts.gas = v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[opts_idx], "heap");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val);
            stream_opts.call_opts.heap_size = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[opts_idx], "stack");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val);
            stream_opts.call_opts.stack_size = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);
    }

    js_wasm_clamp_opts(&stream_opts.call_opts, &js->base);

    /* ── Call stream API ─────────────────────────────────────────── */
    const char *err = NULL;
    HlStreamResult res = {0};

    int rc = hl_cap_wasm_stream(
        js->base.wasm_cache, name,
        &input, out_ptr, &stream_opts,
        js->base.fs_cfg,
        js->base.app_vfs,
        js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
        js->base.alloc, &res, &err);

    /* Cleanup */
    if (input_is_string)
        JS_FreeCString(ctx, (const char *)input.buffer.data);
    if (in_file_str)
        JS_FreeCString(ctx, in_file_str);
    if (out_file_str)
        JS_FreeCString(ctx, out_file_str);
    if (has_output && out_storage.kind == HL_STREAM_OUT_CALLBACK)
        JS_FreeValue(ctx, cb_ctx.func);
    JS_FreeCString(ctx, name);

    if (rc != HL_WASM_OK)
        return JS_ThrowInternalError(ctx, "compute.stream: %s",
                                     err ? err : "stream_failed");

    if (out_storage.kind == HL_STREAM_OUT_BUFFER && out_data) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)out_data, out_len);
        hl_alloc_free(js->base.alloc, out_data, out_len);
        return ab;
    } else if (out_storage.kind == HL_STREAM_OUT_BUFFER) {
        return JS_NewArrayBufferCopy(ctx, NULL, 0);
    }

    return JS_TRUE;
}

static JSValue js_compute_available(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    return JS_NewBool(ctx, js && js->base.wasm_cache != NULL);
}

static int js_compute_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/compute", "hull:compute") != 0)
        return -1;

    JSValue compute = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, compute, "available",
                      JS_NewCFunction(ctx, js_compute_available, "available", 0));
    JS_SetPropertyStr(ctx, compute, "call",
                      JS_NewCFunction(ctx, js_compute_call, "call", 3));
    JS_SetPropertyStr(ctx, compute, "load",
                      JS_NewCFunction(ctx, js_compute_load, "load", 1));
    JS_SetPropertyStr(ctx, compute, "buffer",
                      JS_NewCFunction(ctx, js_compute_buffer, "buffer", 1));
    JS_SetPropertyStr(ctx, compute, "instance",
                      JS_NewCFunction(ctx, js_compute_instance, "instance", 2));
    JS_SetPropertyStr(ctx, compute, "segment",
                      JS_NewCFunction(ctx, js_compute_segment, "segment", 3));
    JS_SetPropertyStr(ctx, compute, "stream",
                      JS_NewCFunction(ctx, js_compute_stream, "stream", 4));

    /* compute.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "call",
                      JS_NewCFunction(ctx, js_compute_async_call, "call", 3));
    JS_SetPropertyStr(ctx, compute, "async", async_obj);

    JS_SetModuleExport(ctx, m, "compute", compute);
    return 0;
}

int hl_js_init_compute_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    js_register_wasm_buf_class(ctx);
    js_register_wasm_inst_class(ctx);
    JSModuleDef *m = JS_NewCModule(ctx, "hull:compute", js_compute_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "compute");
    return 0;
}

#endif /* HL_ENABLE_WASM */
