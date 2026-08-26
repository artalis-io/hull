/*
 * mod_time.c - hull:time module (timestamps, date formatting)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/time.h"

static JSValue js_time_now(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, hl_cap_time_now());
}

static JSValue js_time_now_ms(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, hl_cap_time_now_ms());
}

static JSValue js_time_clock(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, hl_cap_time_clock());
}

static JSValue js_time_date(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char buf[16];
    if (hl_cap_time_date(buf, sizeof(buf)) != 0)
        return JS_ThrowInternalError(ctx, "time.date() failed");
    return JS_NewString(ctx, buf);
}

static JSValue js_time_datetime(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char buf[32];
    if (hl_cap_time_datetime(buf, sizeof(buf)) != 0)
        return JS_ThrowInternalError(ctx, "time.datetime() failed");
    return JS_NewString(ctx, buf);
}

static int js_time_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/time", "hull:time") != 0) return -1;

    JSValue time_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, time_obj, "now",
                      JS_NewCFunction(ctx, js_time_now, "now", 0));
    JS_SetPropertyStr(ctx, time_obj, "nowMs",
                      JS_NewCFunction(ctx, js_time_now_ms, "nowMs", 0));
    JS_SetPropertyStr(ctx, time_obj, "clock",
                      JS_NewCFunction(ctx, js_time_clock, "clock", 0));
    JS_SetPropertyStr(ctx, time_obj, "date",
                      JS_NewCFunction(ctx, js_time_date, "date", 0));
    JS_SetPropertyStr(ctx, time_obj, "datetime",
                      JS_NewCFunction(ctx, js_time_datetime, "datetime", 0));
    JS_SetModuleExport(ctx, m, "time", time_obj);
    return 0;
}

int hl_js_init_time_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:time", js_time_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "time");
    return 0;
}
