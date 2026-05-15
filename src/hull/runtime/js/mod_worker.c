/*
 * mod_worker.c — hull:worker module (thread pool dispatch)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "internal.h"
#include "hull/worker_db.h"
#include "hull/async.h"
#include "hull/alloc.h"

#include <keel/server.h>

/* Deep-copy a JS object (own string properties only, flat) to HlKV array. */
static int js_object_to_kv(JSContext *ctx, JSValueConst obj,
                            HlKV **out_kvs, int *out_count)
{
    *out_kvs = NULL;
    *out_count = 0;

    if (JS_IsNull(obj) || JS_IsUndefined(obj))
        return 0;

    JSPropertyEnum *tab = NULL;
    uint32_t tab_len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, obj,
                                JS_GPN_STRING_MASK |
                                JS_GPN_ENUM_ONLY) < 0)
        return -1;

    if (tab_len == 0) {
        js_free(ctx, tab);
        return 0;
    }

    HlKV *kvs = calloc(tab_len, sizeof(HlKV));
    if (!kvs) {
        for (uint32_t i = 0; i < tab_len; i++)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
        return -1;
    }

    int count = 0;
    for (uint32_t i = 0; i < tab_len; i++) {
        const char *key = JS_AtomToCString(ctx, tab[i].atom);
        if (!key) {
            JS_FreeAtom(ctx, tab[i].atom);
            continue;
        }

        kvs[count].key = strdup(key);
        JS_FreeCString(ctx, key);
        if (!kvs[count].key) {
            /* OOM on key dup: release tab slot, then everything copied so
             * far, and bail (M-2). */
            JS_FreeAtom(ctx, tab[i].atom);
            for (uint32_t j = i + 1; j < tab_len; j++)
                JS_FreeAtom(ctx, tab[j].atom);
            js_free(ctx, tab);
            hl_kv_free(kvs, count);
            return -1;
        }

        JSValue val = JS_GetProperty(ctx, obj, tab[i].atom);
        JS_FreeAtom(ctx, tab[i].atom);

        if (JS_IsBool(val)) {
            kvs[count].value.type = HL_TYPE_BOOL;
            kvs[count].value.b = JS_ToBool(ctx, val);
        } else if (JS_IsNumber(val)) {
            double d;
            JS_ToFloat64(ctx, &d, val);
            if (d == (double)(int64_t)d && d >= -9007199254740992.0 &&
                d <= 9007199254740992.0) {
                kvs[count].value.type = HL_TYPE_INT;
                kvs[count].value.i = (int64_t)d;
            } else {
                kvs[count].value.type = HL_TYPE_DOUBLE;
                kvs[count].value.d = d;
            }
        } else if (JS_IsString(val)) {
            size_t slen;
            const char *sv = JS_ToCStringLen(ctx, &slen, val);
            kvs[count].value.type = HL_TYPE_TEXT;
            if (sv) {
                kvs[count].value.s = malloc(slen + 1);
                if (!kvs[count].value.s) {
                    /* OOM on string dup: mark NIL, bump count so op_free
                     * releases the already-strdup'd key. */
                    kvs[count].value.type = HL_TYPE_NIL;
                    count++;
                    JS_FreeCString(ctx, sv);
                    JS_FreeValue(ctx, val);
                    for (uint32_t j = i + 1; j < tab_len; j++)
                        JS_FreeAtom(ctx, tab[j].atom);
                    js_free(ctx, tab);
                    hl_kv_free(kvs, count);
                    return -1;
                }
                memcpy((void *)kvs[count].value.s, sv, slen);
                ((char *)kvs[count].value.s)[slen] = '\0';
                kvs[count].value.len = slen;
                JS_FreeCString(ctx, sv);
            }
        } else {
            kvs[count].value.type = HL_TYPE_NIL;
        }
        JS_FreeValue(ctx, val);
        count++;
    }
    js_free(ctx, tab);

    *out_kvs = kvs;
    *out_count = count;
    return 0;
}

/* push_result callback: convert HlJsWorkerDispatchOp result to JSValue */
static JSValue js_push_worker_dispatch_result(JSContext *ctx, void *driver)
{
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)driver;

    if (op->error) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "error",
                          JS_NewString(ctx, op->error_msg));
        return obj;
    }

    switch (op->result_kind) {
    case 0: /* nil */
        return JS_NULL;
    case 1: /* bool */
        return JS_NewBool(ctx, op->result_bool);
    case 2: /* int */
        return JS_NewInt64(ctx, op->result_int);
    case 3: /* double */
        return JS_NewFloat64(ctx, op->result_double);
    case 4: /* string */
        return JS_NewStringLen(ctx, op->result_str, op->result_str_len);
    case 5: { /* table/object */
        JSValue obj = JS_NewObject(ctx);
        for (int i = 0; i < op->result_count; i++) {
            JSValue val;
            switch (op->result_kvs[i].value.type) {
            case HL_TYPE_INT:
                val = JS_NewInt64(ctx, op->result_kvs[i].value.i);
                break;
            case HL_TYPE_DOUBLE:
                val = JS_NewFloat64(ctx, op->result_kvs[i].value.d);
                break;
            case HL_TYPE_TEXT:
                val = JS_NewStringLen(ctx, op->result_kvs[i].value.s,
                                     op->result_kvs[i].value.len);
                break;
            case HL_TYPE_BOOL:
                val = JS_NewBool(ctx, op->result_kvs[i].value.b);
                break;
            default:
                val = JS_NULL;
                break;
            }
            JS_SetPropertyStr(ctx, obj, op->result_kvs[i].key, val);
        }
        return obj;
    }
    default:
        return JS_NULL;
    }
}

/* worker.dispatch(fn, ctx) — serialize fn + ctx, submit to thread pool */
static JSValue js_worker_dispatch(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx,
            "worker.dispatch not available (no thread pool)");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx,
            "worker.dispatch can only be called from a request handler");

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "worker.dispatch requires (fn, ctx?)");

    /* Get function source text via fn.toString() */
    JSValue fn_str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(fn_str))
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: cannot serialize function");
    size_t src_len = 0;
    const char *src_cstr = JS_ToCStringLen(ctx, &src_len, fn_str);
    JS_FreeValue(ctx, fn_str);
    if (!src_cstr)
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: cannot serialize function");

    /* Deep-copy ctx object */
    HlKV *ctx_kvs = NULL;
    int ctx_count = 0;
    if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        if (js_object_to_kv(ctx, argv[1], &ctx_kvs, &ctx_count) != 0) {
            JS_FreeCString(ctx, src_cstr);
            return JS_ThrowInternalError(ctx,
                "worker.dispatch: failed to serialize ctx object");
        }
    }

    /* Allocate dispatch op */
    HlJsWorkerDispatchOp *op = calloc(1, sizeof(HlJsWorkerDispatchOp));
    if (!op) {
        JS_FreeCString(ctx, src_cstr);
        hl_kv_free(ctx_kvs, ctx_count);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }

    op->server = js->server;
    op->alloc = js->base.alloc;
    /* Copy source text (JS_ToCStringLen returns js_malloc'd memory) */
    op->fn_source = malloc(src_len + 1);
    if (!op->fn_source) {
        JS_FreeCString(ctx, src_cstr);
        hl_kv_free(ctx_kvs, ctx_count);
        free(op);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }
    memcpy(op->fn_source, src_cstr, src_len);
    op->fn_source[src_len] = '\0';
    op->fn_source_len = src_len;
    JS_FreeCString(ctx, src_cstr);

    op->ctx_kvs = ctx_kvs;
    op->ctx_count = ctx_count;

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.alloc);
    if (!actx) {
        hl_js_worker_dispatch_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_js_worker_dispatch_op_free(op);
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
                                                  js_push_worker_dispatch_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_js_worker_dispatch_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_js_worker_dispatch_op_free_all;
    actx->op.on_cancel = hl_js_worker_dispatch_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_js_worker_dispatch_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_js_worker_dispatch_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: thread pool full");
    }

    /* Suspend the connection */
    if (kl_async_suspend(js->server, js->active_conn, &actx->op) < 0) {
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: failed to suspend connection");
    }

    return promise;
}

static int js_worker_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue worker = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, worker, "dispatch",
                      JS_NewCFunction(ctx, js_worker_dispatch, "dispatch", 2));
    JS_SetModuleExport(ctx, m, "worker", worker);
    return 0;
}

int hl_js_init_worker_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:worker", js_worker_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "worker");
    return 0;
}
