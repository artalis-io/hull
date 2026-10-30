/*
 * mod_compute.c - hull:compute module (WASM compute plugins)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "refusal.h"
#include "mod_buffer.h"
#include "internal.h"            /* async gate, instruction budget */
#include "hull/cap/wasm.h"
#include "db_wait.h"   /* hl_js_db_refuse_wait */
#include "hull/cap/wasm_spans.h" /* HL_WASM_MAX_SPANS */
#include "hull/cap/wasm_buffer.h"
#include "hull/cap/wasm_stream.h"
#include "hull/cap/fs.h"
#include "hull/vfs.h"
#include "hull/worker_wasm.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */
#include "hull/utils/alloc.h"

#include <keel/http_server.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>   /* sysconf: a segment's page size */

/* ── WasmBuffer JS class ──────────────────────────────────────────── */

static void js_wasm_buf_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlWasmBuffer *buf = JS_GetOpaque(val, js_wasm_buf_class_id);
    if (buf)
        hl_wasm_buffer_close(buf); /* defers if still borrowed by an image */
}

static const JSClassDef js_wasm_buf_class = {
    "WasmBuffer",
    .finalizer = js_wasm_buf_finalizer,
};

static JSValue js_wasm_buf_close(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmBuffer *buf = JS_GetOpaque2(ctx, this_val, js_wasm_buf_class_id);
    if (buf) {
        hl_wasm_buffer_close(buf); /* defers if still borrowed by an image */
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
    /* The object QuickJS sees is a few bytes; the bytes - or the pooled
     * WASM instance (heap + stack + linear memory) a zero-copy buffer keeps
     * checked out - behind it are not, so a loop dropping
     * compute.call({buffer: true}) results never pushed QuickJS's own
     * malloc counter to a collection. Charge them as debt and run a full GC
     * past HL_JS_OFFHEAP_GC_DEBT (audit 12), before the new object exists. */
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    /* An OWNED buffer's bytes live on the server allocator, which no VM
     * limit covers: count them against the heap limit (audit 18). Buffers
     * dropped but not yet collected still count, so a refusal collects once
     * and asks again before it throws. */
    if (js && hl_wasm_buffer_account(buf, &js->wasm_buf_bytes,
                                     js->max_heap_bytes) != 0) {
        JS_RunGC(JS_GetRuntime(ctx));
        js->offheap_gc_debt = 0;
        if (hl_wasm_buffer_account(buf, &js->wasm_buf_bytes,
                                   js->max_heap_bytes) != 0) {
            size_t n = hl_wasm_buffer_len(buf);
            HlAllocator *a = buf->alloc;
            hl_wasm_buffer_destroy(buf);
            hl_alloc_free(a, buf, sizeof(HlWasmBuffer));
            return hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowRangeError(ctx,
                "buffer of %zu bytes would take the VM's buffers past its "
                "heap limit (close unused buffers)", n));
        }
    }
    if (js) {
        size_t fp = hl_wasm_buffer_footprint(buf);
        js->offheap_gc_debt = js->offheap_gc_debt > SIZE_MAX - fp
                                  ? SIZE_MAX : js->offheap_gc_debt + fp;
        if (js->offheap_gc_debt >= HL_JS_OFFHEAP_GC_DEBT) {
            js->offheap_gc_debt = 0;
            JS_RunGC(JS_GetRuntime(ctx));
        }
    }
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

/* An input that is not a WasmBuffer / MappedBuffer (audit 29): an
 * ArrayBuffer's or a typed array's bytes (borrowed - the caller takes them
 * after its last app code), a primitive converted as a string (*is_string,
 * JS_FreeCString it), any other object a TypeError: a Float32Array went in
 * as its toString "1,2,3", any object as "[object Object]". NULL with an
 * exception pending on failure. */
static const uint8_t *js_compute_input_bytes(JSContext *ctx, JSValueConst v,
                                             size_t *len, int *is_string,
                                             const char *what)
{
    *is_string = 0;
    *len = 0;
    if (JS_IsObject(v)) {
        const uint8_t *p = hl_js_bytes_view(ctx, len, v);
        if (p) return p;
        JS_ThrowTypeError(ctx, "%s: input must be a string, ArrayBuffer, "
                               "typed array, WasmBuffer, or MappedBuffer", what);
        return NULL;
    }
    const char *s = JS_ToCStringLen(ctx, len, v);
    if (!s) return NULL;
    *is_string = 1;
    return (const uint8_t *)s;
}

static JSValue js_compute_buffer(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compute.buffer requires (input)");

    size_t len = 0;
    int is_string = 0;
    const uint8_t *data = js_compute_input_bytes(ctx, argv[0], &len,
                                                 &is_string, "compute.buffer");
    if (!data)
        return JS_EXCEPTION;

    /* The copy is charged before it is made, 1 unit per 8 bytes (audit 18),
     * and js_push_wasm_buffer counts it against the heap limit. */
    if (hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)len))) {
        if (is_string) JS_FreeCString(ctx, (const char *)data);
        return JS_EXCEPTION;
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

    hl_js_set_class_proto_frozen(ctx, js_wasm_buf_class_id, proto);
}

/* Clamp per-call opts to runtime ceiling */
static void js_wasm_clamp_opts(HlWasmCallOpts *opts, const HlRuntime *base)
{
    hl_cap_wasm_clamp_opts(opts,
        base->wasm_config.max_input, base->wasm_config.max_output,
        base->wasm_config.heap_size, base->wasm_config.stack_size,
        base->wasm_config.gas, base->wasm_config.timeout_ms);
}

/* The same ceilings for a call on a persistent instance, without filling the
 * fields the call left unset: those take the instance's own defaults, already
 * clamped at compute.instance() (round-6 L2). */
static void js_wasm_cap_call_opts(HlWasmCallOpts *opts, const HlRuntime *base)
{
    hl_cap_wasm_cap_call_opts(opts,
        base->wasm_config.max_input, base->wasm_config.max_output,
        base->wasm_config.heap_size, base->wasm_config.stack_size,
        base->wasm_config.gas, base->wasm_config.timeout_ms);
}

/* opts.timeoutMs: a positive number of milliseconds; anything else is "the
 * default" (never "unbounded"). */
static uint32_t js_wasm_timeout_arg(int64_t v)
{
    if (v <= 0) return 0;
    return v > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

#define JS_WOPT_IO  1   /* maxInput / maxOutput */
#define JS_WOPT_MEM 2   /* heap / stack */

/* Read the numeric call options (gas and timeoutMs always; maxInput /
 * maxOutput and heap / stack per `which`) from the options object o. 0, or
 * -1 with the exception pending: a getter or valueOf that threw, or the
 * budget interrupt (audit 23 - the conversions' results were ignored, so the
 * call went on with a default and the exception left pending). */
static int js_wasm_parse_opts(JSContext *ctx, JSValueConst o,
                              HlWasmCallOpts *opts, int which)
{
    int64_t v; int r;
    if (which & JS_WOPT_IO) {
        if ((r = hl_js_opt_int64(ctx, o, "maxInput", &v)) < 0) return -1;
        if (r) opts->max_input = (uint32_t)v;
        if ((r = hl_js_opt_int64(ctx, o, "maxOutput", &v)) < 0) return -1;
        if (r) opts->max_output = (uint32_t)v;
    }
    if (which & JS_WOPT_MEM) {
        if ((r = hl_js_opt_int64(ctx, o, "heap", &v)) < 0) return -1;
        if (r) opts->heap_size = (uint32_t)v;
        if ((r = hl_js_opt_int64(ctx, o, "stack", &v)) < 0) return -1;
        if (r) opts->stack_size = (uint32_t)v;
    }
    if ((r = hl_js_opt_int64(ctx, o, "gas", &v)) < 0) return -1;
    if (r) opts->gas = v;
    if ((r = hl_js_opt_int64(ctx, o, "timeoutMs", &v)) < 0) return -1;
    if (r) opts->timeout_ms = js_wasm_timeout_arg(v);
    return 0;
}

/* Free the OWNED (JS_ToCString) name copies in reqs[0..n). Idempotent. */
static void js_free_span_names(JSContext *ctx, HlWasmSpanReq *reqs,
                               JSValue *holders, int n)
{
    for (int i = 0; i < n; i++) {
        if (reqs[i].name) { JS_FreeCString(ctx, reqs[i].name); reqs[i].name = NULL; }
        if (reqs[i].buf) { hl_cap_fs_mmap_release(reqs[i].buf); reqs[i].buf = NULL; }
        JS_FreeValue(ctx, holders[i]);
        holders[i] = JS_UNDEFINED;
    }
}

/* Parse opts.spans = [{name, buffer:<MappedBuffer>}, ...] into reqs[] (cap
 * HL_WASM_MAX_SPANS), per the public-option contract. Returns the count (0 when
 * spans is absent/empty => a plain call), or -1 with a pending JS exception on any
 * malformed entry: not an array, > HL_WASM_MAX_SPANS entries, a non-object entry,
 * a name that is not a string / empty / > 63 bytes / has an embedded NUL / is a
 * duplicate within the request, or a buffer that is not a live MappedBuffer.
 *
 * On success reqs[i].name is an OWNED C string; the caller frees the set with
 * js_free_span_names (after a synchronous call, or immediately when only
 * validating). reqs[i].buf is a borrowed MappedBuffer pointer. On -1 every name
 * allocated so far is freed. An ASYNC caller must deep-copy names into non-JS
 * storage and pin each buffer before submit (item D); it must NOT retain these. */
/* holders[i] keeps span i's MappedBuffer object referenced until
 * js_free_span_names: `buffer` may be a getter returning a fresh mapping,
 * freed (unmapped) the moment its value is released - reqs[i].buf then
 * named freed memory for the rest of the parse and the call. */
static int js_parse_spans(JSContext *ctx, JSValueConst opts, HlWasmSpanReq *reqs,
                          JSValue *holders)
{
    JSValue arr = JS_GetPropertyStr(ctx, opts, "spans");
    /* A getter's exception (or the budget interrupt) stays the error: the
     * TypeErrors below would replace it (audit 23). */
    if (JS_IsException(arr)) return -1;
    if (JS_IsUndefined(arr) || JS_IsNull(arr)) { JS_FreeValue(ctx, arr); return 0; }
    if (!JS_IsArray(ctx, arr)) {
        JS_FreeValue(ctx, arr);
        JS_ThrowTypeError(ctx, "compute: spans must be an array");
        return -1;
    }
    JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    /* A Proxy array's length trap is app code that can throw (audit 23). */
    int lrc = JS_IsException(lenv) ? -1 : JS_ToUint32(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    if (lrc < 0) { JS_FreeValue(ctx, arr); return -1; }
    if (n > (uint32_t)HL_WASM_MAX_SPANS) {
        JS_FreeValue(ctx, arr);
        JS_ThrowRangeError(ctx, "compute: at most %d spans (got %u)",
                           HL_WASM_MAX_SPANS, n);
        return -1;
    }
    int count = 0;
    for (uint32_t i = 0; i < n; i++) {
        JSValue entry = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(entry)) goto fail;
        if (!JS_IsObject(entry)) {
            JS_FreeValue(ctx, entry);
            JS_ThrowTypeError(ctx, "compute: spans[%u] must be an object", i);
            goto fail;
        }
        JSValue nv = JS_GetPropertyStr(ctx, entry, "name");
        if (JS_IsException(nv)) { JS_FreeValue(ctx, entry); goto fail; }
        if (!JS_IsString(nv)) {
            JS_FreeValue(ctx, nv); JS_FreeValue(ctx, entry);
            JS_ThrowTypeError(ctx, "compute: spans[%u].name must be a string", i);
            goto fail;
        }
        size_t nlen = 0;
        const char *nm = JS_ToCStringLen(ctx, &nlen, nv);
        JS_FreeValue(ctx, nv);
        if (!nm || nlen == 0 || nlen >= 64 || strlen(nm) != nlen) {
            if (nm) JS_FreeCString(ctx, nm);
            JS_FreeValue(ctx, entry);
            JS_ThrowTypeError(ctx,
                "compute: spans[%u].name must be 1..63 bytes with no embedded NUL", i);
            goto fail;
        }
        for (int j = 0; j < count; j++) {
            if (strcmp(reqs[j].name, nm) == 0) {
                JS_FreeCString(ctx, nm); JS_FreeValue(ctx, entry);
                JS_ThrowTypeError(ctx, "compute: duplicate span name '%s'", reqs[j].name);
                goto fail;
            }
        }
        JSValue bv = JS_GetPropertyStr(ctx, entry, "buffer");
        if (JS_IsException(bv)) {
            JS_FreeValue(ctx, entry);
            JS_FreeCString(ctx, nm);
            goto fail;
        }
        HlMappedBuffer *mb = JS_GetOpaque2(ctx, bv, js_mmap_class_id);
        JS_FreeValue(ctx, entry);
        if (!mb) {
            JS_FreeValue(ctx, bv);
            JS_FreeCString(ctx, nm);
            /* JS_GetOpaque2 set a class exception; replace with a clearer message. */
            JS_ThrowTypeError(ctx, "compute: spans[%u].buffer must be a MappedBuffer", i);
            goto fail;
        }
        if (mb->closed) {
            JS_FreeValue(ctx, bv);
            JS_FreeCString(ctx, nm);
            JS_ThrowTypeError(ctx, "compute: spans[%u].buffer is closed", i);
            goto fail;
        }
        reqs[count].name = nm;   /* owned */
        /* Borrowed until js_free_span_names: holding the JS object does not
         * keep the mapping alive - mb.close() frees an unborrowed buffer -
         * and app code still runs after this (a later entry's getter, the
         * input's toString), so the span pointed at freed memory. */
        hl_cap_fs_mmap_borrow(mb);
        reqs[count].buf = mb;
        holders[count] = bv;     /* held: released by js_free_span_names */
        count++;
    }
    JS_FreeValue(ctx, arr);
    return count;

fail:
    JS_FreeValue(ctx, arr);
    js_free_span_names(ctx, reqs, holders, count);
    return -1;
}

/* The synchronous compute.call / instance.call charged neither their input
 * nor their output (audit 27). The input is charged 1 unit per 8 bytes before
 * the call, and a maxOutput above the VM heap limit comes down to it (as
 * gpu / compute.stream clamp theirs): the output is copied onto that heap. */
static int js_wasm_sync_prepare(JSContext *ctx, const HlJS *js,
                                HlWasmCallOpts *opts, size_t input_len)
{
    if (js && js->max_heap_bytes && opts->max_output > js->max_heap_bytes)
        opts->max_output = js->max_heap_bytes;
    return hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)input_len))
           < 0 ? -1 : 0;
}

/* The output copied into an ArrayBuffer, charged 1 unit per 8 bytes before
 * the copy and refused past the heap limit; @p output is freed either way. */
static JSValue js_wasm_output_to_ab(JSContext *ctx, const HlJS *js,
                                    void *output, size_t output_len,
                                    const char *fn)
{
    JSValue ab;
    if (!output) output_len = 0;
    if (js && js->max_heap_bytes && output_len > js->max_heap_bytes)
        ab = JS_ThrowRangeError(ctx, "%s: output_too_large (%zu bytes is "
                                "more than the VM heap limit)", fn, output_len);
    else if (hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)output_len)) < 0)
        ab = JS_EXCEPTION;
    else
        ab = JS_NewArrayBufferCopy(ctx, output_len ? output : NULL, output_len);
    if (js) hl_alloc_free(js->base.alloc, output, output_len);
    else free(output);
    return ab;
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

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;

    /* Parse opts object if provided */
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (js_wasm_parse_opts(ctx, argv[2], &opts, JS_WOPT_IO | JS_WOPT_MEM) < 0 ||
            (want_buffer = hl_js_opt_bool(ctx, argv[2], "buffer")) < 0) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
    }

    /* per-invocation mapped spans. The sync path consumes the BORROWED request
     * array directly: hl_cap_wasm_call attaches, runs, and tears the spans down
     * within this single call. js_parse_spans JS_ToCString's the names, so they
     * outlive the call and are freed right after it (js_free_span_names, below).
     * Empty/absent -> a plain call (span_count 0). */
    HlWasmSpanReq span_reqs[HL_WASM_MAX_SPANS];
    JSValue span_holders[HL_WASM_MAX_SPANS];
    int span_count = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        span_count = js_parse_spans(ctx, argv[2], span_reqs, span_holders);
        if (span_count < 0) {
            /* audit 23: the name was leaked here. */
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
    }
    if (span_count > 0) {
        opts.spans = span_reqs;
        opts.span_count = span_count;
    }

    /* The input is resolved after the options: an options getter is app
     * code, and could close or drop the buffer a pointer taken earlier
     * named (WasmBuffer.close, a fresh mapping freed at once). */
    /* Input can be a string, ArrayBuffer, WasmBuffer, or MappedBuffer */
    size_t input_len = 0;
    const uint8_t *input = NULL;
    int input_is_string = 0;

    /* Check for WasmBuffer first */
    HlWasmBuffer *wbuf_in = JS_GetOpaque(argv[1], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        /* Check for MappedBuffer */
        HlMappedBuffer *mmap_buf = JS_GetOpaque(argv[1], js_mmap_class_id);
        if (mmap_buf && !mmap_buf->closed) {
            input = (const uint8_t *)mmap_buf->addr;
            input_len = mmap_buf->len;
        } else {
            input = js_compute_input_bytes(ctx, argv[1], &input_len,
                                           &input_is_string, "compute.call");
            if (!input) {
                js_free_span_names(ctx, span_reqs, span_holders, span_count);
                JS_FreeCString(ctx, name);
                return JS_EXCEPTION;
            }
        }
    }

    js_wasm_clamp_opts(&opts, &js->base);
    if (js_wasm_sync_prepare(ctx, js, &opts, input_len) != 0) {   /* audit 27 */
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        JS_FreeCString(ctx, name);
        js_free_span_names(ctx, span_reqs, span_holders, span_count);
        return JS_EXCEPTION;
    }

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
        js_free_span_names(ctx, span_reqs, span_holders, span_count);

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
    js_free_span_names(ctx, span_reqs, span_holders, span_count);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "compute.call: %s",
                                     err_msg ? err_msg : "unknown error");

    /* Return result as ArrayBuffer */
    return js_wasm_output_to_ab(ctx, js, output, output_len, "compute.call");
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

/* Copy an async op's input off the VM heap (the worker reads it after the
 * binding returns). A MappedBuffer / WasmBuffer input is not on the heap at
 * all, and nothing bounded the copies: one call copied a whole 1 GiB mapping
 * that the worker then refused (max_input, 1 MB by default), and N in-flight
 * calls held N copies (audit 20). So an input the call would refuse is
 * refused here, before it is copied, with the worker's own message; the copy
 * is charged 1 unit per 8 bytes, and it holds bytes on the VM's off-heap
 * total (`wasm_buf_bytes`, shared with OWNED WasmBuffers) under the heap
 * limit until the op is freed. Returns 0 with *@p out the copy (NULL for an
 * empty input), or -1 with an exception pending and nothing held. */
static int js_async_input_copy(JSContext *ctx, HlJS *js, const char *who,
                               const void *input, size_t len, uint64_t limit,
                               HlOffheapHold *hold, void **out)
{
    *out = NULL;
    if (len == 0) return 0;
    if ((uint64_t)len > limit) {
        JS_ThrowInternalError(ctx, "%s: input_too_large", who);
        return -1;
    }
    if (hl_offheap_take(hold, &js->wasm_buf_bytes, js->max_heap_bytes,
                        len) != 0) {
        hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowRangeError(ctx, "%s: input of %zu bytes would take the VM's "
                           "buffers past its heap limit (close unused "
                           "buffers)", who, len));
        return -1;
    }
    if (hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)len)) < 0) {
        hl_offheap_drop(hold);
        return -1;
    }
    void *copy = malloc(len);
    if (!copy) {
        hl_offheap_drop(hold);
        JS_ThrowInternalError(ctx, "%s: out of memory", who);
        return -1;
    }
    memcpy(copy, input, len);
    *out = copy;
    return 0;
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
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx, "compute.async requires an active event loop");
    if (hl_js_async_gate(ctx, js, "compute.async") != 0)
        return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, "compute.async.call()")) return JS_EXCEPTION;
    if (!js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.async.call: WASM runtime not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.async.call requires (name, input [, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    /* Parse opts */
    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (js_wasm_parse_opts(ctx, argv[2], &opts, JS_WOPT_IO | JS_WOPT_MEM) < 0 ||
            (want_buffer = hl_js_opt_bool(ctx, argv[2], "buffer")) < 0) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
    }

    /* The input is resolved after the options: an options getter is app
     * code, and could close or drop the buffer a pointer taken earlier
     * named (WasmBuffer.close, a fresh mapping freed at once). */
    /* Get input (string, ArrayBuffer, or WasmBuffer) */
    const uint8_t *input = NULL;
    size_t input_len = 0;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque(argv[1], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque(argv[1], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            input = js_compute_input_bytes(ctx, argv[1], &input_len,
                                           &input_is_string,
                                           "compute.async.call");
            if (!input) {
                JS_FreeCString(ctx, name);
                return JS_EXCEPTION;
            }
        }
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

    /* The input copy first: nothing is allocated yet. */
    HlOffheapHold in_hold = {0};
    void *in_copy = NULL;
    int crc = js_async_input_copy(ctx, js, "compute.async.call", input,
        input_len,
        hl_cap_wasm_input_limit(
            hl_cap_wasm_module_lookup(js->base.wasm_cache, name), NULL, &opts),
        &in_hold, &in_copy);
    if (input_is_string) JS_FreeCString(ctx, (const char *)input);
    if (crc != 0) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    /* Allocate op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op) {
        free(in_copy);
        hl_offheap_drop(&in_hold);
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

    /* The input copy (js_async_input_copy) goes with the op. */
    op->input = in_copy;
    op->input_hold = in_hold;
    /* The output, on the VM's in-flight result total (audit 29). */
    hl_offheap_shared_init(&op->result_hold, &js->async_result_bytes,
                           js->max_heap_bytes);
    op->opts.result_hold = &op->result_hold;
    op->input_len = input_len;

    JS_FreeCString(ctx, name);

    /* Parse + validate spans (owned name copies), deep-copy + submission-pin them
     * into the op, then free the parse copies. On bad input free the op (releasing
     * any pins) and throw. Empty/absent -> a plain async call. Every failure path
     * below (ctx / promise / cont / submit) frees the op, releasing the pins. */
    if (argc > 2 && JS_IsObject(argv[2])) {
        HlWasmSpanReq parsed[HL_WASM_MAX_SPANS];
        JSValue parsed_holders[HL_WASM_MAX_SPANS];
        int sn = js_parse_spans(ctx, argv[2], parsed, parsed_holders);
        if (sn < 0) {
            hl_worker_wasm_op_free(op);
            free(op);
            return JS_EXCEPTION;
        }
        hl_worker_wasm_adopt_spans(op, parsed, sn);
        js_free_span_names(ctx, parsed, parsed_holders, sn);
    }

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
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool. done_fn can't fire before we return to
     * the event loop, so the ordering with the attached suspend is
     * race-free. */
    if (hl_worker_wasm_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "compute.async.call: thread pool full");
    }

    /* Suspend the FD (attached only). */
    if (!actx->detached &&
        hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
        op->cancelled = 1;
        /* Never armed: destroy frees resolve / reject. cancel() would also
         * end the still-running handler's request life. */
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

static const JSClassDef js_wasm_inst_class = {
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

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (js_wasm_parse_opts(ctx, argv[1], &opts, JS_WOPT_IO) < 0 ||
            (want_buffer = hl_js_opt_bool(ctx, argv[1], "buffer")) < 0)
            return JS_EXCEPTION;
    }

    /* inst:call spans. Synchronous: consume the BORROWED request array directly
     * (hl_cap_wasm_instance_call attaches + tears down within this call). The
     * JS_ToCString'd names outlive the call and are freed right after it. */
    HlWasmSpanReq span_reqs[HL_WASM_MAX_SPANS];
    JSValue span_holders[HL_WASM_MAX_SPANS];
    int span_count = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        span_count = js_parse_spans(ctx, argv[1], span_reqs, span_holders);
        if (span_count < 0) return JS_EXCEPTION;
    }
    if (span_count > 0) {
        opts.spans = span_reqs;
        opts.span_count = span_count;
    }

    /* The input is resolved after the options: an options getter is app
     * code, and could close or drop the buffer a pointer taken earlier
     * named (WasmBuffer.close, a fresh mapping freed at once). */
    /* Parse input */
    size_t input_len = 0;
    const uint8_t *input = NULL;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque(argv[0], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque(argv[0], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            input = js_compute_input_bytes(ctx, argv[0], &input_len,
                                           &input_is_string,
                                           "WasmInstance.call");
            if (!input) {
                js_free_span_names(ctx, span_reqs, span_holders, span_count);
                return JS_EXCEPTION;
            }
        }
    }

    if (js) js_wasm_cap_call_opts(&opts, &js->base);
    if (js_wasm_sync_prepare(ctx, js, &opts, input_len) != 0) {   /* audit 27 */
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        js_free_span_names(ctx, span_reqs, span_holders, span_count);
        return JS_EXCEPTION;
    }

    /* Option and span getters and the input's toString are app code and can
     * close this instance, which frees it: `pi` must not be read until the
     * object is known to still hold it (a pointer compare, no dereference). */
    if (JS_GetOpaque(this_val, js_wasm_inst_class_id) != pi || pi->closed) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        js_free_span_names(ctx, span_reqs, span_holders, span_count);
        return JS_ThrowInternalError(ctx, "WasmInstance.call: instance closed");
    }

    if (want_buffer) {
        HlWasmBuffer *out_buf = NULL;
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_instance_call_buf(pi, input, input_len,
                                                &out_buf, &opts, NULL, NULL,
                                                js ? js->base.alloc : NULL, &err_msg);
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        js_free_span_names(ctx, span_reqs, span_holders, span_count);
        if (rc != 0)
            return JS_ThrowInternalError(ctx, "WasmInstance.call: %s",
                                         err_msg ? err_msg : "unknown error");
        return js_push_wasm_buffer(ctx, out_buf);
    }

    /* The output is copied onto the VM heap. Without an explicit maxOutput
     * js_wasm_sync_prepare had nothing to clamp and the cap fell to the
     * instance's own default, unclamped (audit 28): resolve the effective
     * cap here - per call, else the instance's default, else the global
     * default - after the still-open check (pi is read), then bring it to
     * the heap limit, as Lua's lua_sync_call_clamp_output does. */
    if (!opts.max_output)
        opts.max_output = pi->default_max_output ? pi->default_max_output
                                                 : HL_WASM_DEFAULT_MAX_OUTPUT;
    if (js && js->max_heap_bytes && opts.max_output > js->max_heap_bytes)
        opts.max_output = js->max_heap_bytes;

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;
    int rc = hl_cap_wasm_instance_call(pi, input, input_len,
                                        &output, &output_len,
                                        &opts, NULL, NULL,
                                        js ? js->base.alloc : NULL, &err_msg);
    if (input_is_string) JS_FreeCString(ctx, (const char *)input);
    js_free_span_names(ctx, span_reqs, span_holders, span_count);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "WasmInstance.call: %s",
                                     err_msg ? err_msg : "unknown error");

    return js_wasm_output_to_ab(ctx, js, output, output_len,
                                "WasmInstance.call");
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
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: requires an active event loop");
    if (hl_js_async_gate(ctx, js, "WasmInstance.asyncCall") != 0)
        return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, "instance.asyncCall()")) return JS_EXCEPTION;

    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (!pi || pi->closed)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance closed");
    if (atomic_load(&pi->busy))
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance busy");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WasmInstance.asyncCall requires (input [, opts])");

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (js_wasm_parse_opts(ctx, argv[1], &opts, JS_WOPT_IO) < 0 ||
            (want_buffer = hl_js_opt_bool(ctx, argv[1], "buffer")) < 0)
            return JS_EXCEPTION;
    }
    /* The input is resolved after the options: an options getter is app
     * code, and could close or drop the buffer a pointer taken earlier
     * named (WasmBuffer.close, a fresh mapping freed at once). */
    /* Parse input */
    const uint8_t *input = NULL;
    size_t input_len = 0;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque(argv[0], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque(argv[0], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            input = js_compute_input_bytes(ctx, argv[0], &input_len,
                                           &input_is_string,
                                           "WasmInstance.async.call");
            if (!input)
                return JS_EXCEPTION;
        }
    }


    /* The input's toString is app code: pi is read below (its name), so it
     * must still be this object's instance (checked again after the spans). */
    if (JS_GetOpaque(this_val, js_wasm_inst_class_id) != pi) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance closed");
    }
    js_wasm_cap_call_opts(&opts, &js->base);

    /* The input copy first: nothing is allocated yet. */
    HlOffheapHold in_hold = {0};
    void *in_copy = NULL;
    int crc = js_async_input_copy(ctx, js, "WasmInstance.asyncCall", input,
        input_len, hl_cap_wasm_input_limit(pi->module, pi, &opts), &in_hold,
        &in_copy);
    if (input_is_string) JS_FreeCString(ctx, (const char *)input);
    if (crc != 0) return JS_EXCEPTION;

    /* Allocate op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op) {
        free(in_copy);
        hl_offheap_drop(&in_hold);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
    }

    op->server = js->server;
    op->persistent_inst = pi;
    op->alloc = js->base.alloc;
    snprintf(op->name, sizeof(op->name), "%s", pi->name);
    op->opts = opts;
    op->want_buffer = want_buffer;

    /* The input copy (js_async_input_copy) goes with the op. */
    op->input = in_copy;
    op->input_hold = in_hold;
    /* The output, on the VM's in-flight result total (audit 29). */
    hl_offheap_shared_init(&op->result_hold, &js->async_result_bytes,
                           js->max_heap_bytes);
    op->opts.result_hold = &op->result_hold;
    op->input_len = input_len;

    /* Parse + validate spans, deep-copy + submission-pin into the op, free the
     * parse copies. On bad input free the op (busy not yet set) and throw. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        HlWasmSpanReq parsed[HL_WASM_MAX_SPANS];
        JSValue parsed_holders[HL_WASM_MAX_SPANS];
        int sn = js_parse_spans(ctx, argv[1], parsed, parsed_holders);
        if (sn < 0) {
            hl_worker_wasm_op_free(op);
            free(op);
            return JS_EXCEPTION;
        }
        hl_worker_wasm_adopt_spans(op, parsed, sn);
        js_free_span_names(ctx, parsed, parsed_holders, sn);
    }

    /* Option and span getters and the input's toString are app code: they
     * may have closed this instance (freeing it) or started a call on it
     * since the checks above. Only once the object still holds `pi` - a
     * pointer compare, no dereference - may pi be read. */
    if (JS_GetOpaque(this_val, js_wasm_inst_class_id) != pi
        || pi->closed || atomic_load(&pi->busy)) {
        hl_worker_wasm_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance closed or busy");
    }

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
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);

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

    if (!actx->detached &&
        hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
        /* busy stays set: the job is already on a worker, and its done /
         * cancel callback clears it (and finishes a deferred close).
         * Clearing it here let close() or GC free the instance - or a
         * second call run on its exec env - while the worker still ran. */
        op->cancelled = 1;
        /* Never armed: destroy frees resolve / reject. cancel() would also
         * end the still-running handler's request life. */
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

    hl_js_set_class_proto_frozen(ctx, js_wasm_inst_class_id, proto);
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
        if (js_wasm_parse_opts(ctx, argv[1], &opts, JS_WOPT_IO | JS_WOPT_MEM) < 0) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
    }

    js_wasm_clamp_opts(&opts, &js->base);

    /* The heap + stack it reserves, charged before (audit 10). */
    if (hl_js_budget_charge(ctx, hl_wasm_instance_units(&opts)) < 0) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

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
        HlMappedBuffer *mmap_buf = JS_GetOpaque(argv[2], js_mmap_class_id);
        if (mmap_buf && !mmap_buf->closed) {
            data = mmap_buf->addr;
            data_len = mmap_buf->len;
            pre_alloc = mmap_buf->addr;
        } else {
            /* Try ArrayBuffer */
            ab_buf = hl_js_bytes_view(ctx, &data_len, argv[2]);   /* + typed arrays (audit 29) */
            if (ab_buf) {
                data = ab_buf;
            } else {
                JS_FreeCString(ctx, module_name);
                JS_FreeCString(ctx, segment_name);
                return JS_ThrowTypeError(ctx, "compute.segment: data must be a string, ArrayBuffer, typed array, or MappedBuffer");
            }
        }
    }

    /* The cap copies the data unless it is a page-aligned MappedBuffer used
     * in place: up to HL_WASM_MAX_SEGMENT_SIZE (1 GiB) a call, charged
     * nothing (audit 21). Charged 1 unit per 8 bytes, as every bulk copy. */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    if (!(pre_alloc && data_len > 0 && data_len % (size_t)page == 0) &&
        hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)data_len))) {
        if (JS_IsString(argv[2]))
            JS_FreeCString(ctx, (const char *)data);
        JS_FreeCString(ctx, module_name);
        JS_FreeCString(ctx, segment_name);
        return JS_EXCEPTION;
    }

    /* A MappedBuffer used in place is borrowed for as long as the segment
     * uses it: without the pin, mmap.close() (or GC) unmapped memory the
     * WASM shared heap still pointed at. The cap owns the borrow from here. */
    HlMappedBuffer *pin = pre_alloc ? JS_GetOpaque(argv[2], js_mmap_class_id) : NULL;
    if (pin) hl_cap_fs_mmap_borrow(pin);
    const char *err_msg = NULL;
    int rc = hl_cap_wasm_data_load_pinned(js->base.wasm_cache, module_name,
                                    segment_name, data, data_len, pre_alloc,
                                    js->base.app_vfs,
                                    js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                    &err_msg,
                                    pin, pin ? hl_cap_fs_mmap_release : NULL);

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
    if (JS_IsException(args[0])) {
        /* The chunk could not be made (out of memory, the heap limit):
         * the callback got JS_EXCEPTION as its data (audit 27). Fail the
         * stream with that exception pending. */
        cb->error = 1;
        return -1;
    }
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

/* Per-chunk charge (audit 15): a stream runs every chunk inside this one
 * binding call, each with its own gas and timeout, so the run's budget and
 * deadline were never consulted - a stream of many chunks held the event
 * loop for hours. 1 unit per chunk plus 1 per 64 input bytes; over the
 * budget (or past the deadline) the uncatchable trip is left pending and the
 * stream stops before the chunk runs. */
typedef struct {
    JSContext *ctx;
    int        tripped;
} JsStreamBudget;

static int js_stream_before_chunk(size_t chunk_len, void *user_data)
{
    JsStreamBudget *b = (JsStreamBudget *)user_data;
    if (b->tripped) return 1;
    if (hl_js_budget_charge(b->ctx, 1 + (uint64_t)chunk_len / 64) < 0) {
        b->tripped = 1;
        return 1;
    }
    return 0;
}

/* The resolution of a file input / output, charged as fs.read / fs.write
 * charge theirs (audit 38), to the same trip flag. */
static int js_stream_fs_charge(void *user_data, uint64_t units)
{
    JsStreamBudget *b = (JsStreamBudget *)user_data;
    if (b->tripped) return 1;
    if (hl_js_budget_charge(b->ctx, units) < 0) b->tripped = 1;
    return b->tripped;
}

/* Release compute.stream's parsed arguments (every path out after parsing). */
static void js_stream_free_args(JSContext *ctx, JsStreamCbCtx *cb,
                                int has_output, HlStreamOutKind out_kind,
                                const char *in_file_str,
                                const char *out_file_str, const char *name)
{
    if (in_file_str) JS_FreeCString(ctx, in_file_str);
    if (out_file_str) JS_FreeCString(ctx, out_file_str);
    if (has_output && out_kind == HL_STREAM_OUT_CALLBACK)
        JS_FreeValue(ctx, cb->func);
    JS_FreeCString(ctx, name);
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
        /* A throwing getter / failed conversion is the call's error, not
         * "no file" with the exception left pending (audit 23). */
        if (JS_IsException(fv)) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        if (JS_IsString(fv)) {
            in_file_str = JS_ToCString(ctx, fv);
            if (!in_file_str) {
                JS_FreeValue(ctx, fv);
                JS_FreeCString(ctx, name);
                return JS_EXCEPTION;
            }
            input.kind = HL_STREAM_IN_FILE;
            input.path = in_file_str;
        }
        JS_FreeValue(ctx, fv);
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
        if (JS_IsException(fv)) {   /* audit 23 */
            if (in_file_str) JS_FreeCString(ctx, in_file_str);
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        if (JS_IsString(fv)) {
            out_file_str = JS_ToCString(ctx, fv);
            if (!out_file_str) {
                JS_FreeValue(ctx, fv);
                if (in_file_str) JS_FreeCString(ctx, in_file_str);
                JS_FreeCString(ctx, name);
                return JS_EXCEPTION;
            }
            has_output = 1;
            out_storage.kind = HL_STREAM_OUT_FILE;
            out_storage.path = out_file_str;
            out_ptr = &out_storage;
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
        int64_t v;
        int r = hl_js_opt_int64(ctx, argv[opts_idx], "chunkSize", &v);
        if (r > 0) stream_opts.chunk_size = (size_t)v;
        if (r < 0 || js_wasm_parse_opts(ctx, argv[opts_idx],
                                        &stream_opts.call_opts, JS_WOPT_MEM) < 0) {
            /* audit 23: a throwing option getter fails the call. */
            js_stream_free_args(ctx, &cb_ctx, has_output, out_storage.kind,
                                in_file_str, out_file_str, name);
            return JS_EXCEPTION;
        }
    }

    js_wasm_clamp_opts(&stream_opts.call_opts, &js->base);
    JsStreamBudget stream_budget = { ctx, 0 };
    stream_opts.before_chunk = js_stream_before_chunk;
    stream_opts.before_chunk_ud = &stream_budget;
    stream_opts.fs_charge = js_stream_fs_charge;
    stream_opts.fs_charge_ud = &stream_budget;
    /* The buffer-mode result becomes one ArrayBuffer on the VM heap. */
    stream_opts.max_output = js->max_heap_bytes;

    /* ── Resolve the input buffer (after output + opts) ──────────── */
    /* Resolved after the output's and the options' getters (app code), and
     * the bytes must outlive the output callback, which is app code too and
     * runs between chunks. A WasmBuffer / MappedBuffer is borrowed for the
     * whole stream (close() then defers the free). An ArrayBuffer is COPIED:
     * ArrayBuffer.prototype.transfer() / resize() free or move its backing
     * store and nothing pins it, so a borrowed pointer was a use-after-free
     * read on the next chunk. A string's C form is owned and immutable. */
    HlWasmBuffer   *in_wbuf = NULL;
    HlMappedBuffer *in_mmap = NULL;
    uint8_t        *in_copy = NULL;
    HlOffheapHold   in_hold = {0};
    if (input.kind != HL_STREAM_IN_FILE) {
        size_t ilen = 0;
        /* An ArrayBuffer or a typed array (audit 29: a typed array went in
         * as its toString). */
        const uint8_t *idata = hl_js_bytes_view(ctx, &ilen, argv[1]);
        if (idata) {
            /* The copy is off the heap for the whole stream, and the output
             * callback (app code, between chunks) can start another stream
             * of the same buffer: held on the VM's off-heap total and
             * charged, as the async inputs are (audit 21; Lua's image copy
             * here already was). */
            if (hl_offheap_take(&in_hold, &js->wasm_buf_bytes,
                                js->max_heap_bytes, ilen) != 0) {
                js_stream_free_args(ctx, &cb_ctx, has_output, out_storage.kind,
                                    in_file_str, out_file_str, name);
                return hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowRangeError(ctx, "compute.stream: input of %zu "
                                          "bytes would take the VM's buffers "
                                          "past its heap limit", ilen));
            }
            if (hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)ilen))) {
                hl_offheap_drop(&in_hold);
                js_stream_free_args(ctx, &cb_ctx, has_output, out_storage.kind,
                                    in_file_str, out_file_str, name);
                return JS_EXCEPTION;
            }
            in_copy = malloc(ilen ? ilen : 1);
            if (!in_copy) {
                hl_offheap_drop(&in_hold);
                js_stream_free_args(ctx, &cb_ctx, has_output, out_storage.kind,
                                    in_file_str, out_file_str, name);
                return JS_ThrowInternalError(ctx, "compute.stream: out of memory");
            }
            if (ilen) memcpy(in_copy, idata, ilen);
            input.kind = HL_STREAM_IN_BUFFER;
            input.buffer.data = in_copy;
            input.buffer.len = ilen;
        } else {
            HlWasmBuffer *wbuf = JS_GetOpaque(argv[1], js_wasm_buf_class_id);
            if (wbuf && !wbuf->closed) {
                input.kind = HL_STREAM_IN_BUFFER;
                input.buffer.data = hl_wasm_buffer_data(wbuf);
                input.buffer.len = hl_wasm_buffer_len(wbuf);
                in_wbuf = wbuf;
                hl_wasm_buffer_borrow(in_wbuf);
            } else {
                HlMappedBuffer *mmap = JS_GetOpaque(argv[1], js_mmap_class_id);
                if (mmap && !mmap->closed) {
                    input.kind = HL_STREAM_IN_BUFFER;
                    input.buffer.data = mmap->addr;
                    input.buffer.len = mmap->len;
                    in_mmap = mmap;
                    hl_cap_fs_mmap_borrow(in_mmap);
                } else {
                    /* Any other object is refused (audit 29): it went in as
                     * its toString. A primitive's runs no app code. */
                    const char *s = JS_IsObject(argv[1]) ? NULL
                                  : JS_ToCStringLen(ctx, &ilen, argv[1]);
                    if (!s) {
                        js_stream_free_args(ctx, &cb_ctx, has_output,
                                            out_storage.kind, in_file_str,
                                            out_file_str, name);
                        return JS_ThrowTypeError(ctx, "compute.stream: input must "
                            "be a string, ArrayBuffer, typed array, WasmBuffer, "
                            "MappedBuffer or {file}");
                    }
                    input.kind = HL_STREAM_IN_BUFFER;
                    input.buffer.data = s;
                    input.buffer.len = ilen;
                    input_is_string = 1;
                }
            }
        }
    }

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
    if (in_wbuf) hl_wasm_buffer_release(in_wbuf);
    if (in_mmap) hl_cap_fs_mmap_release(in_mmap);
    free(in_copy);
    hl_offheap_drop(&in_hold);
    if (input_is_string)
        JS_FreeCString(ctx, (const char *)input.buffer.data);
    js_stream_free_args(ctx, &cb_ctx, has_output, out_storage.kind,
                        in_file_str, out_file_str, name);

    /* The output callback threw: that exception is still pending - rethrow
     * it, not a new InternalError. Replacing it turned an instruction-limit
     * interrupt (uncatchable) into an error the app could catch and loop on,
     * and an app's own error into "callback_failed". */
    if (cb_ctx.error)
        return JS_EXCEPTION;
    /* The per-chunk charge tripped the budget: its interrupt is pending. */
    if (stream_budget.tripped) {
        if (out_data) hl_alloc_free(js->base.alloc, out_data, out_len);
        return JS_EXCEPTION;
    }
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

    hl_js_export_frozen(ctx, m, "compute", compute);
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
