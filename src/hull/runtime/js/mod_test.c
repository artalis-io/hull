/*
 * runtime/js/mod_test.c — JavaScript test bindings
 *
 * JS-specific test registration, HTTP dispatch wrappers, and assertions.
 * Uses the shared pure-C dispatch helper (cap/test.c::hl_cap_test_dispatch);
 * its source moved from cap/ to runtime/js/ as part of architectural
 * roadmap item E (restore the cap-layer "no runtime knowledge" invariant).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_JS

#include "hull/cap/test.h"
#include "hull/runtime/test.h"
#include "hull/runtime/js.h"
#include "hull/alloc.h"
#include "hull/async_backend.h"

#include <keel/request.h>

#include "quickjs.h"

#include <string.h>

/* JS test state stored as opaque pointer on globalThis.__hull_test_state */

typedef struct {
    KlRouter *router;
    HlJS *js;
    JSValue cases; /* Array of { desc: string, fn: function } */
} HlJSTestState;

static HlJSTestState *get_js_test_state(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue state_val = JS_GetPropertyStr(ctx, global, "__hull_test_state");
    HlJSTestState *state = NULL;
    if (!JS_IsUndefined(state_val)) {
        state = JS_GetOpaque(state_val, 0);
    }
    JS_FreeValue(ctx, state_val);
    JS_FreeValue(ctx, global);
    return state;
}

/* Default per-test timeout in milliseconds. Tests that need more time
 * (real I/O, heavy compute) pass an `opts.timeout` override. Five seconds
 * matches the GPU dispatch timeout and is plenty for synthetic-dispatch
 * assertions that settle in <100ms in practice. */
#define HL_JS_TEST_DEFAULT_TIMEOUT_MS 5000

/* test(desc, fn)                 — common case: default timeout
 * test(desc, opts, fn)           — opts may carry { timeout: N_ms }
 *
 * The body may be sync or async. Async bodies' returned promises are
 * driven to settlement by hl_js_test_run; rejected promises (which is
 * how a failing `await test.eq(...)` surfaces) mark the test as FAIL.
 * Before the runner became Promise-aware (May 2026) every async test
 * passed regardless of its assertions — see test_async_runner.c for
 * the regression coverage. */
static JSValue js_test_call(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return JS_ThrowInternalError(ctx, "test not initialized");

    /* Distinguish the two call shapes by inspecting argv[1]. If it's
     * callable, we're in the 2-arg form (desc, fn). Otherwise we
     * expect (desc, opts, fn) — opts may be any object; missing fields
     * fall back to defaults. */
    JSValueConst fn_arg;
    int32_t timeout_ms = HL_JS_TEST_DEFAULT_TIMEOUT_MS;

    if (JS_IsFunction(ctx, argv[1])) {
        fn_arg = argv[1];
    } else if (argc >= 3 && JS_IsFunction(ctx, argv[2])) {
        fn_arg = argv[2];
        if (JS_IsObject(argv[1]) && !JS_IsNull(argv[1])) {
            JSValue t_val = JS_GetPropertyStr(ctx, argv[1], "timeout");
            if (!JS_IsUndefined(t_val))
                JS_ToInt32(ctx, &timeout_ms, t_val);
            JS_FreeValue(ctx, t_val);
            if (timeout_ms <= 0) timeout_ms = HL_JS_TEST_DEFAULT_TIMEOUT_MS;
        }
    } else {
        return JS_ThrowTypeError(ctx,
            "test() requires (desc, fn) or (desc, opts, fn) — "
            "the last argument must be the test function");
    }

    JSValue len_val = JS_GetPropertyStr(ctx, state->cases, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "desc", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, entry, "fn",   JS_DupValue(ctx, fn_arg));
    JS_SetPropertyStr(ctx, entry, "timeout_ms", JS_NewInt32(ctx, timeout_ms));
    JS_SetPropertyUint32(ctx, state->cases, (uint32_t)idx, entry);

    return JS_UNDEFINED;
}

/* test.get/post/put/delete/patch */
static JSValue js_test_http(JSContext *ctx, const char *method,
                            int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "path required");

    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return JS_ThrowInternalError(ctx, "test not initialized");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *body_str = NULL;
    size_t body_len = 0;
    const char *header_names[KL_MAX_HEADERS];
    const char *header_values[KL_MAX_HEADERS];
    int num_headers = 0;
    const char *ctx_json = NULL;
    int run_middleware = 0;

    /* Parse opts (arg 1) */
    if (argc >= 2 && JS_IsObject(argv[1])) {
        /* opts.middleware */
        JSValue mw_val = JS_GetPropertyStr(ctx, argv[1], "middleware");
        if (JS_ToBool(ctx, mw_val))
            run_middleware = 1;
        JS_FreeValue(ctx, mw_val);
        JSValue body_val = JS_GetPropertyStr(ctx, argv[1], "body");
        if (JS_IsString(body_val))
            body_str = JS_ToCStringLen(ctx, &body_len, body_val);
        JS_FreeValue(ctx, body_val);

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hdrs_val)) {
            JSPropertyEnum *props = NULL;
            uint32_t prop_count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, hdrs_val,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < prop_count && num_headers < KL_MAX_HEADERS; i++) {
                    header_names[num_headers] = JS_AtomToCString(ctx, props[i].atom);
                    JSValue val = JS_GetProperty(ctx, hdrs_val, props[i].atom);
                    header_values[num_headers] = JS_ToCString(ctx, val);
                    JS_FreeValue(ctx, val);
                    if (header_names[num_headers] && header_values[num_headers])
                        num_headers++;
                }
                for (uint32_t i = 0; i < prop_count; i++)
                    JS_FreeAtom(ctx, props[i].atom);
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, hdrs_val);

        /* opts.ctx — JSON-stringify to pass through as req.ctx */
        JSValue ctx_val = JS_GetPropertyStr(ctx, argv[1], "ctx");
        if (JS_IsObject(ctx_val)) {
            JSValue json_val = JS_JSONStringify(ctx, ctx_val, JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsString(json_val))
                ctx_json = JS_ToCString(ctx, json_val);
            JS_FreeValue(ctx, json_val);
        }
        JS_FreeValue(ctx, ctx_val);
    }

    HlTestResult result;
    int rc = hl_cap_test_dispatch(state->router, method, path, body_str, body_len,
                           header_names, header_values, num_headers,
                           ctx_json, state->js->base.alloc, run_middleware,
                           &result);

    /* Free C strings */
    // cppcheck-suppress knownConditionTrueFalse
    if (ctx_json) JS_FreeCString(ctx, ctx_json);
    // cppcheck-suppress knownConditionTrueFalse
    if (body_str) JS_FreeCString(ctx, body_str);
    // cppcheck-suppress knownConditionTrueFalse
    for (int i = 0; i < num_headers; i++) {
        if (header_names[i]) JS_FreeCString(ctx, header_names[i]);
        if (header_values[i]) JS_FreeCString(ctx, header_values[i]);
    }
    JS_FreeCString(ctx, path);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "test dispatch failed");

    /* Build result object */
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, result.status));

    if (result.body && result.body_len > 0) {
        JS_SetPropertyStr(ctx, obj, "body",
                          JS_NewStringLen(ctx, result.body, result.body_len));

        /* Auto-decode JSON */
        if (result.body[0] == '{' || result.body[0] == '[') {
            JSValue json_str = JS_NewStringLen(ctx, result.body, result.body_len);
            JSValue parsed = JS_ParseJSON(ctx, result.body, result.body_len, "<test>");
            if (!JS_IsException(parsed))
                JS_SetPropertyStr(ctx, obj, "json", parsed);
            else
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, json_str);
        }

        hl_alloc_free(state->js->base.alloc, (void *)result.body,
                      result.body_len + 1);
    }

    /* Parse response headers into an object */
    if (result.hdr_buf && result.hdr_len > 0) {
        JSValue hdrs = JS_NewObject(ctx);
        const char *p = result.hdr_buf;
        const char *end = p + result.hdr_len;
        while (p < end) {
            const char *colon = memchr(p, ':', (size_t)(end - p));
            if (!colon) break;
            const char *eol = memchr(colon, '\r', (size_t)(end - colon));
            if (!eol) eol = memchr(colon, '\n', (size_t)(end - colon));
            if (!eol) eol = end;
            const char *val = colon + 1;
            while (val < eol && *val == ' ') val++;
            /* Lowercase header name */
            size_t name_len = (size_t)(colon - p);
            char lower[256];
            if (name_len < sizeof(lower)) {
                for (size_t i = 0; i < name_len; i++)
                    lower[i] = (char)(p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]);
                lower[name_len] = '\0';
                JS_SetPropertyStr(ctx, hdrs, lower,
                    JS_NewStringLen(ctx, val, (size_t)(eol - val)));
            }
            p = eol;
            while (p < end && (*p == '\r' || *p == '\n')) p++;
        }
        JS_SetPropertyStr(ctx, obj, "headers", hdrs);
        hl_alloc_free(state->js->base.alloc, (void *)result.hdr_buf,
                      result.hdr_len + 1);
    }

    return obj;
}

static JSValue js_test_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "GET", argc, argv);
}

static JSValue js_test_post(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "POST", argc, argv);
}

static JSValue js_test_put(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "PUT", argc, argv);
}

static JSValue js_test_del(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "DELETE", argc, argv);
}

static JSValue js_test_patch(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "PATCH", argc, argv);
}

/* test.eq(a, b, msg?) */
static JSValue js_test_eq(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "test.eq requires 2 arguments");

    int equal = 0;

    /* Use strict equality for primitives */
    if (JS_IsNumber(argv[0]) && JS_IsNumber(argv[1])) {
        double a, b;
        JS_ToFloat64(ctx, &a, argv[0]);
        JS_ToFloat64(ctx, &b, argv[1]);
        equal = (a == b);
    } else if (JS_IsString(argv[0]) && JS_IsString(argv[1])) {
        const char *a = JS_ToCString(ctx, argv[0]);
        const char *b = JS_ToCString(ctx, argv[1]);
        equal = (a && b && strcmp(a, b) == 0);
        if (a) JS_FreeCString(ctx, a);
        if (b) JS_FreeCString(ctx, b);
    } else if (JS_IsBool(argv[0]) && JS_IsBool(argv[1])) {
        equal = (JS_ToBool(ctx, argv[0]) == JS_ToBool(ctx, argv[1]));
    } else if (JS_IsNull(argv[0]) && JS_IsNull(argv[1])) {
        equal = 1;
    } else if (JS_IsUndefined(argv[0]) && JS_IsUndefined(argv[1])) {
        equal = 1;
    } else {
        /* Reference equality for objects */
        equal = (JS_VALUE_GET_PTR(argv[0]) == JS_VALUE_GET_PTR(argv[1]));
    }

    if (!equal) {
        const char *msg = (argc >= 3 && JS_IsString(argv[2]))
                          ? JS_ToCString(ctx, argv[2]) : NULL;
        JSValue a_str = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
        JSValue b_str = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
        const char *a = JS_ToCString(ctx, a_str);
        const char *b = JS_ToCString(ctx, b_str);

        JSValue err;
        if (msg)
            err = JS_ThrowTypeError(ctx, "test.eq failed: %s\n  expected: %s\n  actual:   %s",
                                    msg, b ? b : "?", a ? a : "?");
        else
            err = JS_ThrowTypeError(ctx, "test.eq failed\n  expected: %s\n  actual:   %s",
                                    b ? b : "?", a ? a : "?");

        if (a) JS_FreeCString(ctx, a);
        if (b) JS_FreeCString(ctx, b);
        JS_FreeValue(ctx, a_str);
        JS_FreeValue(ctx, b_str);
        if (msg) JS_FreeCString(ctx, msg);
        return err;
    }

    return JS_UNDEFINED;
}

/* test.ok(val, msg?) */
static JSValue js_test_ok(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_ToBool(ctx, argv[0])) {
        const char *msg = (argc >= 2 && JS_IsString(argv[1]))
                          ? JS_ToCString(ctx, argv[1]) : "test.ok failed";
        JSValue err = JS_ThrowTypeError(ctx, "%s", msg);
        if (argc >= 2 && JS_IsString(argv[1]))
            JS_FreeCString(ctx, msg);
        return err;
    }
    return JS_UNDEFINED;
}

/* test.err(fn, pattern?) */
static JSValue js_test_err(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "test.err requires a function");

    JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    if (!JS_IsException(ret)) {
        JS_FreeValue(ctx, ret);
        return JS_ThrowTypeError(ctx, "test.err: expected error but function succeeded");
    }

    JSValue exc = JS_GetException(ctx);
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char *pattern = JS_ToCString(ctx, argv[1]);
        JSValue msg_val = JS_GetPropertyStr(ctx, exc, "message");
        const char *msg = JS_ToCString(ctx, msg_val);

        int matches = (msg && pattern && strstr(msg, pattern) != NULL);

        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, msg_val);
        JS_FreeCString(ctx, pattern);

        if (!matches) {
            JS_FreeValue(ctx, exc);
            return JS_ThrowTypeError(ctx, "test.err: error does not match pattern");
        }
    }

    JS_FreeValue(ctx, exc);
    return JS_UNDEFINED;
}

void hl_js_test_register(JSContext *ctx, KlRouter *router, HlJS *js)
{
    HlJSTestState *state = js_malloc(ctx, sizeof(HlJSTestState));
    if (!state) return;

    state->router = router;
    state->js = js;
    state->cases = JS_NewArray(ctx);

    /* Store state on global */
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue state_obj = JS_NewObjectClass(ctx, 0);
    JS_SetOpaque(state_obj, state);
    JS_SetPropertyStr(ctx, global, "__hull_test_state", state_obj);

    /* Create test function (callable + has properties) */
    JSValue test_fn = JS_NewCFunction(ctx, js_test_call, "test", 2);

    /* Add methods */
    JS_SetPropertyStr(ctx, test_fn, "get",
                      JS_NewCFunction(ctx, js_test_get, "get", 2));
    JS_SetPropertyStr(ctx, test_fn, "post",
                      JS_NewCFunction(ctx, js_test_post, "post", 2));
    JS_SetPropertyStr(ctx, test_fn, "put",
                      JS_NewCFunction(ctx, js_test_put, "put", 2));
    JS_SetPropertyStr(ctx, test_fn, "delete",
                      JS_NewCFunction(ctx, js_test_del, "delete", 2));
    JS_SetPropertyStr(ctx, test_fn, "patch",
                      JS_NewCFunction(ctx, js_test_patch, "patch", 2));
    JS_SetPropertyStr(ctx, test_fn, "eq",
                      JS_NewCFunction(ctx, js_test_eq, "eq", 3));
    JS_SetPropertyStr(ctx, test_fn, "ok",
                      JS_NewCFunction(ctx, js_test_ok, "ok", 2));
    JS_SetPropertyStr(ctx, test_fn, "err",
                      JS_NewCFunction(ctx, js_test_err, "err", 2));

    JS_SetPropertyStr(ctx, global, "test", test_fn);
    JS_FreeValue(ctx, global);
}

void hl_js_test_free(JSContext *ctx)
{
    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return;
    JS_FreeValue(ctx, state->cases);
    state->cases = JS_UNDEFINED;
    js_free(ctx, state);
}

void hl_js_test_clear(JSContext *ctx)
{
    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return;

    JS_FreeValue(ctx, state->cases);
    state->cases = JS_NewArray(ctx);
}

/* Record a FAIL into the writer + results array, with formatted error. */
static void record_fail(FILE *out, HlTestCaseResult *results, int max_results,
                        int idx, const char *desc, const char *msg)
{
    if (out)
        fprintf(out, "  FAIL  %s\n    %s\n",
                desc ? desc : "(unnamed)",
                msg  ? msg  : "unknown error");
    if (results && idx < max_results) {
        snprintf(results[idx].name, sizeof(results[idx].name),
                 "%s", desc ? desc : "(unnamed)");
        results[idx].passed = 0;
        snprintf(results[idx].error, sizeof(results[idx].error),
                 "%s", msg ? msg : "unknown error");
    }
}

static void record_pass(FILE *out, HlTestCaseResult *results, int max_results,
                        int idx, const char *desc)
{
    if (out)
        fprintf(out, "  PASS  %s\n", desc ? desc : "(unnamed)");
    if (results && idx < max_results) {
        snprintf(results[idx].name, sizeof(results[idx].name),
                 "%s", desc ? desc : "(unnamed)");
        results[idx].passed = 1;
        results[idx].error[0] = '\0';
    }
}

/* Extract a human-readable error message from a JS exception value
 * (or any value coerced to a string). Caller owns the JSValue copy
 * but not the C string — buffer must outlive use. */
static void extract_error_string(JSContext *ctx, JSValueConst err,
                                 char *buf, size_t buf_size)
{
    /* Prefer .message; fall back to String(err). */
    JSValue msg_val = JS_GetPropertyStr(ctx, err, "message");
    const char *msg = NULL;
    if (!JS_IsUndefined(msg_val) && !JS_IsNull(msg_val))
        msg = JS_ToCString(ctx, msg_val);
    if (!msg)
        msg = JS_ToCString(ctx, err);
    snprintf(buf, buf_size, "%s", msg ? msg : "unknown error");
    if (msg) JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, msg_val);
}

void hl_js_test_run(JSContext *ctx, int *total, int *passed, int *failed,
                        FILE *out, HlTestCaseResult *results, int max_results)
{
    /* All three count pointers are required (callers always pass &locals). */
    if (!total || !passed || !failed)
        return;
    *total = 0;
    *passed = 0;
    *failed = 0;

    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return;

    HlJS *js = state->js;
    HlRuntime *rt = js ? (HlRuntime *)js : NULL;
    HlAsyncBackendCtx *async_ctx = rt ? rt->async_ctx : NULL;
    const HlAsyncBackend *backend = hl_async_backend();

    JSValue len_val = JS_GetPropertyStr(ctx, state->cases, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < count; i++) {
        JSValue entry = JS_GetPropertyUint32(ctx, state->cases, (uint32_t)i);
        JSValue desc_val    = JS_GetPropertyStr(ctx, entry, "desc");
        JSValue fn          = JS_GetPropertyStr(ctx, entry, "fn");
        JSValue timeout_val = JS_GetPropertyStr(ctx, entry, "timeout_ms");

        const char *desc = JS_ToCString(ctx, desc_val);
        int32_t timeout_ms = HL_JS_TEST_DEFAULT_TIMEOUT_MS;
        JS_ToInt32(ctx, &timeout_ms, timeout_val);

        int idx = *total;
        (*total)++;

        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        char err_buf[512];

        if (JS_IsException(ret)) {
            /* Sync throw — captured as before. */
            JSValue exc = JS_GetException(ctx);
            extract_error_string(ctx, exc, err_buf, sizeof(err_buf));
            JS_FreeValue(ctx, exc);
            record_fail(out, results, max_results, idx, desc, err_buf);
            (*failed)++;
        }
        else if (JS_IsObject(ret) && backend && backend->monotonic_ms) {
            /* Promise-aware path. JS_PromiseState only meaningful on
             * Promise values; if `ret` is some other object that
             * happens to pass JS_IsObject we'll still get
             * JS_PROMISE_PENDING-or-equivalent on the first call and
             * fall through to the timeout. In practice async test
             * bodies always return Promises. */
            uint64_t deadline = backend->monotonic_ms() +
                                (timeout_ms > 0 ? (uint64_t)timeout_ms : 0);
            JSPromiseStateEnum pstate = JS_PromiseState(ctx, ret);
            int is_promise = (pstate == JS_PROMISE_PENDING ||
                              pstate == JS_PROMISE_FULFILLED ||
                              pstate == JS_PROMISE_REJECTED);

            if (!is_promise) {
                /* Sync function returned a non-Promise object — treat
                 * as PASS like the original code did for any non-
                 * exception return. */
                record_pass(out, results, max_results, idx, desc);
                (*passed)++;
            } else {
                /* Pump microtasks (and the async backend if any) until
                 * the promise settles or the deadline expires. The
                 * combination handles both pure-assertion async tests
                 * (only microtasks needed) and tests that perform
                 * async I/O (timer / http.fetch / compute.async — the
                 * backend tick advances those). */
                while ((pstate = JS_PromiseState(ctx, ret)) == JS_PROMISE_PENDING) {
                    hl_js_run_jobs(js);
                    if (async_ctx && backend->tick)
                        backend->tick(async_ctx, 10);
                    if (timeout_ms > 0 && backend->monotonic_ms() >= deadline)
                        break;
                    /* If no async backend is wired and microtasks
                     * settled nothing, we'd spin — guard by re-
                     * checking state after a single microtask pass
                     * and bailing if still PENDING. */
                    if (!async_ctx) {
                        if (JS_PromiseState(ctx, ret) == JS_PROMISE_PENDING)
                            break;
                    }
                }

                if (pstate == JS_PROMISE_FULFILLED) {
                    record_pass(out, results, max_results, idx, desc);
                    (*passed)++;
                } else if (pstate == JS_PROMISE_REJECTED) {
                    JSValue reason = JS_PromiseResult(ctx, ret);
                    extract_error_string(ctx, reason, err_buf, sizeof(err_buf));
                    JS_FreeValue(ctx, reason);
                    record_fail(out, results, max_results, idx, desc, err_buf);
                    (*failed)++;
                } else {
                    /* Still PENDING — treat as timeout/hang. */
                    snprintf(err_buf, sizeof(err_buf),
                             "test did not settle within %d ms", timeout_ms);
                    record_fail(out, results, max_results, idx, desc, err_buf);
                    (*failed)++;
                }
            }
        }
        else {
            /* Sync test body that returned a primitive (undefined,
             * boolean, etc.). Treat as PASS — matches the original
             * runner's behavior for any non-exception return. */
            record_pass(out, results, max_results, idx, desc);
            (*passed)++;
        }

        JS_FreeValue(ctx, ret);
        if (desc) JS_FreeCString(ctx, desc);
        JS_FreeValue(ctx, timeout_val);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, desc_val);
        JS_FreeValue(ctx, entry);
    }
}

#endif /* HL_ENABLE_JS */
