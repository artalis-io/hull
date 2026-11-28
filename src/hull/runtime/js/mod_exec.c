/*
 * mod_exec.c - hull:exec module: run a command the manifest declares
 * (docs/design/exec_broker.md)
 *
 *   import { exec } from "hull:exec";
 *   const r = await exec.run("unit-state", ["hs-api.service"]);
 *   const r = await exec.run("key-import", [], { stdin: record });
 *   // r = { status, signal, stdout, stderr, stdoutBytes, timedOut,
 *   //       stdoutTruncated, stderrTruncated, durationMs }
 *
 * The twin of runtime/lua/mod_exec.c. The name must be declared in the
 * manifest's `exec` section and the arguments must fill its slots
 * (hl_exec_check_call, the one matcher); a mismatch throws. stdout and
 * stderr are byte strings (one code unit per byte), as hull/ssh's are;
 * stdoutBytes is the same bytes as an ArrayBuffer. `stdin` (a string,
 * sent as UTF-8, or an ArrayBuffer / typed array) is written to the
 * command's stdin; a non-empty one needs the command's stdin_max.
 *
 * The command broker runs the call (cap/exec_broker.c). Under `hull test`,
 * test.execStub(name, result) registers the answer, given after the full
 * match and cut to stdout_max / stderr_max. For exec.stream the stub
 * carries `chunks` (and `onWrite`), queued on a stub stream at once
 * (hl_exec_stream_stub), so nothing waits.
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
#include "hull/shared/work_charge.h"

#include <stdlib.h>
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
/* The continuation's push_result signature fixes the parameter type. */
// cppcheck-suppress constParameterCallback
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
                               const size_t *lens, size_t n, const uint8_t *in,
                               size_t in_len, const HlExecCmd *cmd)
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
                         in, in_len, cmd->timeout_ms, why, sizeof why) != 0) {
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

/* opts.stdin, copied at once (a view's buffer could be detached or resized
 * by later app code). `stdin` is the only key: a typo is never a silent
 * no-op. 0 with *out / *len set (NULL / 0 when absent), or -1 with an
 * exception pending. */
static int read_opts_ex(JSContext *ctx, JSValueConst opts, uint8_t **out, size_t *len,
                        int *stdin_open)
{
    *out = NULL;
    *len = 0;
    if (stdin_open) *stdin_open = 0;
    const char *what = stdin_open ? "exec.stream" : "exec.run";
    if (JS_IsUndefined(opts) || JS_IsNull(opts)) return 0;
    if (!JS_IsObject(opts) || JS_IsArray(ctx, opts) != 0 || JS_IsFunction(ctx, opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object ({ stdin%s })", what,
                          stdin_open ? ", stdinOpen" : "");
        return -1;
    }
    JSPropertyEnum *tab = NULL;
    uint32_t np = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &np, opts,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return -1;
    int bad = 0;
    for (uint32_t i = 0; i < np; i++) {
        const char *k = JS_AtomToCString(ctx, tab[i].atom);
        if (!k || (strcmp(k, "stdin") != 0 &&
                   !(stdin_open && strcmp(k, "stdinOpen") == 0))) bad = 1;
        if (k) JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, tab, np);
    if (bad) {
        JS_ThrowTypeError(ctx, stdin_open ? "exec.stream: unknown option (stdin, stdinOpen)"
                                          : "exec.run: unknown option (the only one is stdin)");
        return -1;
    }
    if (stdin_open) {
        JSValue o = JS_GetPropertyStr(ctx, opts, "stdinOpen");
        if (JS_IsException(o)) return -1;
        if (!JS_IsUndefined(o) && !JS_IsBool(o)) {
            JS_FreeValue(ctx, o);
            JS_ThrowTypeError(ctx, "exec.stream: stdinOpen must be a boolean");
            return -1;
        }
        *stdin_open = JS_ToBool(ctx, o) > 0;
        JS_FreeValue(ctx, o);
    }
    JSValue v = JS_GetPropertyStr(ctx, opts, "stdin");
    if (JS_IsException(v)) return -1;
    int rc = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        /* none */
    } else if (JS_IsString(v)) {
        size_t sl = 0;
        const char *str = JS_ToCStringLen(ctx, &sl, v);   /* UTF-8 */
        if (!str) { rc = -1; goto done; }
        if (sl > HL_EXEC_STDIN_CEIL) {
            JS_FreeCString(ctx, str);
            JS_ThrowRangeError(ctx, "exec.run: stdin is longer than %u bytes",
                               (unsigned)HL_EXEC_STDIN_CEIL);
            rc = -1;
            goto done;
        }
        *out = js_malloc(ctx, sl ? sl : 1);
        if (*out) { memcpy(*out, str, sl); *len = sl; } else rc = -1;
        JS_FreeCString(ctx, str);
    } else {
        size_t bl = 0;
        const uint8_t *p = hl_js_bytes_view(ctx, &bl, v);
        if (!p) {
            JS_ThrowTypeError(ctx, "exec.run: stdin must be a string, an "
                              "ArrayBuffer or a typed array");
            rc = -1;
            goto done;
        }
        if (bl > HL_EXEC_STDIN_CEIL) {
            JS_ThrowRangeError(ctx, "exec.run: stdin is longer than %u bytes",
                               (unsigned)HL_EXEC_STDIN_CEIL);
            rc = -1;
            goto done;
        }
        *out = js_malloc(ctx, bl ? bl : 1);   /* no app code since the view */
        if (*out) { if (bl) memcpy(*out, p, bl); *len = bl; } else rc = -1;
    }
done:
    JS_FreeValue(ctx, v);
    return rc;
}

static int read_opts(JSContext *ctx, JSValueConst opts, uint8_t **out, size_t *len)
{
    return read_opts_ex(ctx, opts, out, len, NULL);
}

/* exec.run(name, args?, opts?) -> Promise<result> */
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
    JSValue stub_in = JS_UNDEFINED;   /* the stdin, for a stub function */
    uint8_t *in = NULL;
    size_t in_len = 0;

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
    if (argc > 2 && read_opts(ctx, argv[2], &in, &in_len) != 0) goto out;
    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) goto out;

    /* The match walks the policy and the arguments, and stdin is copied
     * twice more (the frame, the broker): charged first. */
    if (hl_js_budget_charge(ctx, 64 + (total + in_len) / 8)) goto out;

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
    if (in_len > cmd.stdin_max) {
        if (!cmd.stdin_max)
            JS_ThrowTypeError(ctx, "exec.run('%s'): the command takes no stdin "
                              "(declare stdinMax)", name);
        else
            JS_ThrowTypeError(ctx, "exec.run('%s'): stdin is %zu bytes; the "
                              "command's stdinMax is %u", name, in_len,
                              (unsigned)cmd.stdin_max);
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
        ret = exec_run_broker(ctx, js, name, name_len, args, lens, n, in, in_len,
                              &cmd);
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
        /* stub(args, name, stdin): stdin as an ArrayBuffer, or undefined */
        if (in_len) {
            stub_in = JS_NewArrayBufferCopy(ctx, in, in_len);
            if (JS_IsException(stub_in)) { JS_FreeValue(ctx, stub); goto out; }
        }
        JSValue cargs[3] = { copy, argv[0], stub_in };
        JSValue r = JS_Call(ctx, stub, JS_UNDEFINED, 3, cargs);
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
    JS_FreeValue(ctx, stub_in);
    js_free(ctx, in);
    for (size_t i = 0; i < n; i++) js_free(ctx, bufs[i]);
    return ret;
}

/* ── exec.stream (docs/design/exec_stream.md §4) ───────────────────── *
 *
 * exec.stream(name, args?, { stdin, stdinOpen }) returns a stream object at
 * once: read() -> Promise<{ fd, data: Uint8Array } | null>, write(bytes),
 * closeWrite(), close(), result(), and for-await over the chunks (breaking
 * out closes it). A parked wait pins the object (as hull:http-client's
 * stream does), so `await exec.stream(...).read()` does not lose it, and
 * its command, to the collector mid-wait. */

static JSClassID hl_js_exec_stream_class_id;

typedef struct {
    HlExecStream *s;             /* one reference, released by the finalizer */
    JSValue       on_write;      /* a hull test stub's onWrite, or undefined */
} JsExecStream;

/* Queue a stub's chunks (an array of { fd: "stdout" | "stderr", data:
 * string | ArrayBuffer | typed array }, in order). -1 with an exception. */
static int stub_push_chunks(JSContext *ctx, HlExecStream *s, JSValueConst arr,
                            const char *name)
{
    if (JS_IsArray(ctx, arr) <= 0) {
        JS_ThrowTypeError(ctx, "exec stub for '%s': chunks must be an array", name);
        return -1;
    }
    int32_t len = 0;
    if (hl_js_get_length32(ctx, arr, &len) != 0) return -1;
    for (int32_t i = 0; i < len; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        if (JS_IsException(c)) return -1;
        if (!JS_IsObject(c)) {
            JS_FreeValue(ctx, c);
            JS_ThrowTypeError(ctx, "exec stub for '%s': chunk %d is not an object", name, (int)i);
            return -1;
        }
        JSValue fdv = JS_GetPropertyStr(ctx, c, "fd");
        JSValue dv = JS_IsException(fdv) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, c, "data");
        JS_FreeValue(ctx, c);
        if (JS_IsException(dv)) { JS_FreeValue(ctx, fdv); return -1; }
        const char *fd = JS_IsString(fdv) ? JS_ToCString(ctx, fdv) : NULL;
        int f = fd && strcmp(fd, "stdout") == 0 ? 1 : fd && strcmp(fd, "stderr") == 0 ? 2 : 0;
        if (fd) JS_FreeCString(ctx, fd);
        JS_FreeValue(ctx, fdv);
        if (!f) {
            JS_FreeValue(ctx, dv);
            JS_ThrowTypeError(ctx, "exec stub for '%s': chunk %d fd must be \"stdout\" "
                              "or \"stderr\"", name, (int)i);
            return -1;
        }
        int rc;
        if (JS_IsString(dv)) {
            size_t n = 0;
            const char *str = JS_ToCStringLen(ctx, &n, dv);
            if (!str) { JS_FreeValue(ctx, dv); return -1; }
            rc = hl_exec_stream_stub_chunk(s, f, str, n);
            JS_FreeCString(ctx, str);
        } else {
            size_t n = 0;
            const uint8_t *p = hl_js_bytes_view(ctx, &n, dv);
            /* NULL for anything that is not a buffer (cppcheck reads the
             * inline view as always NULL). */
            // cppcheck-suppress knownConditionTrueFalse
            if (!p) {
                JS_FreeValue(ctx, dv);
                JS_ThrowTypeError(ctx, "exec stub for '%s': chunk %d data must be a "
                                  "string, an ArrayBuffer or a typed array", name, (int)i);
                return -1;
            }
            rc = hl_exec_stream_stub_chunk(s, f, p, n);
        }
        JS_FreeValue(ctx, dv);
        if (rc == -2) {
            JS_ThrowRangeError(ctx, "exec stub for '%s': its chunks would take the VM's "
                               "off-heap copies past its heap limit", name);
            return -1;
        }
        if (rc != 0) { JS_ThrowOutOfMemory(ctx); return -1; }
    }
    return 0;
}

enum { JS_W_READ = 1, JS_W_NEXT, JS_W_WRITE, JS_W_RESULT };

typedef struct {
    JSRuntime *rt;
    JSValue    self;             /* the pin */
    int        kind;             /* JS_W_* */
} JsExecPin;

static void pin_free(void *ud)
{
    JsExecPin *p = ud;
    JS_FreeValueRT(p->rt, p->self);
    free(p);
}

static JsExecStream *stream_this(JSContext *ctx, JSValueConst this_val)
{
    JsExecStream *o = JS_GetOpaque(this_val, hl_js_exec_stream_class_id);
    if (!o || !o->s) JS_ThrowTypeError(ctx, "exec.stream: not a stream");
    return o;
}

static JSValue iter_result(JSContext *ctx, JSValue value, int done)
{
    if (JS_IsException(value)) return value;
    JSValue r = JS_NewObject(ctx);
    if (JS_IsException(r)) { JS_FreeValue(ctx, value); return r; }
    if (JS_DefinePropertyValueStr(ctx, r, "value", value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, r, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

/* The next chunk as { fd, data }, charged before it is taken; null when
 * none is queued (*none set). */
static JSValue take_chunk(JSContext *ctx, HlExecStream *s, int *none)
{
    *none = 0;
    size_t len = hl_exec_stream_peek(s);
    if (len == (size_t)-1) { *none = 1; return JS_NULL; }
    if (hl_js_budget_charge(ctx, HL_WORK_EXEC_CHUNK_UNITS + len / 8)) return JS_EXCEPTION;
    HlExecChunk *c = hl_exec_stream_take(s);
    JSValue ab = JS_NewArrayBufferCopy(ctx, c->data, c->len);
    int fd = c->fd;
    free(c);
    if (JS_IsException(ab)) return ab;
    /* All three arguments: the constructor reads argv[1] / argv[2]
     * whatever argc says. */
    JSValue targs[3] = { ab, JS_NewInt32(ctx, 0), JS_UNDEFINED };
    JSValue u8 = JS_NewTypedArray(ctx, 3, targs, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(u8)) return u8;
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) { JS_FreeValue(ctx, u8); return o; }
    if (JS_DefinePropertyValueStr(ctx, o, "fd", JS_NewString(ctx, fd == 2 ? "stderr" : "stdout"),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, o, "data", u8, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue stream_result_value(JSContext *ctx, const HlExecReply *r)
{
    if (!r) return JS_ThrowTypeError(ctx, "exec.stream: no result from the command broker");
    if (r->kind == HL_EXEC_R_BUSY)
        return hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowTypeError(ctx, "%s", r->msg));
    if (r->kind != HL_EXEC_R_DONE) return JS_ThrowTypeError(ctx, "%s", r->msg);
    JSValue res = JS_NewObject(ctx);
    JSValue sig = r->signal[0] ? JS_NewString(ctx, r->signal) : JS_NULL;
    if (JS_IsException(res) || JS_IsException(sig)) {
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, sig);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, res, "status", JS_NewInt32(ctx, r->status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "signal", sig, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "timedOut", JS_NewBool(ctx, r->timed_out), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "idleTimedOut", JS_NewBool(ctx, r->idle_timed_out), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdoutTruncated", JS_NewBool(ctx, r->out_cut), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stderrTruncated", JS_NewBool(ctx, r->err_cut), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stdoutLength", JS_NewInt64(ctx, (int64_t)r->out_bytes), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "stderrLength", JS_NewInt64(ctx, (int64_t)r->err_bytes), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "durationMs", JS_NewInt64(ctx, r->duration_ms), JS_PROP_C_W_E);
    return res;
}

/* What a call of @p kind gives now, when nothing would wait. *wait set
 * (and JS_UNDEFINED returned) when it must park. */
static JSValue stream_now(JSContext *ctx, HlExecStream *s, int kind, int *wait)
{
    *wait = 0;
    if (kind == JS_W_READ || kind == JS_W_NEXT) {
        int none = 0;
        JSValue c = take_chunk(ctx, s, &none);
        if (!none) return kind == JS_W_NEXT ? iter_result(ctx, c, 0) : c;
        if (hl_exec_stream_drained(s))
            return kind == JS_W_NEXT ? iter_result(ctx, JS_UNDEFINED, 1) : JS_NULL;
        *wait = 1;
        return JS_UNDEFINED;
    }
    if (kind == JS_W_WRITE) {
        if (hl_exec_stream_writing(s)) { *wait = 1; return JS_UNDEFINED; }
        if (!hl_exec_stream_write_ok(s))
            return JS_ThrowTypeError(ctx, "exec.stream write: the command's stdin closed "
                                     "before the bytes were written (stdin_closed)");
        return JS_UNDEFINED;
    }
    const HlExecReply *r = hl_exec_stream_result(s);
    if (!r) { *wait = 1; return JS_UNDEFINED; }
    return stream_result_value(ctx, r);
}

// cppcheck-suppress constParameterCallback
static JSValue stream_push(JSContext *ctx, void *driver)
{
    const JsExecPin *p = hl_exec_stream_wait_ud(driver);
    HlExecStream *s = hl_exec_stream_of_wait(driver);
    if (!p || !s) return JS_ThrowInternalError(ctx, "exec.stream: lost its wait");
    int wait = 0;
    JSValue v = stream_now(ctx, s, p->kind, &wait);
    if (wait) return JS_ThrowInternalError(ctx, "exec.stream: woken with nothing ready");
    return v;
}

/* A promise already settled with @p v (taken), or rejected with the
 * pending exception when v is JS_EXCEPTION. */
static JSValue settled(JSContext *ctx, JSValue v)
{
    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, v); return promise; }
    JSValue arg = v, ret;
    if (JS_IsException(v)) {
        arg = JS_GetException(ctx);
        ret = JS_Call(ctx, rf[1], JS_UNDEFINED, 1, &arg);
    } else {
        ret = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, &arg);
    }
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, rf[0]);
    JS_FreeValue(ctx, rf[1]);
    return promise;
}

/* The wait gates, with the exception set when they refuse (-1). */
static int stream_gate(JSContext *ctx, const char *what)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.async_ctx) {
        JS_ThrowInternalError(ctx, "%s: cannot wait", what);
        return -1;
    }
    if (hl_js_async_gate(ctx, js, what) != 0) return -1;
#ifdef HL_ENABLE_DB
    if (hl_js_db_refuse_wait(ctx, what) != 0) return -1;
#endif
    return 0;
}

static JSValue stream_park(JSContext *ctx, HlExecStream *s, JSValueConst self, int kind,
                           const char *what)
{
    if (stream_gate(ctx, what) != 0) return JS_EXCEPTION;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    int ckind = kind == JS_W_WRITE ? HL_EXEC_WAIT_WRITE
              : kind == JS_W_RESULT ? HL_EXEC_WAIT_RESULT : HL_EXEC_WAIT_READ;
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx, js->base.alloc);
    if (!actx) return JS_ThrowOutOfMemory(ctx);
    JSValue fns[2];
    JSValue promise = JS_NewPromiseCapability(ctx, fns);
    if (JS_IsException(promise)) { hl_async_ctx_free(actx); return promise; }
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js, fns[0], fns[1], js->base.alloc,
                                                stream_push);
    if (!cont) {
        JS_FreeValue(ctx, fns[0]);
        JS_FreeValue(ctx, fns[1]);
        JS_FreeValue(ctx, promise);
        hl_async_ctx_free(actx);
        return JS_ThrowOutOfMemory(ctx);
    }
    actx->cont = cont;
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);
    JsExecPin *pin = malloc(sizeof *pin);
    if (!pin) {
        actx->cont = NULL;
        cont->destroy(cont);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowOutOfMemory(ctx);
    }
    pin->rt = JS_GetRuntime(ctx);
    pin->self = JS_DupValue(ctx, self);
    pin->kind = kind;
    if (hl_exec_stream_wait_ex(actx, s, ckind, pin_free, pin) != 0) {
        pin_free(pin);
        actx->cont = NULL;
        cont->destroy(cont);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        JS_ThrowTypeError(ctx, "%s: another one is already waiting on this stream", what);
        return hl_js_refusal_tag(ctx, HL_REFUSAL_BUSY);
    }
    if (!actx->detached) {
        if (hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
            actx->cont->destroy(actx->cont);
            actx->cont = NULL;
            hl_async_ctx_cancel(actx);     /* the wait's free closes the stream */
            JS_FreeValue(ctx, promise);
            return JS_ThrowInternalError(ctx, "%s: failed to suspend", what);
        }
    } else {
        hl_async_ctx_track(actx, &js->base.async_live);
    }
    return promise;
}

static JSValue stream_call(JSContext *ctx, JSValueConst this_val, int kind, const char *what)
{
    JsExecStream *o = stream_this(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    int wait = 0;
    JSValue v = stream_now(ctx, o->s, kind, &wait);
    if (!wait) return settled(ctx, v);
    return stream_park(ctx, o->s, this_val, kind, what);
}

static JSValue js_stream_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return stream_call(ctx, this_val, JS_W_READ, "exec.stream read()");
}

static JSValue js_stream_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return stream_call(ctx, this_val, JS_W_NEXT, "exec.stream iterator");
}

static JSValue js_stream_result(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    (void)argc; (void)argv;
    return stream_call(ctx, this_val, JS_W_RESULT, "exec.stream result()");
}

/* write(bytes): a string (UTF-8), an ArrayBuffer or a typed array. */
static JSValue js_stream_write(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    JsExecStream *o = stream_this(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    const uint8_t *p = NULL;
    size_t n = 0;
    const char *str = NULL;
    if (JS_IsString(v)) {
        str = JS_ToCStringLen(ctx, &n, v);
        if (!str) return JS_EXCEPTION;
        p = (const uint8_t *)str;
    } else {
        p = hl_js_bytes_view(ctx, &n, v);
        /* NULL for anything that is not a buffer (cppcheck reads the inline
         * view as always NULL). */
        // cppcheck-suppress knownConditionTrueFalse
        if (!p) return JS_ThrowTypeError(ctx, "exec.stream write: a string, an "
                                         "ArrayBuffer or a typed array");
    }
    if (hl_js_budget_charge(ctx, 64 + n / 8)) {
        if (str) JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    char why[192];
    const char *code = NULL;
    if (hl_exec_stream_is_stub(o->s)) {
        /* hull test: the stub's onWrite(bytes) may give more chunks. The
         * bytes are copied before any app code runs. */
        int rc = hl_exec_stream_stub_write(o->s, n, why, sizeof why, &code);
        JSValue copy = rc == 0 && JS_IsFunction(ctx, o->on_write)
                     ? JS_NewArrayBufferCopy(ctx, p, n) : JS_UNDEFINED;
        if (str) JS_FreeCString(ctx, str);
        if (rc != 0) return settled(ctx, JS_ThrowTypeError(ctx, "%s (%s)", why, code));
        if (JS_IsException(copy)) return settled(ctx, copy);
        if (JS_IsUndefined(copy)) return settled(ctx, JS_UNDEFINED);
        JSValue targs[3] = { copy, JS_NewInt32(ctx, 0), JS_UNDEFINED };
        JSValue u8 = JS_NewTypedArray(ctx, 3, targs, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, copy);
        if (JS_IsException(u8)) return settled(ctx, u8);
        JSValue fn = JS_DupValue(ctx, o->on_write);
        JSValue more = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&u8);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, u8);
        if (JS_IsException(more)) return settled(ctx, more);
        /* The app code may have closed it; o itself lives while this_val does. */
        int prc = JS_IsUndefined(more) || JS_IsNull(more) || !o->s ? 0
                : stub_push_chunks(ctx, o->s, more, "onWrite");
        JS_FreeValue(ctx, more);
        return settled(ctx, prc != 0 ? JS_EXCEPTION : JS_UNDEFINED);
    }
    /* A write that will have to wait is refused before anything is queued:
     * refused after, its bytes still went out (review of PR 2, L1). */
    if (hl_exec_stream_write_would_wait(o->s, n) && stream_gate(ctx, "exec.stream write()") != 0) {
        if (str) JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    /* No app code ran since the view was taken; the bytes are copied here. */
    int rc = hl_exec_stream_write(o->s, p, n, why, sizeof why, &code);
    if (str) JS_FreeCString(ctx, str);
    if (rc < 0) {
        JSValue e = JS_ThrowTypeError(ctx, "%s (%s)", why, code);
        if (strcmp(code, "busy") == 0) e = hl_js_refusal_tag(ctx, HL_REFUSAL_BUSY);
        return settled(ctx, e);
    }
    if (rc == 1) return settled(ctx, JS_UNDEFINED);
    return stream_park(ctx, o->s, this_val, JS_W_WRITE, "exec.stream write()");
}

static JSValue js_stream_close_write(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv)
{
    (void)argc; (void)argv;
    JsExecStream *o = stream_this(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    char why[192];
    if (hl_exec_stream_close_write(o->s, why, sizeof why) != 0)
        return JS_ThrowTypeError(ctx, "%s", why);
    return JS_UNDEFINED;
}

static JSValue js_stream_close(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    (void)argc; (void)argv;
    JsExecStream *o = stream_this(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    hl_exec_stream_close(o->s);
    return JS_UNDEFINED;
}

/* The iterator's return(): a for-await that breaks out closes the stream. */
static JSValue js_stream_return(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{
    JSValue r = js_stream_close(ctx, this_val, argc, argv);
    if (JS_IsException(r)) return settled(ctx, r);
    return settled(ctx, iter_result(ctx, JS_UNDEFINED, 1));
}

static JSValue js_stream_async_iterator(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

static void js_stream_finalizer(JSRuntime *rt, JSValue val)
{
    JsExecStream *o = JS_GetOpaque(val, hl_js_exec_stream_class_id);
    if (!o) return;
    /* A parked wait pins the object, so no wait is parked here. */
    if (o->s) {
        hl_exec_stream_close(o->s);
        hl_exec_stream_release(o->s);
    }
    JS_FreeValueRT(rt, o->on_write);
    free(o);
}

/* A stub's onWrite may capture its own stream: reported so the pair can be
 * collected. */
static void js_stream_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JsExecStream *o = JS_GetOpaque(val, hl_js_exec_stream_class_id);
    if (o) JS_MarkValue(rt, o->on_write, mark_func);
}

static const JSClassDef js_exec_stream_class = {
    "ExecStream", .finalizer = js_stream_finalizer, .gc_mark = js_stream_mark,
};

static const JSCFunctionListEntry js_stream_methods[] = {
    JS_CFUNC_DEF("read", 0, js_stream_read),
    JS_CFUNC_DEF("write", 1, js_stream_write),
    JS_CFUNC_DEF("closeWrite", 0, js_stream_close_write),
    JS_CFUNC_DEF("close", 0, js_stream_close),
    JS_CFUNC_DEF("result", 0, js_stream_result),
    JS_CFUNC_DEF("next", 0, js_stream_next),
    JS_CFUNC_DEF("return", 0, js_stream_return),
};

static int stream_register_class(JSContext *ctx)
{
    JS_NewClassID(&hl_js_exec_stream_class_id);
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (JS_IsRegisteredClass(rt, hl_js_exec_stream_class_id)) return 0;
    if (JS_NewClass(rt, hl_js_exec_stream_class_id, &js_exec_stream_class) < 0) return -1;
    JSValue proto = JS_NewObject(ctx);
    if (JS_IsException(proto)) return -1;
    hl_js_set_function_list(ctx, proto, js_stream_methods,
        (int)(sizeof js_stream_methods / sizeof js_stream_methods[0]));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue ai = JS_GetPropertyStr(ctx, sym, "asyncIterator");
    if (JS_IsSymbol(ai)) {
        JSAtom atom = JS_ValueToAtom(ctx, ai);
        JS_DefinePropertyValue(ctx, proto, atom,
            JS_NewCFunction(ctx, js_stream_async_iterator, "[Symbol.asyncIterator]", 0),
            JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, ai);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, global);
    hl_js_set_class_proto_frozen(ctx, hl_js_exec_stream_class_id, proto);
    return 0;
}

/* Under hull test: the stream test.execStub registered for @p name, after
 * the same match. Its chunks and result are queued at once, so nothing
 * waits (a request test.get dispatches may not). */
static JSValue stub_stream(JSContext *ctx, JSValueConst stubs, JSValueConst name_v,
                           const char *name, const char *const *args, const size_t *lens,
                           size_t n, const uint8_t *in, size_t in_len, int stdin_open,
                           const HlExecCmd *cmd)
{
    JSValue stub = JS_GetPropertyStr(ctx, stubs, name);
    if (JS_IsException(stub)) return stub;
    if (JS_IsUndefined(stub))
        return JS_ThrowTypeError(ctx, "exec.stream: no stub for '%s' in hull test "
                                 "(register one with test.execStub)", name);
    if (JS_IsFunction(ctx, stub)) {
        /* stub(args, name, stdin): stdin as an ArrayBuffer, or undefined */
        JSValue copy = JS_NewArray(ctx), stub_in = JS_UNDEFINED;
        int bad = JS_IsException(copy);
        for (size_t i = 0; !bad && i < n; i++) {
            JSValue sv = JS_NewStringLen(ctx, args[i], lens[i]);
            bad = JS_IsException(sv) ||
                  JS_DefinePropertyValueUint32(ctx, copy, (uint32_t)i, sv, JS_PROP_C_W_E) < 0;
        }
        if (!bad && in_len) {
            stub_in = JS_NewArrayBufferCopy(ctx, in, in_len);
            bad = JS_IsException(stub_in);
        }
        JSValue r = JS_EXCEPTION;
        if (!bad) {
            JSValue cargs[3] = { copy, JS_DupValue(ctx, name_v), stub_in };
            r = JS_Call(ctx, stub, JS_UNDEFINED, 3, cargs);
            JS_FreeValue(ctx, cargs[1]);
        }
        JS_FreeValue(ctx, copy);
        JS_FreeValue(ctx, stub_in);
        JS_FreeValue(ctx, stub);
        if (JS_IsException(r)) return r;
        stub = r;
    }
    if (!JS_IsObject(stub) || JS_IsFunction(ctx, stub)) {
        JS_FreeValue(ctx, stub);
        return JS_ThrowTypeError(ctx, "exec stub for '%s' must be (or give) a result "
                                 "object", name);
    }
    if (stream_register_class(ctx) != 0) { JS_FreeValue(ctx, stub); return JS_EXCEPTION; }
    JsExecStream *o = calloc(1, sizeof *o);
    if (!o) { JS_FreeValue(ctx, stub); return JS_ThrowOutOfMemory(ctx); }
    o->on_write = JS_UNDEFINED;
    JSValue obj = JS_NewObjectClass(ctx, (int)hl_js_exec_stream_class_id);
    if (JS_IsException(obj)) { free(o); JS_FreeValue(ctx, stub); return obj; }
    JS_SetOpaque(obj, o);              /* the finalizer frees o from here on */
    o->s = hl_exec_stream_stub(stdin_open, cmd->stream_stdin_max, in_len,
                               cmd->stream_stdout_max, cmd->stream_stderr_max);
    JSValue fail = JS_UNDEFINED;
    if (!o->s) {
        fail = JS_ThrowTypeError(ctx, "exec.stream('%s'): stream stubs need a host with "
                                 "the command broker (Linux, macOS)", name);
        goto bad;
    }
    {
        HlJS *hj = (HlJS *)JS_GetContextOpaque(ctx);
        hl_exec_stream_hold_on(o->s, &hj->wasm_buf_bytes, hj->max_heap_bytes);
    }
    JSValue chunks = JS_GetPropertyStr(ctx, stub, "chunks");
    if (JS_IsException(chunks)) goto bad_exc;
    int rc = JS_IsUndefined(chunks) ? 0 : stub_push_chunks(ctx, o->s, chunks, name);
    JS_FreeValue(ctx, chunks);
    if (rc != 0) goto bad_exc;
    int32_t status = 0, ms = 0;
    JSValue v = JS_GetPropertyStr(ctx, stub, "status");
    if (JS_IsException(v) || (!JS_IsUndefined(v) && JS_ToInt32(ctx, &status, v) != 0)) {
        JS_FreeValue(ctx, v);
        goto bad_exc;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, stub, "durationMs");
    if (JS_IsException(v) || (!JS_IsUndefined(v) && JS_ToInt32(ctx, &ms, v) != 0)) {
        JS_FreeValue(ctx, v);
        goto bad_exc;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, stub, "timedOut");
    if (JS_IsException(v)) goto bad_exc;
    int timed_out = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, stub, "idleTimedOut");
    if (JS_IsException(v)) goto bad_exc;
    if (JS_ToBool(ctx, v) > 0) timed_out = 2;   /* ended by its idle limit */
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, stub, "signal");
    if (JS_IsException(v)) goto bad_exc;
    int has_sig = !(JS_IsUndefined(v) || JS_IsNull(v));
    const char *sig = has_sig ? JS_ToCString(ctx, v) : NULL;
    JS_FreeValue(ctx, v);
    if (has_sig && !sig) goto bad_exc;
    rc = hl_exec_stream_stub_finish(o->s, status, sig, timed_out, (uint32_t)(ms < 0 ? 0 : ms));
    if (sig) JS_FreeCString(ctx, sig);
    if (rc != 0) { fail = JS_ThrowOutOfMemory(ctx); goto bad; }
    v = JS_GetPropertyStr(ctx, stub, "onWrite");
    if (JS_IsException(v)) goto bad_exc;
    if (!JS_IsUndefined(v) && !JS_IsFunction(ctx, v)) {
        JS_FreeValue(ctx, v);
        fail = JS_ThrowTypeError(ctx, "exec stub for '%s': onWrite must be a function", name);
        goto bad;
    }
    o->on_write = v;
    JS_FreeValue(ctx, stub);
    return obj;
bad_exc:
    fail = JS_EXCEPTION;
bad:
    JS_FreeValue(ctx, stub);
    JS_FreeValue(ctx, obj);
    return fail;
}

/* exec.stream(name, args?, opts?) -> stream (at once) */
static JSValue js_exec_stream(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "exec.stream(name, args?): name must be a string");
    char *bufs[HL_EXEC_MAX_CALL_ARGS];
    const char *args[HL_EXEC_MAX_CALL_ARGS];
    size_t lens[HL_EXEC_MAX_CALL_ARGS];
    size_t n = 0, total = 0;
    JSValue ret = JS_EXCEPTION;
    const char *name = NULL;
    size_t name_len = 0;
    uint8_t *in = NULL;
    size_t in_len = 0;
    int stdin_open = 0;

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (JS_IsArray(ctx, argv[1]) <= 0)
            return JS_ThrowTypeError(ctx, "exec.stream: args must be an array of strings");
        int32_t len = 0;
        if (hl_js_get_length32(ctx, argv[1], &len) != 0) return JS_EXCEPTION;
        if (len > HL_EXEC_MAX_CALL_ARGS)
            return JS_ThrowRangeError(ctx, "exec.stream: more than %d arguments",
                                      HL_EXEC_MAX_CALL_ARGS);
        for (int32_t i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
            if (JS_IsException(e)) goto out;
            if (!JS_IsString(e)) {
                JS_FreeValue(ctx, e);
                JS_ThrowTypeError(ctx, "exec.stream: argument %d must be a string", (int)i + 1);
                goto out;
            }
            size_t sl = 0;
            const char *str = JS_ToCStringLen(ctx, &sl, e);
            JS_FreeValue(ctx, e);
            if (!str) goto out;
            bufs[n] = js_malloc(ctx, sl + 1);
            if (!bufs[n]) { JS_FreeCString(ctx, str); goto out; }
            memcpy(bufs[n], str, sl + 1);
            JS_FreeCString(ctx, str);
            args[n] = bufs[n];
            lens[n] = sl;
            total += sl;
            n++;
        }
    }
    if (argc > 2 && read_opts_ex(ctx, argv[2], &in, &in_len, &stdin_open) != 0) goto out;
    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) goto out;
    if (hl_js_budget_charge(ctx, 64 + (total + in_len) / 8)) goto out;

    HlExecCmd cmd;
    char err[256];
    int rc = js && js->base.exec_policy
           ? hl_exec_check_call(js->base.exec_policy, js->base.exec_policy_len,
                                name, name_len, args, lens, n, &cmd, err, sizeof err)
           : HL_EXEC_E_UNKNOWN;
    if (rc == HL_EXEC_E_UNKNOWN) {
        JS_ThrowTypeError(ctx, "exec.stream: '%s' is not declared in the manifest's "
                          "exec section", name);
        goto out;
    }
    if (rc != HL_EXEC_OK) { JS_ThrowTypeError(ctx, "exec.stream('%s'): %s", name, err); goto out; }
    if (!(cmd.flags & HL_EXEC_CMD_STREAM)) {
        JS_ThrowTypeError(ctx, "exec.stream('%s'): not a stream command (declare "
                          "stream: true; exec.run runs it)", name);
        goto out;
    }
    if ((in_len || stdin_open) && cmd.stream_stdin_max == 0) {
        JS_ThrowTypeError(ctx, "exec.stream('%s'): the command takes no stdin "
                          "(declare stdinMax)", name);
        goto out;
    }
    if (in_len > cmd.stream_stdin_max) {
        JS_ThrowTypeError(ctx, "exec.stream('%s'): stdin is longer than its stdinMax", name);
        goto out;
    }
    /* Its reads will wait: refused where a wait is. */
    if (js && (js->in_middleware || js->in_load)) {
        JS_ThrowTypeError(ctx, "exec.stream cannot wait %s", js->in_middleware
                          ? "in middleware (it is synchronous)" : "at the module top level");
        hl_js_refusal_tag(ctx, HL_REFUSAL_WAIT);
        goto out;
    }
#ifdef HL_ENABLE_DB
    if (hl_js_db_refuse_wait(ctx, "exec.stream") != 0) goto out;
#endif
    JSValue stubs = exec_stubs(ctx);
    if (JS_IsObject(stubs)) {           /* hull test: the stub answers */
        ret = stub_stream(ctx, stubs, argv[0], name, args, lens, n, in, in_len,
                          stdin_open, &cmd);
        JS_FreeValue(ctx, stubs);
        goto out;
    }
    JS_FreeValue(ctx, stubs);
    if (!js || !hl_exec_broker_running() || !js->base.async_ctx) {
        JS_ThrowTypeError(ctx, "exec.stream('%s'): the command broker is not running "
                          "(this host has none, or it was lost)", name);
        goto out;
    }
    if (hl_exec_inflight() >= HL_EXEC_MAX_INFLIGHT ||
        hl_exec_inflight_cmd(name, name_len) >= cmd.concurrency) {
        hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowTypeError(ctx, "exec.stream('%s'): "
                     "too many calls in flight", name));
        goto out;
    }
    if (stream_register_class(ctx) != 0) goto out;
    JsExecStream *o = calloc(1, sizeof *o);
    if (!o) { JS_ThrowOutOfMemory(ctx); goto out; }
    o->on_write = JS_UNDEFINED;
    JSValue obj = JS_NewObjectClass(ctx, (int)hl_js_exec_stream_class_id);
    if (JS_IsException(obj)) { free(o); goto out; }
    JS_SetOpaque(obj, o);              /* the finalizer frees o from here on */
    char why[256];
    o->s = hl_exec_stream_start(js->base.async_ctx, name, name_len, args, lens, n,
                                in, in_len, stdin_open, cmd.stream_stdin_max,
                                why, sizeof why);
    if (!o->s) {
        JS_FreeValue(ctx, obj);
        JS_ThrowTypeError(ctx, "exec.stream('%s'): %s", name, why);
        goto out;
    }
    hl_exec_stream_hold_on(o->s, &js->wasm_buf_bytes, js->max_heap_bytes);
    ret = obj;
out:
    if (name) JS_FreeCString(ctx, name);
    js_free(ctx, in);
    for (size_t i = 0; i < n; i++) js_free(ctx, bufs[i]);
    return ret;
}

static int js_exec_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/exec", "hull:exec") != 0) return -1;
    JSValue exec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, exec, "run", JS_NewCFunction(ctx, js_exec_run, "run", 2));
    JS_SetPropertyStr(ctx, exec, "stream", JS_NewCFunction(ctx, js_exec_stream, "stream", 2));
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
