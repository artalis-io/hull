/*
 * mod_smtp.c - hull:smtp module (email sending)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/smtp.h"
#include "hull/cap/smtp_op.h"
#include "hull/cap/smtp_async.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"

#include <stdio.h>

/* Helper: extract JS string array from an Array object */
static int js_get_string_array(JSContext *ctx, JSValueConst arr,
                               const char ***out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    if (len <= 0)
        return 0;

    const char **strs = js_mallocz(ctx, (size_t)len * sizeof(const char *));
    if (!strs)
        return -1;

    int count = 0;
    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        if (JS_IsString(v)) {
            strs[count] = JS_ToCString(ctx, v);
            if (strs[count])
                count++;
        }
        JS_FreeValue(ctx, v);
    }

    *out = strs;
    *out_count = count;
    return 0;
}

static void js_free_string_array(JSContext *ctx, const char **strs, int count)
{
    if (!strs) return;
    for (int i = 0; i < count; i++) {
        if (strs[i])
            JS_FreeCString(ctx, strs[i]);
    }
    js_free(ctx, strs);
}

/* Build the { ok, error? } SMTP result object. */
static JSValue js_smtp_result_obj(JSContext *ctx, int ok, const char *err)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "ok", ok ? JS_TRUE : JS_FALSE);
    if (!ok)
        JS_SetPropertyStr(ctx, o, "error",
                          JS_NewString(ctx, err ? err : "smtp send failed"));
    return o;
}

/* Wrap @p value in an already-resolved Promise (consumes @p value). Every SMTP
 * outcome - sync no-loop, immediate scheduling failure, validation failure - is
 * returned this way so smtp.send ALWAYS returns a Promise resolving to {ok,error}.
 * Returns JS_EXCEPTION only if the promise capability itself cannot be created. */
static JSValue js_resolved_promise(JSContext *ctx, JSValue value)
{
    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, value); return JS_EXCEPTION; }
    JSValue r = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, (JSValueConst *)&value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, rf[0]);
    JS_FreeValue(ctx, rf[1]);
    return promise;
}

static JSValue js_resolved_result(JSContext *ctx, int ok, const char *err)
{
    return js_resolved_promise(ctx, js_smtp_result_obj(ctx, ok, err));
}

/* Async resume push_result: read the published terminal (audit once) and produce
 * the { ok, error } value the cont resolves the Promise with. driver is the
 * HlSmtpAsyncOp. No JSValue/JSContext ever crosses to the worker; this runs on the
 * event-loop thread at resume. */
static JSValue js_push_smtp_result(JSContext *ctx, void *driver)
{
    HlSmtpResult r;
    hl_smtp_async_finish((HlSmtpAsyncOp *)driver, &r);
    return js_smtp_result_obj(ctx, r.rc == 0, r.token);
}

/* smtp.send(opts) */
static JSValue js_smtp_send(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.smtp_cfg)
        return JS_ThrowInternalError(ctx, "smtp not configured (no hosts in manifest)");

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "smtp.send requires an options object");

    JSValueConst opts = argv[0];

    /* Extract fields */
    JSValue v_host = JS_GetPropertyStr(ctx, opts, "host");
    JSValue v_port = JS_GetPropertyStr(ctx, opts, "port");
    JSValue v_user = JS_GetPropertyStr(ctx, opts, "username");
    JSValue v_pass = JS_GetPropertyStr(ctx, opts, "password");
    JSValue v_tls  = JS_GetPropertyStr(ctx, opts, "tls");
    JSValue v_from = JS_GetPropertyStr(ctx, opts, "from");
    JSValue v_to   = JS_GetPropertyStr(ctx, opts, "to");
    JSValue v_subj = JS_GetPropertyStr(ctx, opts, "subject");
    JSValue v_body = JS_GetPropertyStr(ctx, opts, "body");
    JSValue v_ct   = JS_GetPropertyStr(ctx, opts, "content_type");
    JSValue v_rto  = JS_GetPropertyStr(ctx, opts, "reply_to");
    JSValue v_cc   = JS_GetPropertyStr(ctx, opts, "cc");

    const char *host = JS_IsString(v_host) ? JS_ToCString(ctx, v_host) : NULL;
    int32_t port = 587;
    if (JS_IsNumber(v_port))
        JS_ToInt32(ctx, &port, v_port);
    const char *username = JS_IsString(v_user) ? JS_ToCString(ctx, v_user) : NULL;
    const char *password = JS_IsString(v_pass) ? JS_ToCString(ctx, v_pass) : NULL;

    int use_tls = 0;
    if (JS_IsBool(v_tls)) {
        use_tls = JS_ToBool(ctx, v_tls) ? 1 : 0;
    } else if (JS_IsNumber(v_tls)) {
        int32_t t = 0;
        JS_ToInt32(ctx, &t, v_tls);
        use_tls = t;
    }

    const char *from = JS_IsString(v_from) ? JS_ToCString(ctx, v_from) : NULL;
    const char *to   = JS_IsString(v_to)   ? JS_ToCString(ctx, v_to)   : NULL;
    const char *subject = JS_IsString(v_subj) ? JS_ToCString(ctx, v_subj) : NULL;
    const char *body = JS_IsString(v_body) ? JS_ToCString(ctx, v_body) : NULL;
    const char *content_type = JS_IsString(v_ct) ? JS_ToCString(ctx, v_ct) : NULL;
    const char *reply_to = JS_IsString(v_rto) ? JS_ToCString(ctx, v_rto) : NULL;

    /* CC array */
    const char **cc = NULL;
    int cc_count = 0;
    if (JS_IsArray(ctx, v_cc))
        js_get_string_array(ctx, v_cc, &cc, &cc_count);

    /* Free JS values */
    JS_FreeValue(ctx, v_host);
    JS_FreeValue(ctx, v_port);
    JS_FreeValue(ctx, v_user);
    JS_FreeValue(ctx, v_pass);
    JS_FreeValue(ctx, v_tls);
    JS_FreeValue(ctx, v_from);
    JS_FreeValue(ctx, v_to);
    JS_FreeValue(ctx, v_subj);
    JS_FreeValue(ctx, v_body);
    JS_FreeValue(ctx, v_ct);
    JS_FreeValue(ctx, v_rto);
    JS_FreeValue(ctx, v_cc);

    /* smtp.send ALWAYS returns a Promise for SMTP outcomes (resolving to
     * {ok,error}); it rejects/throws only for argument/type/binding failures. */
    JSValue result;

    /* Missing required field: an SMTP-outcome-style {ok:false} (parity with Lua),
     * returned as an already-resolved Promise - NOT a reject. */
    if (!host || !from || !to || !subject || !body) {
        const char *missing = !host ? "host" : !from ? "from" :
                              !to ? "to" : !subject ? "subject" : "body";
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "%s required", missing);
        result = js_resolved_result(ctx, 0, errbuf);
        goto cleanup;
    }

    /* Build message struct */
    HlSmtpMessage msg = {
        .host = host,
        .port = port,
        .username = username,
        .password = password,
        .use_tls = use_tls,
        .from = from,
        .to = to,
        .cc = cc,
        .cc_count = cc_count,
        .reply_to = reply_to,
        .subject = subject,
        .body = body,
        .content_type = content_type,
    };

    /* No active event loop (app.main CLI / in-process test harness): synchronous
     * model 1, returned as an already-resolved Promise. The ONLY sync path; we
     * never fall back to it merely because admission or the pool is unavailable. */
    if (!js->base.async_ctx || !js->base.smtp_async) {
        const char *err_msg = NULL;
        int rc = hl_cap_smtp_send(js->base.smtp_cfg, &msg, &err_msg);
        result = js_resolved_result(ctx, rc == 0, err_msg);
        goto cleanup;
    }

    /* Active loop: model 2 (regardless of cap). Authorize the host on the submit
     * side (audited once here) before any reservation or worker submission. */
    if (hl_smtp_check_host(js->base.smtp_cfg, msg.host) != 0) {
        hl_smtp_audit_denied(&msg);
        result = js_resolved_result(ctx, 0, "host_not_allowed");
        goto cleanup;
    }

    /* Deep-copy the message into an owned op (crosses to the worker; no JS
     * value/context ever does). */
    int to_ms = js->base.smtp_cfg->timeout_ms > 0 ? js->base.smtp_cfg->timeout_ms : 0;
    HlSmtpOp *op = hl_smtp_op_create(&msg, to_ms);
    if (!op) { result = js_resolved_result(ctx, 0, "connect_failed"); goto cleanup; }

    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx,
                                           js->base.alloc);
    if (!actx) { hl_smtp_op_free(op);
                 result = js_resolved_result(ctx, 0, "connect_failed"); goto cleanup; }

    /* One promise, created up front and returned; resolved either by the cont (on
     * the async resume) or by us (on an immediate scheduling failure). We keep our
     * OWN references to resolve/reject and hand DUPLICATES to the cont, so exactly
     * one side resolves and both free their copies exactly once. */
    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) {
        hl_smtp_op_free(op); hl_async_ctx_free(actx);
        result = JS_EXCEPTION;   /* binding failure -> reject */
        goto cleanup;
    }
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *, JSValue, JSValue,
        HlAllocator *, JSValue (*)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js, JS_DupValue(ctx, rf[0]),
                                                JS_DupValue(ctx, rf[1]),
                                                js->base.alloc, js_push_smtp_result);
    if (!cont) {
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
        JS_FreeValue(ctx, promise);
        hl_smtp_op_free(op); hl_async_ctx_free(actx);
        result = JS_ThrowInternalError(ctx, "smtp.send: out of memory");
        goto cleanup;
    }
    actx->cont     = cont;
    actx->detached = (js->active_conn == NULL);

    HlSmtpAsyncReq areq = {
        .server     = js->base.smtp_async,
        .pool       = js->base.thread_pool,
        .net_ctx    = js->base.net_ctx,
        .actx       = actx,
        .req_handle = js->active_conn,
        .detached   = (js->active_conn == NULL),
        .inputs     = op,
    };
    HlSmtpAsyncOutcome aout;
    hl_smtp_async_submit(&areq, &aout);

    if (aout.disposition == HL_SMTP_ASYNC_SUSPENDED) {
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);  /* cont resolves later */
        result = promise;
        goto cleanup;
    }

    /* RESOLVED: the immediate scheduling failure was audited once in the
     * orchestration, which also tore down the unparked cont (freeing its dup of
     * rf). hl_js_async_cont_create had registered that cont as js->last_async_cont;
     * now that it is freed, clear the dangling pointer so the dispatch's
     * pending-handler path does not attach the outer handler promise to freed
     * memory (the handler's `await` on our resolved promise completes via the
     * microtask pump, no cont needed). */
    js->last_async_cont = NULL;

    /* Resolve the promise ourselves via our own rf copies, then free them. */
    {
        JSValue rv = js_smtp_result_obj(ctx, 0,
            aout.result.token ? aout.result.token : "connect_failed");
        JSValue rr = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, (JSValueConst *)&rv);
        JS_FreeValue(ctx, rr); JS_FreeValue(ctx, rv);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
        result = promise;
    }

cleanup:
    if (host)         JS_FreeCString(ctx, host);
    if (username)     JS_FreeCString(ctx, username);
    if (password)     JS_FreeCString(ctx, password);
    if (from)         JS_FreeCString(ctx, from);
    if (to)           JS_FreeCString(ctx, to);
    if (subject)      JS_FreeCString(ctx, subject);
    if (body)         JS_FreeCString(ctx, body);
    if (content_type) JS_FreeCString(ctx, content_type);
    if (reply_to)     JS_FreeCString(ctx, reply_to);
    js_free_string_array(ctx, cc, cc_count);

    return result;
}

static int js_smtp_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/smtp", "hull:smtp") != 0) return -1;

    JSValue smtp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, smtp, "send",
                      JS_NewCFunction(ctx, js_smtp_send, "send", 1));
    JS_SetModuleExport(ctx, m, "smtp", smtp);
    return 0;
}

int hl_js_init_smtp_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:smtp", js_smtp_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "smtp");
    return 0;
}
