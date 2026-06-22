/*
 * timers.c — JS background timer support
 *
 * Implements `app.every()` / `app.daily()` timer callbacks. Hosts the
 * trampoline that fires from Keel's timer min-heap, drives the handler
 * (which may return a Promise for async work), and reschedules after
 * the handler completes (or self-cancels if it returned false).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/utils/alloc.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"

#include <keel/keel.h>

#include <sh_arena.h>

#include "log.h"

#include <assert.h>
#include <string.h>
#include <time.h>

/* From async.c */
extern void hl_js_async_cont_set_handler_promise(HlAsyncCont *cont,
                                                   JSContext *ctx,
                                                   JSValue promise);

int64_t hl_js_compute_daily_delay_ms(int hour, int minute, int use_local)
{
    time_t now = time(NULL);
    struct tm now_tm;
    if (use_local)
        localtime_r(&now, &now_tm);
    else
        gmtime_r(&now, &now_tm);

    int64_t now_secs = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;
    int64_t target_secs = hour * 3600 + minute * 60;

    int64_t delta = target_secs - now_secs;
    if (delta <= 0)
        delta += 86400;

    return delta * 1000;
}

int hl_js_track_timer(HlJS *js, void *timer)
{
    if (js->timer_count >= js->timer_cap) {
        size_t new_cap = js->timer_cap ? js->timer_cap * 2 : 4;
        if (new_cap < js->timer_cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1;
        size_t old_sz = js->timer_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(js->base.alloc,
                                           js->timers, old_sz, new_sz);
        if (!new_arr)
            return -1;
        js->timers = new_arr;
        js->timer_cap = new_cap;
    }
    js->timers[js->timer_count++] = timer;
    return 0;
}

void hl_js_timer_reschedule(HlJSTimer *t)
{
    HlJS *js = t->js;
    int64_t delay_ms;

    if (t->daily)
        delay_ms = hl_js_compute_daily_delay_ms(t->hour, t->minute, t->localtime);
    else
        delay_ms = t->interval_ms;

    const HlAsyncBackend *be = hl_async_backend();
    t->timer_id = (int64_t)be->timer_add(js->base.async_ctx,
                                          (uint64_t)delay_ms,
                                          hl_js_timer_trampoline, t);
    if (t->timer_id == 0)
        log_error("[hull:timer] failed to reschedule timer (handler_id=%d)",
                  t->handler_id);
}

void hl_js_timer_trampoline(void *user_data)
{
    HlJSTimer *t = (HlJSTimer *)user_data;
    HlJS *js = t->js;
    JSContext *ctx = js->ctx;

    /* Skip if previous invocation still in flight (async) */
    if (t->in_flight) {
        const HlAsyncBackend *be = hl_async_backend();
        t->timer_id = (int64_t)be->timer_add(js->base.async_ctx, 1000,
                                              hl_js_timer_trampoline, t);
        return;
    }

    t->in_flight = 1;

    /* Reset scratch arena + guard stale txn */
    sh_arena_reset(js->scratch);
    hl_db_guard_stale_txn(js->base.db_handle);

    assert(js->dispatch_depth == 0 && "timer fired during active dispatch");
    js->dispatch_depth++;

    /* Clear per-request state (no connection) */
    js->active_conn = NULL;
    js->active_req = NULL;
    js->last_async_cont = NULL;
    js->async_pending = 0;
    js->active_timer = t;

    /* Reset instruction counter */
    js->instruction_count = 0;

    /* Look up handler */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue timers = JS_GetPropertyStr(ctx, global, "__hull_timers");
    JSValue handler = JS_GetPropertyUint32(ctx, timers, (uint32_t)t->handler_id);
    JS_FreeValue(ctx, timers);
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        t->in_flight = 0;
        js->active_timer = NULL;
        hl_js_timer_reschedule(t);
        return;
    }

    /* Call handler() */
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, handler);

    if (JS_IsException(ret)) {
        JSValue exception = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exception);
        log_error("[hull:timer] %s", msg ? msg : "unknown error");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exception);
        JS_FreeValue(ctx, ret);
        t->in_flight = 0;
        js->active_timer = NULL;
        js->dispatch_depth--;
        hl_js_timer_reschedule(t);
        return;
    }

    JSPromiseStateEnum state = JS_PromiseState(ctx, ret);

    if (state == JS_PROMISE_PENDING) {
        /* Async handler — wire handler_promise on the continuation */
        if (js->last_async_cont) {
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
        }
        /* Timer ctx was already set via js->active_timer at cont creation */
        JS_FreeValue(ctx, ret);
        /* in_flight stays 1; dispatch_depth stays elevated.
         * Both cleared when async resume completes. */
        js->active_timer = NULL;
        return;
    }

    /* Synchronous completion */
    int cancelled = 0;
    if (state == JS_PROMISE_FULFILLED) {
        JSValue result = JS_PromiseResult(ctx, ret);
        if (JS_IsBool(result) && JS_ToBool(ctx, result) == 0)
            cancelled = 1;
        JS_FreeValue(ctx, result);
    } else if (state == JS_PROMISE_REJECTED) {
        JSValue result = JS_PromiseResult(ctx, ret);
        const char *msg = JS_ToCString(ctx, result);
        log_error("[hull:timer] %s", msg ? msg : "unknown error");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, result);
    } else {
        /* Sync return (not a promise) — check for false */
        if (JS_IsBool(ret) && JS_ToBool(ctx, ret) == 0)
            cancelled = 1;
    }

    /* Drain microtasks */
    hl_js_run_jobs(js);

    JS_FreeValue(ctx, ret);
    t->in_flight = 0;
    js->active_timer = NULL;
    js->dispatch_depth--;

    if (!cancelled)
        hl_js_timer_reschedule(t);
}
