/*
 * mod_exec.c - hull:exec module: run a command the manifest declares
 * (docs/design/exec_broker.md)
 *
 *   import { exec } from "hull:exec";
 *   const r = await exec.run("unit-state", ["hs-api.service"]);
 *   // r = { status, signal, stdout, stderr, stdoutBytes, timedOut,
 *   //       stdoutTruncated, stderrTruncated, durationMs }
 *
 * The twin of runtime/lua/mod_exec.c. The name must be declared in the
 * manifest's `exec` section and the arguments must fill its slots
 * (hl_exec_check_call, the one matcher); a mismatch throws. stdout and
 * stderr are byte strings (one code unit per byte), as hull/ssh's are;
 * stdoutBytes is the same bytes as an ArrayBuffer.
 *
 * No command broker yet: outside `hull test` every call throws after the
 * match. Under `hull test`, test.execStub(name, result) registers the
 * answer, given after the full match and cut to stdout_max / stderr_max.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "mod_exec.h"
#include "internal.h"   /* hl_js_export_frozen, hl_js_internals, hl_js_get_length32 */
#include "refusal.h"
#include "hull/cap/exec.h"
#include "hull/cap/exec_broker.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#ifdef HL_ENABLE_DB
#include "db_wait.h"    /* hl_js_db_refuse_wait */
#endif

#include <string.h>

#define EXEC_STUBS_KEY "__hull_exec_stubs"

/* The stub table in the runtime's internal store, or undefined outside
 * hull test. A new reference. */
static JSValue exec_stubs(JSContext *ctx)
{
    JSValue store = hl_js_internals(ctx);
    JSValue t = JS_IsObject(store) ? JS_GetPropertyStr(ctx, store, EXEC_STUBS_KEY)
                                   : JS_UNDEFINED;
    JS_FreeValue(ctx, store);
    if (JS_IsException(t)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
    return t;
}

static int exec_find(JSContext *ctx, const char *name, size_t n, HlExecCmd *out)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.exec_policy) return -1;
    return hl_exec_policy_find(js->base.exec_policy, js->base.exec_policy_len,
                               name, n, out);
}

/* A byte string from raw bytes: every byte one code unit (JS_NewStringLen
 * takes UTF-8, so bytes >= 0x80 are written as U+0080..U+00FF). */
static JSValue byte_string(JSContext *ctx, const uint8_t *p, size_t n)
{
    size_t hi = 0;
    for (size_t i = 0; i < n; i++) hi += p[i] >> 7;
    if (!hi) return JS_NewStringLen(ctx, (const char *)p, n);
    char *u = js_malloc(ctx, n + hi);
    if (!u) return JS_EXCEPTION;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c < 0x80) u[k++] = (char)c;
        else { u[k++] = (char)(0xC0 | (c >> 6)); u[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    JSValue s = JS_NewStringLen(ctx, u, k);
    js_free(ctx, u);
    return s;
}

/* A stub's output field as bytes (js_malloc'd into *out; NULL / 0 when
 * absent): a byte string (code units <= 0xFF) or an ArrayBuffer. 0, or -1
 * with an exception pending. */
static int stub_bytes(JSContext *ctx, JSValueConst stub, const char *key,
                      uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    JSValue v = JS_GetPropertyStr(ctx, stub, key);
    if (JS_IsException(v)) return -1;
    int rc = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        /* empty */
    } else if (JS_IsString(v)) {
        size_t ul = 0;
        const char *u = JS_ToCStringLen(ctx, &ul, v);
        if (!u) { rc = -1; goto done; }
        uint8_t *b = js_malloc(ctx, ul ? ul : 1);
        if (!b) { JS_FreeCString(ctx, u); rc = -1; goto done; }
        size_t k = 0;
        for (size_t i = 0; i < ul; i++) {
            unsigned char c = (unsigned char)u[i];
            if (c < 0x80) { b[k++] = c; continue; }
            if ((c & 0xE0) == 0xC0 && i + 1 < ul && c <= 0xC3) {
                b[k++] = (uint8_t)(((c & 0x1F) << 6) | ((unsigned char)u[i + 1] & 0x3F));
                i++;
                continue;
            }
            JS_FreeCString(ctx, u);
            js_free(ctx, b);
            JS_ThrowTypeError(ctx, "exec stub: %s is a byte string (every code "
                              "unit 0..255) or an ArrayBuffer", key);
            rc = -1;
            goto done;
        }
        JS_FreeCString(ctx, u);
        *out = b;
        *len = k;
    } else {
        size_t bl = 0;
        uint8_t *p = JS_GetArrayBuffer(ctx, &bl, v);
        if (!p) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_ThrowTypeError(ctx, "exec stub: %s is a byte string or an "
                              "ArrayBuffer", key);
            rc = -1;
            goto done;
        }
        uint8_t *b = js_malloc(ctx, bl ? bl : 1);
        if (!b) { rc = -1; goto done; }
        if (bl) memcpy(b, p, bl);
        *out = b;
        *len = bl;
    }
done:
    JS_FreeValue(ctx, v);
    return rc;
}

/* The result object the broker will return, from a stub's object. */
static JSValue build_result(JSContext *ctx, JSValueConst stub, const HlExecCmd *cmd)
{
    uint8_t *so = NULL, *se = NULL;
    size_t sol = 0, sel = 0;
    JSValue res;
    int64_t status = 0, ms = 0;

    JSValue v = JS_GetPropertyStr(ctx, stub, "status");
    if (JS_IsException(v)) return JS_EXCEPTION;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        double d = 0;
        int ok = JS_IsNumber(v);
        if (ok) JS_ToFloat64(ctx, &d, v);   /* a number: runs no app code */
        ok = ok && d >= -2147483648.0 && d <= 2147483647.0 &&
             d == (double)(int32_t)d;       /* in range: a defined cast */
        if (!ok) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx, "exec stub: status must be an integer");
        }
        status = (int64_t)d;
    }
    JS_FreeValue(ctx, v);

    JSValue sig = JS_GetPropertyStr(ctx, stub, "signal");
    if (JS_IsException(sig)) return JS_EXCEPTION;
    if (!JS_IsUndefined(sig) && !JS_IsNull(sig) && !JS_IsString(sig)) {
        JS_FreeValue(ctx, sig);
        return JS_ThrowTypeError(ctx, "exec stub: signal must be a string "
                                 "(\"SIGKILL\") or null");
    }
    if (JS_IsUndefined(sig)) sig = JS_NULL;

    JSValue to = JS_GetPropertyStr(ctx, stub, "timedOut");
    if (JS_IsException(to)) { JS_FreeValue(ctx, sig); return JS_EXCEPTION; }
    int timed_out = JS_ToBool(ctx, to);
    JS_FreeValue(ctx, to);

    v = JS_GetPropertyStr(ctx, stub, "durationMs");
    if (JS_IsException(v)) { JS_FreeValue(ctx, sig); return JS_EXCEPTION; }
    if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(ctx, &d, v);
        ms = d >= 0 && d < 1e12 ? (int64_t)d : 0;
    }
    JS_FreeValue(ctx, v);

    if (stub_bytes(ctx, stub, "stdout", &so, &sol) != 0 ||
        stub_bytes(ctx, stub, "stderr", &se, &sel) != 0) {
        JS_FreeValue(ctx, sig);
        js_free(ctx, so);
        return JS_EXCEPTION;
    }
    int so_cut = sol > cmd->stdout_max, se_cut = sel > cmd->stderr_max;
    if (so_cut) sol = cmd->stdout_max;
    if (se_cut) sel = cmd->stderr_max;

    /* No app code from here on: only plain objects are made. */
    res = JS_NewObject(ctx);
    JSValue s_out = byte_string(ctx, so ? so : (const uint8_t *)"", sol);
    JSValue s_err = byte_string(ctx, se ? se : (const uint8_t *)"", sel);
    JSValue ab = JS_NewArrayBufferCopy(ctx, so ? so : (const uint8_t *)"", sol);
    js_free(ctx, so);
    js_free(ctx, se);
    if (JS_IsException(res) || JS_IsException(s_out) || JS_IsException(s_err) ||
        JS_IsException(ab)) {
        JS_FreeValue(ctx, res); JS_FreeValue(ctx, s_out); JS_FreeValue(ctx, s_err);
        JS_FreeValue(ctx, ab); JS_FreeValue(ctx, sig);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, res, "status", JS_NewInt64(ctx, status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "signal", sig, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdout", s_out, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stderr", s_err, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdoutBytes", ab, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "timedOut", JS_NewBool(ctx, timed_out), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdoutTruncated", JS_NewBool(ctx, so_cut), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stderrTruncated", JS_NewBool(ctx, se_cut), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "durationMs", JS_NewInt64(ctx, ms), JS_PROP_C_W_E);
    return res;
}

/* The broker's answer as the promise's value: the result object for DONE,
 * else an error (BUSY carries the code busy). */
static JSValue exec_push(JSContext *ctx, void *driver)
{
    const HlExecOp *op = driver;
    const HlExecReply *r = op ? op->reply : NULL;
    if (!r) return JS_ThrowTypeError(ctx, "exec.run: no reply from the command broker");
    if (r->kind == HL_EXEC_R_BUSY)
        return hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowTypeError(ctx, "%s", r->msg));
    if (r->kind != HL_EXEC_R_DONE) return JS_ThrowTypeError(ctx, "%s", r->msg);
    /* The output is charged on the way out, 1 unit per 8 bytes. */
    if (hl_js_budget_charge(ctx, (r->out_len + r->err_len) / 8)) return JS_EXCEPTION;
    JSValue res = JS_NewObject(ctx);
    JSValue s_out = byte_string(ctx, r->out, r->out_len);
    JSValue s_err = byte_string(ctx, r->err, r->err_len);
    JSValue ab = JS_NewArrayBufferCopy(ctx, r->out, r->out_len);
    JSValue sig = r->signal[0] ? JS_NewString(ctx, r->signal) : JS_NULL;
    if (JS_IsException(res) || JS_IsException(s_out) || JS_IsException(s_err) ||
        JS_IsException(ab) || JS_IsException(sig)) {
        JS_FreeValue(ctx, res); JS_FreeValue(ctx, s_out); JS_FreeValue(ctx, s_err);
        JS_FreeValue(ctx, ab); JS_FreeValue(ctx, sig);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, res, "status", JS_NewInt32(ctx, r->status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "signal", sig, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdout", s_out, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stderr", s_err, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdoutBytes", ab, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "timedOut", JS_NewBool(ctx, r->timed_out), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdoutTruncated", JS_NewBool(ctx, r->out_cut), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stderrTruncated", JS_NewBool(ctx, r->err_cut), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "durationMs", JS_NewInt64(ctx, r->duration_ms), JS_PROP_C_W_E);
    return res;
}

/* The call through the broker: gated as every Hull wait, submitted, and
 * parked until the reply (the promise). JS_EXCEPTION with one pending. */
static JSValue exec_run_broker(JSContext *ctx, HlJS *js, const char *name,
                               size_t name_len, const char *const *args,
                               const size_t *lens, size_t n, const HlExecCmd *cmd)
{
    if (!js || !hl_exec_broker_running() || !js->base.async_ctx)
        return JS_ThrowTypeError(ctx, "exec.run('%s'): the command broker is not "
                                 "running (this host has none, or it was lost)", name);
    /* The fast admission check; the broker decides (§5.2). */
    if (hl_exec_inflight() >= HL_EXEC_MAX_INFLIGHT ||
        hl_exec_inflight_cmd(name, name_len) >= cmd->concurrency)
        return hl_js_refuse(ctx, HL_REFUSAL_BUSY,
                            JS_ThrowTypeError(ctx, "exec.run('%s'): too many calls "
                                              "in flight", name));
    if (hl_js_async_gate(ctx, js, "exec.run") != 0) return JS_EXCEPTION;
#ifdef HL_ENABLE_DB
    if (hl_js_db_refuse_wait(ctx, "exec.run") != 0) return JS_EXCEPTION;
#endif
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx, js->base.alloc);
    if (!actx) return JS_ThrowInternalError(ctx, "exec.run: out of memory");
    JSValue fns[2];
    JSValue promise = JS_NewPromiseCapability(ctx, fns);
    if (JS_IsException(promise)) { hl_async_ctx_free(actx); return JS_EXCEPTION; }
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js, fns[0], fns[1], js->base.alloc,
                                                exec_push);
    if (!cont) {
        JS_FreeValue(ctx, fns[0]);
        JS_FreeValue(ctx, fns[1]);
        JS_FreeValue(ctx, promise);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "exec.run: out of memory");
    }
    actx->cont = cont;
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);
    char why[256];
    if (hl_exec_op_start(actx, js->base.async_ctx, name, name_len, args, lens, n,
                         cmd->timeout_ms, why, sizeof why) != 0) {
        actx->cont->destroy(actx->cont);   /* never armed: frees resolve / reject */
        actx->cont = NULL;
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowTypeError(ctx, "exec.run('%s'): %s", name, why);
    }
    if (!actx->detached) {
        if (hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
            actx->cont->destroy(actx->cont);
            actx->cont = NULL;
            hl_async_ctx_cancel(actx);     /* frees the op: KILL */
            JS_FreeValue(ctx, promise);
            return JS_ThrowInternalError(ctx, "exec.run: failed to suspend");
        }
    } else {
        /* No request holds it: a teardown cancels it, and the op's free
         * sends KILL. */
        hl_async_ctx_track(actx, &js->base.async_live);
    }
    return promise;
}

/* exec.run(name, args?) -> Promise<result> */
static JSValue js_exec_run(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "exec.run(name, args?): name must be a string");

    /* Copy every argument out first: reading an element of the caller's
     * array can run app code (a getter, a Proxy trap), so nothing below
     * holds a pointer into it. */
    char *bufs[HL_EXEC_MAX_CALL_ARGS];
    const char *args[HL_EXEC_MAX_CALL_ARGS];
    size_t lens[HL_EXEC_MAX_CALL_ARGS];
    size_t n = 0, total = 0;
    JSValue ret = JS_EXCEPTION;
    const char *name = NULL;
    size_t name_len = 0;
    JSValue copy = JS_UNDEFINED;   /* the arguments, for a stub function */

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (JS_IsArray(ctx, argv[1]) <= 0)
            return JS_ThrowTypeError(ctx, "exec.run: args must be an array of strings");
        int32_t len = 0;
        if (hl_js_get_length32(ctx, argv[1], &len) != 0) return JS_EXCEPTION;
        if (len > HL_EXEC_MAX_CALL_ARGS)
            return JS_ThrowRangeError(ctx, "exec.run: more than %d arguments",
                                      HL_EXEC_MAX_CALL_ARGS);
        for (int32_t i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
            if (JS_IsException(e)) goto out;
            if (!JS_IsString(e)) {
                JS_FreeValue(ctx, e);
                JS_ThrowTypeError(ctx, "exec.run: argument %d must be a string",
                                  (int)i + 1);
                goto out;
            }
            size_t sl = 0;
            const char *s = JS_ToCStringLen(ctx, &sl, e);
            JS_FreeValue(ctx, e);
            if (!s) goto out;
            bufs[n] = js_malloc(ctx, sl + 1);
            if (!bufs[n]) { JS_FreeCString(ctx, s); goto out; }
            memcpy(bufs[n], s, sl + 1);
            JS_FreeCString(ctx, s);
            args[n] = bufs[n];
            lens[n] = sl;
            total += sl;
            n++;
        }
    }
    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) goto out;

    /* The match walks the policy and the arguments: charged first. */
    if (hl_js_budget_charge(ctx, 64 + total / 8)) goto out;

    HlExecCmd cmd;
    char err[256];
    int rc = js && js->base.exec_policy
           ? hl_exec_check_call(js->base.exec_policy, js->base.exec_policy_len,
                                name, name_len, args, lens, n, &cmd, err, sizeof err)
           : HL_EXEC_E_UNKNOWN;
    if (rc == HL_EXEC_E_UNKNOWN) {
        JS_ThrowTypeError(ctx, "exec.run: '%s' is not declared in the manifest's "
                          "exec section", name);
        goto out;
    }
    if (rc != HL_EXEC_OK) {
        JS_ThrowTypeError(ctx, "exec.run('%s'): %s", name, err);
        goto out;
    }

    /* A run will wait for the command (PR 2): refused now where a wait will
     * be refused then - middleware, the module top level, a transaction. */
    if (js && (js->in_middleware || js->in_load)) {
        JS_ThrowTypeError(ctx, "exec.run cannot wait %s", js->in_middleware
                          ? "in middleware (it is synchronous)"
                          : "at the module top level");
        hl_js_refusal_tag(ctx, HL_REFUSAL_WAIT);
        goto out;
    }
#ifdef HL_ENABLE_DB
    if (hl_js_db_refuse_wait(ctx, "exec.run") != 0) goto out;
#endif

    JSValue stubs = exec_stubs(ctx);
    if (!JS_IsObject(stubs)) {          /* not hull test: the broker runs it */
        JS_FreeValue(ctx, stubs);
        ret = exec_run_broker(ctx, js, name, name_len, args, lens, n, &cmd);
        goto out;
    }
    JSValue stub = JS_GetPropertyStr(ctx, stubs, name);
    JS_FreeValue(ctx, stubs);
    if (JS_IsException(stub)) goto out;
    if (JS_IsUndefined(stub)) {
        JS_ThrowTypeError(ctx, "exec.run: no stub for '%s' in hull test "
                          "(register one with test.execStub)", name);
        goto out;
    }
    if (JS_IsFunction(ctx, stub)) {
        copy = JS_NewArray(ctx);
        if (JS_IsException(copy)) { JS_FreeValue(ctx, stub); goto out; }
        for (size_t i = 0; i < n; i++) {
            JSValue s = JS_NewStringLen(ctx, args[i], lens[i]);
            if (JS_IsException(s) ||
                JS_DefinePropertyValueUint32(ctx, copy, (uint32_t)i, s,
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, stub);
                goto out;
            }
        }
        JSValue cargs[2] = { copy, argv[0] };
        JSValue r = JS_Call(ctx, stub, JS_UNDEFINED, 2, cargs);
        JS_FreeValue(ctx, stub);
        if (JS_IsException(r)) goto out;
        stub = r;
    }
    if (!JS_IsObject(stub) || JS_IsFunction(ctx, stub)) {
        JS_FreeValue(ctx, stub);
        JS_ThrowTypeError(ctx, "exec stub for '%s' must give a result object", name);
        goto out;
    }
    JSValue then = JS_GetPropertyStr(ctx, stub, "then");
    int thenable = JS_IsFunction(ctx, then);
    JS_FreeValue(ctx, then);
    if (thenable) {
        JS_FreeValue(ctx, stub);
        JS_ThrowTypeError(ctx, "exec stub for '%s' must give a result object, "
                          "not a promise", name);
        goto out;
    }
    JSValue res = build_result(ctx, stub, &cmd);
    JS_FreeValue(ctx, stub);
    if (JS_IsException(res)) goto out;

    JSValue fns[2];
    JSValue promise = JS_NewPromiseCapability(ctx, fns);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, res); goto out; }
    JSValue r = JS_Call(ctx, fns[0], JS_UNDEFINED, 1, (JSValueConst *)&res);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, fns[0]);
    JS_FreeValue(ctx, fns[1]);
    ret = promise;

out:
    if (name) JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, copy);
    for (size_t i = 0; i < n; i++) js_free(ctx, bufs[i]);
    return ret;
}

static int js_exec_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/exec", "hull:exec") != 0) return -1;
    JSValue exec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, exec, "run", JS_NewCFunction(ctx, js_exec_run, "run", 2));
    hl_js_export_frozen(ctx, m, "exec", exec);
    return 0;
}

int hl_js_init_exec_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:exec", js_exec_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "exec");
    return 0;
}

/* test.execStub(name, result | fn | null) */
JSValue hl_js_exec_stub(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "test.execStub(name, result): name must be "
                                 "a string");
    JSValueConst val = argc > 1 ? argv[1] : JS_UNDEFINED;
    int remove = JS_IsUndefined(val) || JS_IsNull(val);
    if (!remove && !JS_IsObject(val))
        return JS_ThrowTypeError(ctx, "test.execStub(name, result): result is an "
                                 "object, a function, or null to remove the stub");
    size_t n = 0;
    const char *name = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!name) return JS_EXCEPTION;
    HlExecCmd cmd;
    if (exec_find(ctx, name, n, &cmd) != 0) {
        JS_ThrowTypeError(ctx, "test.execStub: '%s' is not declared in the "
                          "manifest's exec section", name);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    JSValue stubs = exec_stubs(ctx);
    if (!JS_IsObject(stubs)) {
        JS_FreeValue(ctx, stubs);
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "test.execStub is only available in hull test");
    }
    int rc;
    if (remove) {
        JSAtom a = JS_NewAtomLen(ctx, name, n);
        rc = a == JS_ATOM_NULL ? -1 : JS_DeleteProperty(ctx, stubs, a, 0);
        if (a != JS_ATOM_NULL) JS_FreeAtom(ctx, a);
    } else {
        rc = JS_DefinePropertyValueStr(ctx, stubs, name, JS_DupValue(ctx, val),
                                       JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, stubs);
    JS_FreeCString(ctx, name);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

void hl_js_exec_stubs_reset(JSContext *ctx)
{
    JSValue store = hl_js_internals(ctx);
    if (JS_IsObject(store)) {
        JSValue t = JS_NewObjectProto(ctx, JS_NULL);
        if (!JS_IsException(t))
            JS_SetPropertyStr(ctx, store, EXEC_STUBS_KEY, t);
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, store);
}
