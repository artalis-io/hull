/*
 * js_modules.c — hull:* built-in module implementations for QuickJS
 *
 * Each module is registered as a native C module via JS_NewCModule().
 * All capability calls go through hl_cap_* — no direct SQLite,
 * filesystem, or network access from this file.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "hull/limits.h"
#include "hull/cap/db.h"
#include "hull/cap/time.h"
#include "hull/cap/env.h"
#include "hull/cap/crypto.h"
#include "quickjs.h"

#include "log.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════
 * hull:app module
 *
 * Provides route registration: app.get(), app.post(), app.use(), etc.
 * Routes are stored in globalThis.__hull_routes (array of functions)
 * and globalThis.__hull_route_defs (array of {method, pattern} objects)
 * for the C router to consume at startup.
 * ════════════════════════════════════════════════════════════════════ */

/* Helper: register a route with given method string */
static JSValue js_app_route(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    static const char *method_names[] = {
        "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "*"
    };

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.%s requires (pattern, handler)",
                                 method_names[magic]);

    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!pattern)
        return JS_EXCEPTION;

    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, pattern);
        return JS_ThrowTypeError(ctx, "handler must be a function");
    }

    JSValue global = JS_GetGlobalObject(ctx);

    /* Ensure __hull_routes array exists */
    JSValue routes = JS_GetPropertyStr(ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes)) {
        routes = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_routes", JS_DupValue(ctx, routes));
    }

    /* Ensure __hull_route_defs array exists */
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");
    if (JS_IsUndefined(defs)) {
        defs = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_route_defs", JS_DupValue(ctx, defs));
    }

    /* Get current length (= next index) */
    JSValue len_val = JS_GetPropertyStr(ctx, routes, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    /* Store handler function */
    JS_SetPropertyUint32(ctx, routes, (uint32_t)idx, JS_DupValue(ctx, argv[1]));

    /* Store route definition */
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "method",
                      JS_NewString(ctx, method_names[magic]));
    JS_SetPropertyStr(ctx, def, "pattern", JS_NewString(ctx, pattern));
    JS_SetPropertyStr(ctx, def, "handler_id", JS_NewInt32(ctx, idx));
    JS_SetPropertyUint32(ctx, defs, (uint32_t)idx, def);

    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, routes);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, pattern);

    return JS_UNDEFINED;
}

/* app.use(method, pattern, handler) — middleware registration */
static JSValue js_app_use(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "app.use requires (method, pattern, handler)");

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *pattern = JS_ToCString(ctx, argv[1]);
    if (!method || !pattern || !JS_IsFunction(ctx, argv[2])) {
        if (method) JS_FreeCString(ctx, method);
        if (pattern) JS_FreeCString(ctx, pattern);
        return JS_ThrowTypeError(ctx, "app.use requires (method, pattern, handler)");
    }

    JSValue global = JS_GetGlobalObject(ctx);

    /* Store in __hull_middleware array */
    JSValue mw = JS_GetPropertyStr(ctx, global, "__hull_middleware");
    if (JS_IsUndefined(mw)) {
        mw = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_middleware", JS_DupValue(ctx, mw));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, mw, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "method", JS_NewString(ctx, method));
    JS_SetPropertyStr(ctx, entry, "pattern", JS_NewString(ctx, pattern));
    JS_SetPropertyStr(ctx, entry, "handler", JS_DupValue(ctx, argv[2]));
    JS_SetPropertyUint32(ctx, mw, (uint32_t)idx, entry);

    JS_FreeValue(ctx, mw);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, pattern);
    JS_FreeCString(ctx, method);

    return JS_UNDEFINED;
}

/* app.config(obj) — application configuration */
static JSValue js_app_config(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "app.config requires an object");

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__hull_config", JS_DupValue(ctx, argv[0]));
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* app.manifest(obj) — declare application capabilities */
static JSValue js_app_manifest(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "app.manifest requires an object");

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__hull_manifest", JS_DupValue(ctx, argv[0]));
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* app.getManifest() — retrieve the stored manifest object */
static JSValue js_app_get_manifest(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue manifest = JS_GetPropertyStr(ctx, global, "__hull_manifest");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(manifest))
        return JS_NULL;
    return manifest;
}

/* app.static(prefix, directory) — static file serving */
static JSValue js_app_static(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.static requires (prefix, directory)");

    /* Store static config for C router to process */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue statics = JS_GetPropertyStr(ctx, global, "__hull_statics");
    if (JS_IsUndefined(statics)) {
        statics = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_statics", JS_DupValue(ctx, statics));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, statics, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "prefix", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, entry, "directory", JS_DupValue(ctx, argv[1]));
    JS_SetPropertyUint32(ctx, statics, (uint32_t)idx, entry);

    JS_FreeValue(ctx, statics);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

static int js_app_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue app = JS_NewObject(ctx);

    /* Route methods: magic encodes the HTTP method index */
    JS_SetPropertyStr(ctx, app, "get",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "get", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, app, "post",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "post", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, app, "put",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "put", 2, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, app, "del",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "del", 2, JS_CFUNC_generic_magic, 3));
    JS_SetPropertyStr(ctx, app, "patch",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "patch", 2, JS_CFUNC_generic_magic, 4));

    JS_SetPropertyStr(ctx, app, "use",
                      JS_NewCFunction(ctx, js_app_use, "use", 3));
    JS_SetPropertyStr(ctx, app, "config",
                      JS_NewCFunction(ctx, js_app_config, "config", 1));
    JS_SetPropertyStr(ctx, app, "static",
                      JS_NewCFunction(ctx, js_app_static, "static", 2));
    JS_SetPropertyStr(ctx, app, "manifest",
                      JS_NewCFunction(ctx, js_app_manifest, "manifest", 1));
    JS_SetPropertyStr(ctx, app, "getManifest",
                      JS_NewCFunction(ctx, js_app_get_manifest, "getManifest", 0));

    JS_SetModuleExport(ctx, m, "app", app);
    return 0;
}

int hl_js_init_app_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:app", js_app_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "app");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:db module
 *
 * db.query(sql, params?) → array of row objects
 * db.exec(sql, params?)  → number of rows affected
 * db.lastId()            → last insert rowid
 * ════════════════════════════════════════════════════════════════════ */

/* Callback context for building JS result array from hl_cap_db_query */
typedef struct {
    JSContext *ctx;
    JSValue    array;
    int32_t    row_count;
} JsQueryCtx;

static int js_query_row_cb(void *opaque, HlColumn *cols, int ncols)
{
    JsQueryCtx *qc = (JsQueryCtx *)opaque;

    JSValue row = JS_NewObject(qc->ctx);
    for (int i = 0; i < ncols; i++) {
        JSValue val;
        switch (cols[i].value.type) {
        case HL_TYPE_INT:
            val = JS_NewInt64(qc->ctx, cols[i].value.i);
            break;
        case HL_TYPE_DOUBLE:
            val = JS_NewFloat64(qc->ctx, cols[i].value.d);
            break;
        case HL_TYPE_TEXT:
            val = JS_NewStringLen(qc->ctx, cols[i].value.s,
                                  cols[i].value.len);
            break;
        case HL_TYPE_BLOB:
            val = JS_NewArrayBufferCopy(qc->ctx,
                                         (const uint8_t *)cols[i].value.s,
                                         cols[i].value.len);
            break;
        case HL_TYPE_BOOL:
            val = JS_NewBool(qc->ctx, cols[i].value.b);
            break;
        case HL_TYPE_NIL:
        default:
            val = JS_NULL;
            break;
        }
        JS_SetPropertyStr(qc->ctx, row, cols[i].name, val);
    }

    JS_SetPropertyUint32(qc->ctx, qc->array, (uint32_t)qc->row_count, row);
    qc->row_count++;
    return 0;
}

/* Marshal JS values to HlValue array for parameter binding */
static int js_to_hl_values(JSContext *ctx, JSValueConst arr,
                              HlValue **out_params, int *out_count)
{
    *out_params = NULL;
    *out_count = 0;

    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;

    if (!JS_IsArray(ctx, arr))
        return -1;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    if (len <= 0)
        return 0;

    /* Overflow guard */
    if ((size_t)len > SIZE_MAX / sizeof(HlValue))
        return -1;

    HlValue *params = js_mallocz(ctx, (size_t)len * sizeof(HlValue));
    if (!params)
        return -1;

    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        int tag = JS_VALUE_GET_NORM_TAG(v);

        switch (tag) {
        case JS_TAG_INT:
            params[i].type = HL_TYPE_INT;
            params[i].i = JS_VALUE_GET_INT(v);
            break;
        case JS_TAG_FLOAT64: {
            double d;
            JS_ToFloat64(ctx, &d, v);
            params[i].type = HL_TYPE_DOUBLE;
            params[i].d = d;
            break;
        }
        case JS_TAG_STRING: {
            size_t slen;
            const char *s = JS_ToCStringLen(ctx, &slen, v);
            params[i].type = HL_TYPE_TEXT;
            params[i].s = s; /* kept alive until JS_FreeCString */
            params[i].len = slen;
            break;
        }
        case JS_TAG_BOOL:
            params[i].type = HL_TYPE_BOOL;
            params[i].b = JS_VALUE_GET_BOOL(v);
            break;
        case JS_TAG_NULL:
        case JS_TAG_UNDEFINED:
        default:
            params[i].type = HL_TYPE_NIL;
            break;
        }
        JS_FreeValue(ctx, v);
    }

    *out_params = params;
    *out_count = len;
    return 0;
}

static void js_free_hl_values(JSContext *ctx, HlValue *params, int count)
{
    if (!params)
        return;
    /* Free any strings we borrowed via JS_ToCStringLen */
    for (int i = 0; i < count; i++) {
        if (params[i].type == HL_TYPE_TEXT && params[i].s)
            JS_FreeCString(ctx, params[i].s);
    }
    js_free(ctx, params);
}

/* db.query(sql, params?) */
static JSValue js_db_query(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->db)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.query requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    HlValue *params = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (js_to_hl_values(ctx, argv[1], &params, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "params must be an array");
        }
    }

    JsQueryCtx qc = {
        .ctx = ctx,
        .array = JS_NewArray(ctx),
        .row_count = 0,
    };

    int rc = hl_cap_db_query(js->db, sql, params, nparams,
                               js_query_row_cb, &qc, js->alloc);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc != 0) {
        JS_FreeValue(ctx, qc.array);
        return JS_ThrowInternalError(ctx, "query failed: %s",
                                     sqlite3_errmsg(js->db));
    }

    return qc.array;
}

/* db.exec(sql, params?) */
static JSValue js_db_exec(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->db)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.exec requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    HlValue *params = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (js_to_hl_values(ctx, argv[1], &params, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "params must be an array");
        }
    }

    int rc = hl_cap_db_exec(js->db, sql, params, nparams);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc < 0)
        return JS_ThrowInternalError(ctx, "exec failed: %s",
                                     sqlite3_errmsg(js->db));

    return JS_NewInt32(ctx, rc);
}

/* db.lastId() */
static JSValue js_db_last_id(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->db)
        return JS_ThrowInternalError(ctx, "database not available");

    return JS_NewInt64(ctx, hl_cap_db_last_id(js->db));
}

static int js_db_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue db = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, db, "query",
                      JS_NewCFunction(ctx, js_db_query, "query", 2));
    JS_SetPropertyStr(ctx, db, "exec",
                      JS_NewCFunction(ctx, js_db_exec, "exec", 2));
    JS_SetPropertyStr(ctx, db, "lastId",
                      JS_NewCFunction(ctx, js_db_last_id, "lastId", 0));
    JS_SetModuleExport(ctx, m, "db", db);
    return 0;
}

int hl_js_init_db_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:db", js_db_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "db");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:time module
 *
 * time.now()      → Unix timestamp (seconds)
 * time.nowMs()    → milliseconds since epoch
 * time.clock()    → monotonic ms
 * time.date()     → "YYYY-MM-DD"
 * time.datetime() → "YYYY-MM-DDTHH:MM:SSZ"
 * ════════════════════════════════════════════════════════════════════ */

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

/* ════════════════════════════════════════════════════════════════════
 * hull:env module
 *
 * env.get(name) → string or null
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_env_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->env_cfg)
        return JS_ThrowInternalError(ctx, "env not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "env.get requires (name)");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    const char *val = hl_cap_env_get(js->env_cfg, name);
    JS_FreeCString(ctx, name);

    if (val)
        return JS_NewString(ctx, val);
    return JS_NULL;
}

static int js_env_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue env = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, env, "get",
                      JS_NewCFunction(ctx, js_env_get, "get", 1));
    JS_SetModuleExport(ctx, m, "env", env);
    return 0;
}

int hl_js_init_env_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:env", js_env_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "env");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:crypto module
 *
 * crypto.sha256(data)          → hex string
 * crypto.random(n)             → ArrayBuffer of n random bytes
 * crypto.hashPassword(pw)      → hash string
 * crypto.verifyPassword(pw, h) → boolean
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_crypto_sha256(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.sha256 requires (data)");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data)
        return JS_EXCEPTION;

    uint8_t hash[32];
    if (hl_cap_crypto_sha256(data, len, hash) != 0) {
        JS_FreeCString(ctx, data);
        return JS_ThrowInternalError(ctx, "sha256 failed");
    }
    JS_FreeCString(ctx, data);

    /* Convert to hex string */
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = '\0';

    return JS_NewString(ctx, hex);
}

static JSValue js_crypto_random(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.random requires (n)");

    int32_t n;
    if (JS_ToInt32(ctx, &n, argv[0]))
        return JS_EXCEPTION;

    if (n <= 0 || n > HL_RANDOM_MAX_BYTES)
        return JS_ThrowRangeError(ctx, "random bytes must be 1-%d",
                                  HL_RANDOM_MAX_BYTES);

    uint8_t *buf = js_malloc(ctx, (size_t)n);
    if (!buf)
        return JS_EXCEPTION;

    if (hl_cap_crypto_random(buf, (size_t)n) != 0) {
        js_free(ctx, buf);
        return JS_ThrowInternalError(ctx, "random failed");
    }

    /* Copy into ArrayBuffer and free temp */
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)n);
    js_free(ctx, buf);
    return ab;
}

/* Hex nibble helper (no sscanf — Cosmopolitan compat) */
static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* crypto.hashPassword(password) → "pbkdf2:iterations:salt_hex:hash_hex" */
static JSValue js_crypto_hash_password(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.hashPassword requires (password)");

    size_t pw_len;
    const char *pw = JS_ToCStringLen(ctx, &pw_len, argv[0]);
    if (!pw)
        return JS_EXCEPTION;

    /* Generate 16-byte salt */
    uint8_t salt[16];
    if (hl_cap_crypto_random(salt, sizeof(salt)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_ThrowInternalError(ctx, "random failed");
    }

    /* PBKDF2-HMAC-SHA256, 32-byte output */
    uint8_t hash[32];
    int iterations = HL_PBKDF2_ITERATIONS;
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                               iterations, hash, sizeof(hash)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_ThrowInternalError(ctx, "pbkdf2 failed");
    }
    JS_FreeCString(ctx, pw);

    /* Format: "pbkdf2:100000:salt_hex:hash_hex" */
    char salt_hex[33], hash_hex[65];
    for (int i = 0; i < 16; i++)
        snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);

    char result[128];
    snprintf(result, sizeof(result), "pbkdf2:%d:%s:%s",
             iterations, salt_hex, hash_hex);

    return JS_NewString(ctx, result);
}

/* crypto.verifyPassword(password, hash_string) → boolean */
static JSValue js_crypto_verify_password(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.verifyPassword requires (password, hash)");

    size_t pw_len;
    const char *pw = JS_ToCStringLen(ctx, &pw_len, argv[0]);
    const char *stored = JS_ToCString(ctx, argv[1]);
    if (!pw || !stored) {
        if (pw) JS_FreeCString(ctx, pw);
        if (stored) JS_FreeCString(ctx, stored);
        return JS_EXCEPTION;
    }

    /* Parse "pbkdf2:iterations:salt_hex:hash_hex" manually (no scansets
     * — Cosmopolitan libc doesn't support sscanf %[...] scansets). */
    if (strncmp(stored, "pbkdf2:", 7) != 0) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    const char *p = stored + 7;

    char *end = NULL;
    long iterations = strtol(p, &end, 10);
    if (!end || *end != ':' || iterations <= 0) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    p = end + 1;

    if (strlen(p) < 32 + 1 + 64 || p[32] != ':') {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    char salt_hex[33];
    memcpy(salt_hex, p, 32);
    salt_hex[32] = '\0';
    p += 33;

    if (strlen(p) < 64) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    char hash_hex[65];
    memcpy(hash_hex, p, 64);
    hash_hex[64] = '\0';

    JS_FreeCString(ctx, stored);

    /* Decode hex salt (manual — sscanf %x broken on Cosmopolitan) */
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble((unsigned char)salt_hex[i * 2]);
        int lo = hex_nibble((unsigned char)salt_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { JS_FreeCString(ctx, pw); return JS_FALSE; }
        salt[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                               (int)iterations, computed, sizeof(computed)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_FALSE;
    }
    JS_FreeCString(ctx, pw);

    /* Decode stored hash and compare (constant-time) */
    uint8_t stored_hash[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble((unsigned char)hash_hex[i * 2]);
        int lo = hex_nibble((unsigned char)hash_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return JS_FALSE;
        stored_hash[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Constant-time comparison */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed[i] ^ stored_hash[i];

    return diff == 0 ? JS_TRUE : JS_FALSE;
}

static int js_crypto_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "sha256",
                      JS_NewCFunction(ctx, js_crypto_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto, "random",
                      JS_NewCFunction(ctx, js_crypto_random, "random", 1));
    JS_SetPropertyStr(ctx, crypto, "hashPassword",
                      JS_NewCFunction(ctx, js_crypto_hash_password, "hashPassword", 1));
    JS_SetPropertyStr(ctx, crypto, "verifyPassword",
                      JS_NewCFunction(ctx, js_crypto_verify_password, "verifyPassword", 2));
    JS_SetModuleExport(ctx, m, "crypto", crypto);
    return 0;
}

int hl_js_init_crypto_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:crypto", js_crypto_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "crypto");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:json module
 *
 * Wraps the built-in JSON.stringify/JSON.parse as:
 *   json.encode(value) → string
 *   json.decode(str)   → value
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_json_encode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "json.encode requires (value)");

    JSValue result = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(result))
        return JS_EXCEPTION;
    /* JSON.stringify returns undefined for unsupported types */
    if (JS_IsUndefined(result))
        return JS_NewString(ctx, "null");
    return result;
}

static JSValue js_json_decode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "json.decode requires (str)");

    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    JSValue result = JS_ParseJSON(ctx, str, strlen(str), "<json>");
    JS_FreeCString(ctx, str);
    return result;
}

static int js_json_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue json = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, json, "encode",
                      JS_NewCFunction(ctx, js_json_encode, "encode", 1));
    JS_SetPropertyStr(ctx, json, "decode",
                      JS_NewCFunction(ctx, js_json_decode, "decode", 1));
    JS_SetModuleExport(ctx, m, "json", json);
    return 0;
}

int hl_js_init_json_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:json", js_json_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "json");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:log module (placeholder — full implementation needs hl_cap_db)
 *
 * log.info(msg)  → logs to stderr (and DB when available)
 * log.warn(msg)
 * log.error(msg)
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_log_level(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    static const int levels[] = { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_DEBUG };
    int level = (magic >= 0 && magic < 4) ? levels[magic] : LOG_INFO;

    /* Detect stdlib vs app: hull:* modules → [hull:js], else [app] */
    const char *tag = "[app]";
    const char *mod = NULL;
    JSAtom mod_atom = JS_GetScriptOrModuleName(ctx, 1);
    if (mod_atom != JS_ATOM_NULL) {
        mod = JS_AtomToCString(ctx, mod_atom);
        JS_FreeAtom(ctx, mod_atom);
    }
    if (mod && strncmp(mod, "hull:", 5) == 0)
        tag = "[hull:js]";

    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            log_log(level, mod ? mod : "js", 0, "%s %s", tag, str);
            JS_FreeCString(ctx, str);
        }
    }
    if (mod) JS_FreeCString(ctx, mod);
    return JS_UNDEFINED;
}

static int js_log_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue log = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, log, "info",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "info", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, log, "warn",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "warn", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, log, "error",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "error", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, log, "debug",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "debug", 1, JS_CFUNC_generic_magic, 3));
    JS_SetModuleExport(ctx, m, "log", log);
    return 0;
}

int hl_js_init_log_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:log", js_log_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "log");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Module registry — called by hl_js_init() to register all
 * hull:* built-in modules.
 * ════════════════════════════════════════════════════════════════════ */

int hl_js_register_modules(HlJS *js)
{
    if (!js || !js->ctx)
        return -1;

    /* Register hull:app module */
    if (hl_js_init_app_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:db module (only if database is available) */
    if (js->db) {
        if (hl_js_init_db_module(js->ctx, js) != 0)
            return -1;
    }

    /* Register hull:json module */
    if (hl_js_init_json_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:time module */
    if (hl_js_init_time_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:env module */
    if (hl_js_init_env_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:crypto module */
    if (hl_js_init_crypto_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:log module */
    if (hl_js_init_log_module(js->ctx, js) != 0)
        return -1;

    return 0;
}
