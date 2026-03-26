/*
 * mod_smtp.c — hull:smtp module (email sending)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/smtp.h"

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

    /* Build result early for validation errors */
    JSValue result;

    if (!host || !from || !to || !subject || !body) {
        result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "ok", JS_FALSE);
        const char *missing = !host ? "host" : !from ? "from" :
                              !to ? "to" : !subject ? "subject" : "body";
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "%s required", missing);
        JS_SetPropertyStr(ctx, result, "error", JS_NewString(ctx, errbuf));
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

    const char *err_msg = NULL;
    int rc = hl_cap_smtp_send(js->base.smtp_cfg, &msg, &err_msg);

    result = JS_NewObject(ctx);
    if (rc == 0) {
        JS_SetPropertyStr(ctx, result, "ok", JS_TRUE);
    } else {
        JS_SetPropertyStr(ctx, result, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, result, "error",
                          JS_NewString(ctx, err_msg ? err_msg
                                                    : "smtp send failed"));
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
