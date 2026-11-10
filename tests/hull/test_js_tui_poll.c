/* Fault injection against the actual TUI binding, with real QuickJS promises.
 * Terminal IO, timers and continuation plumbing are isolated from the host. */
#include "utest.h"
#ifndef HL_ENABLE_TUI
#define HL_ENABLE_TUI 1
#endif
#include "../../src/hull/runtime/js/mod_tui.c"

static int timer_calls, fail_timer, live_contexts, live_conts;
static HlAsyncTimerFn pending_tick;
static void *pending_user;
static uint64_t test_now(void) { return 100; }
static uint64_t test_timer(HlAsyncBackendCtx *ctx, uint64_t ms,
                           HlAsyncTimerFn fn, void *user)
{
    (void)ctx; (void)ms;
    if (++timer_calls == fail_timer) return 0;
    pending_tick = fn; pending_user = user;
    return (uint64_t)timer_calls;
}
static uint64_t cancelled_timer;
static void test_timer_cancel(HlAsyncBackendCtx *ctx, uint64_t handle)
{ (void)ctx; cancelled_timer = handle; }
static const HlAsyncBackend test_backend = {
    .monotonic_ms = test_now, .timer_add = test_timer,
    .timer_cancel = test_timer_cancel,
};
const HlAsyncBackend *hl_async_backend(void) { return &test_backend; }
int hl_js_async_gate(JSContext *ctx, HlJS *js, const char *what)
{ (void)ctx; (void)js; (void)what; return 0; }
#ifdef HL_ENABLE_DB
const char *hl_db_registry_open_txn(HlDbRegistry *reg)
{ (void)reg; return NULL; }
#endif
/* The run: hl_js_budget_charge says "over" (and leaves the interrupt
 * pending) once `run_stop_after` charges have been asked; 0 = never. */
static int run_charges, run_stop_after;
int hl_js_budget_charge(JSContext *ctx, uint64_t units)   /* audit 40 */
{
    (void)units;
    run_charges++;
    if (run_stop_after && run_charges >= run_stop_after) {
        JS_ThrowInternalError(ctx, "run exceeded its time limit");
        return -1;
    }
    return 0;
}
JSValue hl_js_budget_throw(JSContext *ctx)
{ return JS_ThrowInternalError(ctx, "run exceeded its time limit"); }
void hl_js_budget_arm_kind(HlJS *js, HlRunKind kind) { (void)js; (void)kind; }
int hl_cap_tui_acquire(HlTuiCtx **out) { (void)out; return -1; }
/* Every poll times out (no input), recording the longest wait asked for;
 * -1 is "wait for ever". */
static int poll_calls, poll_max_timeout, poll_unbounded;
int hl_cap_tui_poll(HlTuiCtx *ctx, int timeout, HlTuiEvent *out)
{
    (void)ctx; (void)out;
    poll_calls++;
    if (timeout < 0) { poll_unbounded++; return 1; }
    if (timeout > poll_max_timeout) poll_max_timeout = timeout;
    return 1;
}
int hl_cap_tui_drain(HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_next_event(HlTuiCtx *ctx, HlTuiEvent *out)
{ (void)ctx; (void)out; return 1; }
int hl_cap_tui_parser_pending(HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_commit_idle(HlTuiCtx *ctx) { (void)ctx; return 0; }

typedef struct {
    HlAsyncCont base;
    HlJS *js;
    JSValue resolve, reject;
    JSValue (*push_result)(JSContext *, void *);
} TestCont;
static void test_resume(HlAsyncCont *self, void *driver)
{
    TestCont *cont = (TestCont *)self;
    JSValue result = cont->push_result(cont->js->ctx, driver);
    JSValue value = JS_Call(cont->js->ctx, cont->resolve, JS_UNDEFINED, 1, &result);
    JS_FreeValue(cont->js->ctx, result);
    JS_FreeValue(cont->js->ctx, value);
}
static void test_destroy(HlAsyncCont *self)
{
    TestCont *cont = (TestCont *)self;
    JS_FreeValue(cont->js->ctx, cont->resolve);
    JS_FreeValue(cont->js->ctx, cont->reject);
    cont->js->last_async_cont = NULL;
    live_conts--; free(cont);
}
HlAsyncCont *hl_js_async_cont_create(HlJS *js, JSValue resolve, JSValue reject,
    HlAllocator *alloc, JSValue (*push_result)(JSContext *, void *))
{
    (void)alloc;
    TestCont *cont = calloc(1, sizeof *cont);
    if (!cont) return NULL;
    cont->base.resume = test_resume; cont->base.destroy = test_destroy;
    cont->js = js; cont->resolve = resolve; cont->reject = reject;
    cont->push_result = push_result;
    js->last_async_cont = cont; live_conts++;
    return &cont->base;
}
HlAsyncCtx *hl_async_ctx_create(KlHttpServer *server, HlNetBackendCtx *net,
                               HlAllocator *alloc)
{
    (void)server; (void)net; (void)alloc;
    HlAsyncCtx *ctx = calloc(1, sizeof *ctx);
    if (ctx) live_contexts++;
    return ctx;
}
static HlAsyncCtx *tracked;
void hl_async_ctx_track(HlAsyncCtx *ctx, HlAsyncTeardown **list)
{ (void)list; tracked = ctx; }
void hl_async_ctx_free(HlAsyncCtx *ctx)
{
    if (ctx == tracked) tracked = NULL;
    live_contexts--; free(ctx);
}
void hl_async_ctx_resume_detached(HlAsyncCtx *ctx)
{
    ctx->cont->resume(ctx->cont, ctx->driver);
    ctx->free_driver(ctx->driver);
    ctx->cont->destroy(ctx->cont);
    hl_async_ctx_free(ctx);
}
static void setup(HlJS *js, int failure)
{
    memset(js, 0, sizeof *js);
    js->rt = JS_NewRuntime(); js->ctx = JS_NewContext(js->rt);
    JS_SetContextOpaque(js->ctx, js);
    js->base.async_ctx = (HlAsyncBackendCtx *)(uintptr_t)1;
    g_ctx = (HlTuiCtx *)(uintptr_t)1;
    timer_calls = 0; fail_timer = failure;
    live_contexts = 0; live_conts = 0;
    pending_tick = NULL; pending_user = NULL;
    tracked = NULL; cancelled_timer = 0;
    run_charges = 0; run_stop_after = 0;
    poll_calls = 0; poll_max_timeout = 0; poll_unbounded = 0;
}
static void teardown(HlJS *js)
{
    g_ctx = NULL;
    JS_FreeContext(js->ctx); JS_FreeRuntime(js->rt);
}
UTEST(js_tui_poll, initial_timer_failure_unwinds)
{
    HlJS js; setup(&js, 1);
    JSValue timeout = JS_NewInt32(js.ctx, 100);
    JSValue result = js_tui_poll(js.ctx, JS_UNDEFINED, 1, &timeout);
    EXPECT_TRUE(JS_IsException(result));
    JSValue error = JS_GetException(js.ctx);
    const char *message = JS_ToCString(js.ctx, error);
    EXPECT_TRUE(message && strstr(message, "could not arm a timer"));
    JS_FreeCString(js.ctx, message); JS_FreeValue(js.ctx, error);
    JS_FreeValue(js.ctx, timeout);
    EXPECT_EQ(live_contexts, 0); EXPECT_EQ(live_conts, 0);
    EXPECT_EQ(js.last_async_cont, NULL);
    EXPECT_TRUE(pending_tick == NULL);
    teardown(&js);
}
UTEST(js_tui_poll, reschedule_failure_settles_timeout)
{
    HlJS js; setup(&js, 2);
    JSValue timeout = JS_NewInt32(js.ctx, 100);
    JSValue promise = js_tui_poll(js.ctx, JS_UNDEFINED, 1, &timeout);
    ASSERT_FALSE(JS_IsException(promise));
    EXPECT_EQ(JS_PromiseState(js.ctx, promise), JS_PROMISE_PENDING);
    ASSERT_TRUE(pending_tick != NULL);
    pending_tick(pending_user);
    EXPECT_EQ(timer_calls, 2);
    EXPECT_EQ(JS_PromiseState(js.ctx, promise), JS_PROMISE_FULFILLED);
    JSValue result = JS_PromiseResult(js.ctx, promise);
    EXPECT_TRUE(JS_IsNull(result));
    EXPECT_EQ(live_contexts, 0); EXPECT_EQ(live_conts, 0);
    EXPECT_EQ(js.last_async_cont, NULL);
    JS_FreeValue(js.ctx, result); JS_FreeValue(js.ctx, promise);
    JS_FreeValue(js.ctx, timeout);
    teardown(&js);
}
/* Audit 14 G1: a poll still pending when the runtime shuts down is on the
 * teardown list, and stopping it disarms its timer before the continuation is
 * released - nothing fires into freed memory, and QuickJS frees cleanly. */
UTEST(js_tui_poll, pending_poll_is_stopped_at_teardown)
{
    HlJS js; setup(&js, 0);
    JSValue timeout = JS_NewInt32(js.ctx, 100);
    JSValue promise = js_tui_poll(js.ctx, JS_UNDEFINED, 1, &timeout);
    ASSERT_FALSE(JS_IsException(promise));
    ASSERT_TRUE(tracked != NULL);
    ASSERT_TRUE(tracked->on_shutdown != NULL);
    HlAsyncCtx *c = tracked;
    c->on_shutdown(c, js.base.async_ctx);
    EXPECT_EQ(cancelled_timer, (uint64_t)1);
    /* What hl_async_ctx_cancel does next. */
    c->free_driver(c->driver);
    c->cont->destroy(c->cont);
    hl_async_ctx_free(c);
    EXPECT_EQ(live_contexts, 0); EXPECT_EQ(live_conts, 0);
    EXPECT_EQ(JS_PromiseState(js.ctx, promise), JS_PROMISE_PENDING);
    JS_FreeValue(js.ctx, promise);
    JS_FreeValue(js.ctx, timeout);
    teardown(&js);
}
/* Audit 44: with no event loop wired (hull test / hull agent build their app
 * with no async ctx) tui.poll fell back to ONE blocking cap poll with the
 * caller's timeout - -1 waited for ever - and never asked the run, so the
 * run deadline could not end it. It now waits in slices, asking the run
 * after each, and raises the run's limit once it says stop. So does
 * tui.pollSync outside app.main's own run. */
UTEST(tui_audit44, blocking_poll_honours_deadline)
{
    HlJS js; setup(&js, 0);
    js.base.async_ctx = NULL;          /* no event loop: the blocking path */
    run_stop_after = 3;
    JSValue forever = JS_NewInt32(js.ctx, -1);
    JSValue result = js_tui_poll(js.ctx, JS_UNDEFINED, 1, &forever);
    EXPECT_TRUE(JS_IsException(result));
    EXPECT_EQ(poll_unbounded, 0);
    EXPECT_GT(poll_calls, 1);
    EXPECT_LE(poll_max_timeout, 50);
    JSValue err = JS_GetException(js.ctx);
    JS_FreeValue(js.ctx, err);

    /* A bounded poll that runs out first is a timeout (null). */
    run_charges = 0; run_stop_after = 0; poll_calls = 0;
    JSValue small = JS_NewInt32(js.ctx, 120);
    result = js_tui_poll(js.ctx, JS_UNDEFINED, 1, &small);
    EXPECT_TRUE(JS_IsNull(result));
    EXPECT_EQ(poll_unbounded, 0);

    /* pollSync outside app.main's own run (a timer, a ws callback). */
    run_charges = 0; run_stop_after = 3; poll_calls = 0; poll_unbounded = 0;
    js.active_cli_main = 0;
    result = js_tui_poll_sync(js.ctx, JS_UNDEFINED, 1, &forever);
    EXPECT_TRUE(JS_IsException(result));
    EXPECT_EQ(poll_unbounded, 0);
    err = JS_GetException(js.ctx);
    JS_FreeValue(js.ctx, err);
    teardown(&js);
}

UTEST_MAIN();
