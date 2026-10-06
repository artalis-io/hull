/*
 * js_worker.c - Per-worker JS VMs for worker.dispatch()
 *
 * Manages TLS-keyed QuickJS VMs on worker threads. Each worker gets a
 * minimal JS VM with capabilities registered via init hooks
 * (e.g. db.* from js/worker_db.c).
 *
 * Zero DB knowledge - capabilities are plugged in via hooks.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "internal.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"
#include "hull/worker_db.h"   /* hl_worker_db_end_job */

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

/* ── Worker JS VM: a runtime per thread, a context per dispatch ─────── */

/* The runtime (atoms, shapes, allocator state) is reused on its thread; the
 * CONTEXT - every global and builtin an app can touch - is created for one
 * dispatch and freed after it. A context reused across dispatches leaked
 * state between unrelated requests: a global, or a patched builtin
 * (Array.prototype.map = ...), left by one was seen by the next. */

typedef struct {
    JSRuntime *rt;
    int64_t    instructions;
    int64_t    max_instructions;   /* 0 = none */
    int        tripped;            /* sticky for the dispatch: see below */
} HlJsWorkerCtx;

static pthread_key_t  js_worker_key;
static pthread_once_t js_worker_once = PTHREAD_ONCE_INIT;

static void js_worker_destructor(void *ptr)
{
    if (!ptr) return;
    HlJsWorkerCtx *wctx = (HlJsWorkerCtx *)ptr;
    if (wctx->rt) JS_FreeRuntime(wctx->rt);
    free(wctx);
}

static void js_worker_key_create(void)
{
    pthread_key_create(&js_worker_key, js_worker_destructor);
}

/* The same instruction budget a request handler gets: without it
 * `while (true) {}` held a pool thread for good, and a few of those starved
 * every db.async / compute.async / smtp job. Sticky like the main runtime's
 * (runtime.c): once over, every poll interrupts until the next dispatch, so
 * an async function or a promise job - which turn the interrupt into a
 * rejection - cannot let the dispatch keep running. */
static int js_worker_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt;
    HlJsWorkerCtx *wctx = (HlJsWorkerCtx *)opaque;
    if (wctx->tripped)
        return 1;
    wctx->instructions += HL_JS_INTERRUPT_WEIGHT;   /* see internal.h */
    if (wctx->max_instructions > 0 &&
        wctx->instructions > wctx->max_instructions)
        wctx->tripped = 1;
    return wctx->tripped;
}

static HlJsWorkerCtx *get_js_worker_rt(void)
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
    JS_SetInterruptHandler(wctx->rt, js_worker_interrupt, wctx);
    pthread_setspecific(js_worker_key, wctx);
    return wctx;
}

/* A fresh, sandboxed context for one dispatch. */
static JSContext *js_worker_context_new(HlJsWorkerCtx *wctx,
                                        const HlJsWorkerDispatchOp *op)
{
    /* The app's limits; set per dispatch (the runtime outlives any app). */
    /* QuickJS reads a 0 limit literally (every allocation fails). */
    JS_SetMemoryLimit(wctx->rt, op->max_heap_bytes ? op->max_heap_bytes
                                                   : (size_t)-1);
    if (op->max_stack_bytes > 0)
        JS_SetMaxStackSize(wctx->rt, op->max_stack_bytes);
    JS_UpdateStackTop(wctx->rt);
    wctx->instructions = 0;
    wctx->tripped = 0;
    wctx->max_instructions = op->max_instructions;

    JSContext *ctx = JS_NewContext(wctx->rt);
    if (!ctx) return NULL;

    /* Remove dangerous globals from worker VM */
    JSValue global = JS_GetGlobalObject(ctx);
    JSAtom eval_atom = JS_NewAtom(ctx, "eval");
    JS_DeleteProperty(ctx, global, eval_atom, 0);
    JS_FreeAtom(ctx, eval_atom);
    JSAtom fn_atom = JS_NewAtom(ctx, "Function");
    JS_DeleteProperty(ctx, global, fn_atom, 0);
    JS_FreeAtom(ctx, fn_atom);
    JS_FreeValue(ctx, global);
    if (hl_js_poison_code_constructors(ctx) != 0) {
        log_error("[hull:worker] could not disable the Function constructors");
        JS_FreeContext(ctx);
        return NULL;
    }

    /* The registered hooks install `db`: only for an app that declared it. */
    if (op->with_db) {
        for (int i = 0; i < init_hook_count; i++) {
            if (init_hooks[i](ctx) != 0) {
                log_error("[hull:worker] JS init hook %d failed", i);
                JS_FreeContext(ctx);
                return NULL;
            }
        }
    }
    return ctx;
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

static void js_dispatch_run(HlJsWorkerCtx *wctx, JSContext *ctx,
                            HlJsWorkerDispatchOp *op);

/* Run the dispatch's pending promise jobs until none is left. Bounded by
 * the budget: every job runs JS, and once the budget trips every job fails
 * at its first call (QuickJS HULL PATCH 0003) without settling anything
 * that would queue more - which is also how leftovers are discarded. */
static void js_worker_drain(JSRuntime *rt)
{
    JSContext *ctx1;
    for (;;) {
        int ret = JS_ExecutePendingJob(rt, &ctx1);
        if (ret == 0) break;
        if (ret < 0 && ctx1)
            JS_FreeValue(ctx1, JS_GetException(ctx1));
    }
}

static void js_dispatch_work_fn(void *ud)
{
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)ud;

    HlJsWorkerCtx *wctx = get_js_worker_rt();
    JSContext *ctx = wctx ? js_worker_context_new(wctx, op) : NULL;
    if (!ctx) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "failed to create worker JS VM");
        return;
    }
    js_dispatch_run(wctx, ctx, op);
    /* The runtime outlives the context, and so did a job left in its queue
     * (`Promise.resolve().then(...)` the function did not wait for): it held
     * the freed context alive through its function's realm, so every such
     * dispatch leaked a whole context against the heap limit until the pool
     * thread failed every dispatch with out-of-memory. Discard them - as
     * over budget, so none runs app code - before the context goes. */
    wctx->tripped = 1;
    js_worker_drain(wctx->rt);
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeContext(ctx);
    JS_RunGC(wctx->rt);   /* cycles the dispatch left behind */
#ifdef HL_ENABLE_DB
    if (op->with_db &&
        hl_worker_db_end_job(op->error ? NULL : op->error_msg,
                             sizeof(op->error_msg)))
        op->error = 1;
#endif
}

/* Message for a failed dispatch: the error's text, or the limit. A tripped
 * run cannot run a toString. */
static void js_dispatch_fail(HlJsWorkerCtx *wctx, JSContext *ctx,
                             HlJsWorkerDispatchOp *op, JSValueConst err)
{
    op->error = 1;
    if (wctx->tripped) {
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "dispatch: interrupted (instruction limit exceeded)");
        return;
    }
    const char *msg = JS_ToCString(ctx, err);
    snprintf(op->error_msg, sizeof(op->error_msg),
             "dispatch: %s", msg ? msg : "(unknown)");
    if (msg) JS_FreeCString(ctx, msg);
    else JS_FreeValue(ctx, JS_GetException(ctx));
}

static void js_dispatch_run(HlJsWorkerCtx *wctx, JSContext *ctx,
                            HlJsWorkerDispatchOp *op)
{
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
        JSValue exc = JS_GetException(ctx);
        js_dispatch_fail(wctx, ctx, op, exc);
        JS_FreeValue(ctx, exc);
        return;
    }

    /* Run the function's own promise jobs: an async function's body past
     * its first await, a .then it chained. Its result is the settled value
     * of the promise it returned - which used to come back as a Promise
     * object (captured as {}), its jobs never run. */
    js_worker_drain(wctx->rt);
    if (wctx->tripped) {
        JS_FreeValue(ctx, result);
        js_dispatch_fail(wctx, ctx, op, JS_UNDEFINED);
        return;
    }
    JSPromiseStateEnum st = JS_PromiseState(ctx, result);
    if (st == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(ctx, result);
        js_dispatch_fail(wctx, ctx, op, reason);
        JS_FreeValue(ctx, reason);
        JS_FreeValue(ctx, result);
        return;
    }
    if (st == JS_PROMISE_PENDING) {
        /* Nothing in a worker VM can settle it later: no event loop, no
         * Hull async ops. */
        JS_FreeValue(ctx, result);
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "dispatch: the function's promise never settled (a worker "
                 "has no event loop to await anything on)");
        return;
    }
    if (st == JS_PROMISE_FULFILLED) {
        JSValue v = JS_PromiseResult(ctx, result);
        JS_FreeValue(ctx, result);
        result = v;
    }

    /* Capture the return value. Its getters run app code: one that went
     * over the budget left a partly captured result (the rest read as
     * null), which used to be reported as success (L4). */
    if (capture_result(ctx, result, op) != 0 && !wctx->tripped) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg), "out of memory");
    }
    if (wctx->tripped) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        js_dispatch_fail(wctx, ctx, op, JS_UNDEFINED);
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
