/*
 * js_worker.c — Per-worker JS VMs for worker.dispatch()
 *
 * Manages TLS-keyed QuickJS VMs on worker threads. Each worker gets a
 * minimal JS VM with capabilities registered via init hooks
 * (e.g. db.* from js/worker_db.c).
 *
 * Zero DB knowledge — capabilities are plugged in via hooks.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "internal.h"
#include "hull/async.h"
#include "hull/async_backend.h"
#include "hull/net_backend.h"
#include "hull/alloc.h"

#include <keel/thread_pool.h>
#include <keel/async.h>

#include "quickjs.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "log.h"

/* ── Init hooks ────────────────────────────────────────────────────── */

#define MAX_WORKER_INIT_HOOKS 8

static HlJsWorkerInitFn init_hooks[MAX_WORKER_INIT_HOOKS];
static int              init_hook_count;

void hl_js_worker_register_init(HlJsWorkerInitFn fn)
{
    if (init_hook_count < MAX_WORKER_INIT_HOOKS)
        init_hooks[init_hook_count++] = fn;
}

/* ── Per-worker JS VM (TLS, lazy init) ─────────────────────────────── */

typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
} HlJsWorkerCtx;

static pthread_key_t  js_worker_key;
static pthread_once_t js_worker_once = PTHREAD_ONCE_INIT;

static void js_worker_destructor(void *ptr)
{
    if (!ptr) return;
    HlJsWorkerCtx *wctx = (HlJsWorkerCtx *)ptr;
    if (wctx->ctx) JS_FreeContext(wctx->ctx);
    if (wctx->rt)  JS_FreeRuntime(wctx->rt);
    free(wctx);
}

static void js_worker_key_create(void)
{
    pthread_key_create(&js_worker_key, js_worker_destructor);
}

static HlJsWorkerCtx *get_js_worker_ctx(void)
{
    pthread_once(&js_worker_once, js_worker_key_create);

    HlJsWorkerCtx *wctx = (HlJsWorkerCtx *)pthread_getspecific(js_worker_key);
    if (wctx) return wctx;

    wctx = calloc(1, sizeof(HlJsWorkerCtx));
    if (!wctx) return NULL;

    wctx->rt = JS_NewRuntime();
    if (!wctx->rt) {
        free(wctx);
        return NULL;
    }

    wctx->ctx = JS_NewContext(wctx->rt);
    if (!wctx->ctx) {
        JS_FreeRuntime(wctx->rt);
        free(wctx);
        return NULL;
    }

    /* Remove dangerous globals from worker VM */
    JSValue global = JS_GetGlobalObject(wctx->ctx);
    JSAtom eval_atom = JS_NewAtom(wctx->ctx, "eval");
    JS_DeleteProperty(wctx->ctx, global, eval_atom, 0);
    JS_FreeAtom(wctx->ctx, eval_atom);
    JSAtom fn_atom = JS_NewAtom(wctx->ctx, "Function");
    JS_DeleteProperty(wctx->ctx, global, fn_atom, 0);
    JS_FreeAtom(wctx->ctx, fn_atom);
    JS_FreeValue(wctx->ctx, global);

    /* Run registered init hooks (e.g. db.*, json.*) */
    for (int i = 0; i < init_hook_count; i++) {
        if (init_hooks[i](wctx->ctx) != 0) {
            log_error("[hull:worker] JS init hook %d failed", i);
            JS_FreeContext(wctx->ctx);
            JS_FreeRuntime(wctx->rt);
            free(wctx);
            return NULL;
        }
    }

    pthread_setspecific(js_worker_key, wctx);
    return wctx;
}

/* ── KV helpers: JS object ↔ HlKV array ──────────────────────────── */

static void push_kv_object(JSContext *ctx, JSValue obj,
                           const HlKV *kvs, int count)
{
    for (int i = 0; i < count; i++) {
        switch (kvs[i].value.type) {
        case HL_TYPE_INT:
            JS_SetPropertyStr(ctx, obj, kvs[i].key,
                              JS_NewInt64(ctx, kvs[i].value.i));
            break;
        case HL_TYPE_DOUBLE:
            JS_SetPropertyStr(ctx, obj, kvs[i].key,
                              JS_NewFloat64(ctx, kvs[i].value.d));
            break;
        case HL_TYPE_TEXT:
            JS_SetPropertyStr(ctx, obj, kvs[i].key,
                              JS_NewStringLen(ctx, kvs[i].value.s,
                                              kvs[i].value.len));
            break;
        case HL_TYPE_BOOL:
            JS_SetPropertyStr(ctx, obj, kvs[i].key,
                              JS_NewBool(ctx, kvs[i].value.b));
            break;
        case HL_TYPE_NIL:
        default:
            JS_SetPropertyStr(ctx, obj, kvs[i].key, JS_NULL);
            break;
        }
    }
}

/* Capture a JS value into the dispatch op result fields.
 * Returns 0 on success. */
static int capture_result(JSContext *ctx, JSValue val,
                          HlJsWorkerDispatchOp *op)
{
    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        op->result_kind = 0;
        return 0;
    }
    if (JS_IsBool(val)) {
        op->result_kind = 1;
        op->result_bool = JS_ToBool(ctx, val);
        return 0;
    }
    if (JS_IsNumber(val)) {
        double d;
        JS_ToFloat64(ctx, &d, val);
        /* Check if it's an integer */
        if (d == (double)(int64_t)d && d >= -9007199254740992.0 &&
            d <= 9007199254740992.0) {
            op->result_kind = 2;
            op->result_int = (int64_t)d;
        } else {
            op->result_kind = 3;
            op->result_double = d;
        }
        return 0;
    }
    if (JS_IsString(val)) {
        op->result_kind = 4;
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, val);
        if (!s) return -1;
        op->result_str = malloc(len + 1);
        if (!op->result_str) {
            JS_FreeCString(ctx, s);
            return -1;
        }
        memcpy(op->result_str, s, len);
        op->result_str[len] = '\0';
        op->result_str_len = len;
        JS_FreeCString(ctx, s);
        return 0;
    }
    if (JS_IsObject(val)) {
        op->result_kind = 5;

        /* Enumerate own string properties */
        JSPropertyEnum *tab = NULL;
        uint32_t tab_len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, val,
                                    JS_GPN_STRING_MASK |
                                    JS_GPN_ENUM_ONLY) < 0)
            return -1;

        if (tab_len == 0) {
            js_free(ctx, tab);
            op->result_kvs = NULL;
            op->result_count = 0;
            return 0;
        }

        op->result_kvs = calloc(tab_len, sizeof(HlKV));
        if (!op->result_kvs) {
            for (uint32_t i = 0; i < tab_len; i++)
                JS_FreeAtom(ctx, tab[i].atom);
            js_free(ctx, tab);
            return -1;
        }
        op->result_count = 0;

        for (uint32_t i = 0; i < tab_len; i++) {
            const char *key = JS_AtomToCString(ctx, tab[i].atom);
            if (!key) {
                JS_FreeAtom(ctx, tab[i].atom);
                continue;
            }

            int idx = op->result_count;
            op->result_kvs[idx].key = strdup(key);
            JS_FreeCString(ctx, key);
            if (!op->result_kvs[idx].key) {
                /* OOM on key dup: caller's op_free will release everything
                 * already in result_kvs (M-2). */
                JS_FreeAtom(ctx, tab[i].atom);
                for (uint32_t j = i + 1; j < tab_len; j++)
                    JS_FreeAtom(ctx, tab[j].atom);
                js_free(ctx, tab);
                return -1;
            }

            JSValue pval = JS_GetProperty(ctx, val, tab[i].atom);
            JS_FreeAtom(ctx, tab[i].atom);

            if (JS_IsBool(pval)) {
                op->result_kvs[idx].value.type = HL_TYPE_BOOL;
                op->result_kvs[idx].value.b = JS_ToBool(ctx, pval);
            } else if (JS_IsNumber(pval)) {
                double dv;
                JS_ToFloat64(ctx, &dv, pval);
                if (dv == (double)(int64_t)dv && dv >= -9007199254740992.0 &&
                    dv <= 9007199254740992.0) {
                    op->result_kvs[idx].value.type = HL_TYPE_INT;
                    op->result_kvs[idx].value.i = (int64_t)dv;
                } else {
                    op->result_kvs[idx].value.type = HL_TYPE_DOUBLE;
                    op->result_kvs[idx].value.d = dv;
                }
            } else if (JS_IsString(pval)) {
                size_t slen;
                const char *sv = JS_ToCStringLen(ctx, &slen, pval);
                op->result_kvs[idx].value.type = HL_TYPE_TEXT;
                if (sv) {
                    char *buf = malloc(slen + 1);
                    if (!buf) {
                        /* OOM on string dup: mark NIL, bump count so
                         * op_free releases the strdup'd key. */
                        op->result_kvs[idx].value.type = HL_TYPE_NIL;
                        op->result_count++;
                        JS_FreeCString(ctx, sv);
                        JS_FreeValue(ctx, pval);
                        for (uint32_t j = i + 1; j < tab_len; j++)
                            JS_FreeAtom(ctx, tab[j].atom);
                        js_free(ctx, tab);
                        return -1;
                    }
                    memcpy(buf, sv, slen);
                    buf[slen] = '\0';
                    op->result_kvs[idx].value.s = buf;
                    op->result_kvs[idx].value.len = slen;
                    JS_FreeCString(ctx, sv);
                }
            } else {
                op->result_kvs[idx].value.type = HL_TYPE_NIL;
            }
            JS_FreeValue(ctx, pval);
            op->result_count++;
        }
        js_free(ctx, tab);
        return 0;
    }

    /* Unsupported type → nil */
    op->result_kind = 0;
    return 0;
}

/* ── KlWorkItem callbacks ──────────────────────────────────────────── */

static void js_dispatch_work_fn(void *ud)
{
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)ud;

    HlJsWorkerCtx *wctx = get_js_worker_ctx();
    if (!wctx || !wctx->ctx) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "failed to create worker JS VM");
        return;
    }

    JSContext *ctx = wctx->ctx;

    /* Compile function source text: wrap in parens to get an expression.
     * Source comes from fn.toString(), e.g. "(ctx) => { ... }" or
     * "function(ctx) { ... }".  Evaluating "(<source>)" yields the function
     * value with free variables resolved against the worker VM's globals
     * (where db.*, json.* etc. are set by init hooks). */
    size_t wrap_len = 1 + op->fn_source_len + 1 + 1;  /* '(' + src + ')' + NUL */
    char *wrap = malloc(wrap_len);
    if (!wrap) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg), "out of memory");
        return;
    }
    wrap[0] = '(';
    memcpy(wrap + 1, op->fn_source, op->fn_source_len);
    wrap[1 + op->fn_source_len] = ')';
    wrap[1 + op->fn_source_len + 1] = '\0';

    JSValue eval_fn = JS_Eval(ctx, wrap, wrap_len - 1,
                               "<worker:dispatch>", JS_EVAL_TYPE_GLOBAL);
    free(wrap);

    if (JS_IsException(eval_fn)) {
        op->error = 1;
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "eval: %s", msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        return;
    }

    /* Reconstruct ctx object from HlKV array */
    JSValue ctx_obj = JS_NewObject(ctx);
    if (op->ctx_kvs && op->ctx_count > 0) {
        push_kv_object(ctx, ctx_obj, op->ctx_kvs, op->ctx_count);
    }

    /* Call fn(ctx) */
    JSValue result = JS_Call(ctx, eval_fn, JS_UNDEFINED, 1, &ctx_obj);
    JS_FreeValue(ctx, ctx_obj);
    JS_FreeValue(ctx, eval_fn);

    if (JS_IsException(result)) {
        op->error = 1;
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "dispatch: %s", msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        return;
    }

    /* Capture the return value */
    if (capture_result(ctx, result, op) != 0) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg), "out of memory");
    }
    JS_FreeValue(ctx, result);
}

static void js_dispatch_done_fn(void *ud)
{
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)ud;
    if (op->cancelled) {
        HlAsyncCtx *ctx = op->async_ctx;
        hl_js_worker_dispatch_op_free(op);
        free(op);
        if (ctx) hl_async_ctx_free(ctx);
        return;
    }
    HlAsyncCtx *ctx = op->async_ctx;
    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
}

static void js_dispatch_cancel_fn(void *ud)
{
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)ud;
    HlAsyncCtx *ctx = op->async_ctx;
    hl_js_worker_dispatch_op_free(op);
    free(op);
    if (ctx) hl_async_ctx_free(ctx);
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_js_worker_dispatch_submit(HlAsyncBackendPool *pool,
                                   HlJsWorkerDispatchOp *op)
{
    if (!pool || !op) return -1;
    const HlAsyncBackend *be = hl_async_backend();
    return be->pool_submit(pool, js_dispatch_work_fn, js_dispatch_done_fn,
                           js_dispatch_cancel_fn, op);
}

void hl_js_worker_dispatch_op_free(HlJsWorkerDispatchOp *op)
{
    if (!op) return;
    free(op->fn_source);
    hl_kv_free(op->ctx_kvs, op->ctx_count);
    op->ctx_kvs = NULL;
    free(op->result_str);
    hl_kv_free(op->result_kvs, op->result_count);
    op->result_kvs = NULL;
}

void hl_js_worker_dispatch_op_free_all(void *ptr)
{
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)ptr;
    hl_js_worker_dispatch_op_free(op);
    free(op);
}

void hl_js_worker_dispatch_cancel(KlAsyncOp *kl_op, void *user_data)
{
    (void)kl_op;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)ctx->driver;

    op->cancelled = 1;
    if (ctx->cont) {
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
    }
}
