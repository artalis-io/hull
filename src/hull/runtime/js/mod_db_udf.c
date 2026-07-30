/* mod_db_udf.c — hull.db UDF bindings (SQLite user-defined functions, JS).
 *
 * The per-runtime SQLite UDF bridge, split out of mod_db.c so the base runtime
 * archive (libhull_feature-js.a) carries ZERO sqlite3_* references (Phase C.2,
 * docs/sqlite_feature.md). Whole-archived into an app only when the SQLite
 * backend is reachable (libhull_feature-sqlite-js.a); provides the strong
 * hl_js_db_attach_udf override that mod_db.c's weak default replaces. UDFs
 * register into the SQLite VM via sqlite3_create_function and marshal
 * sqlite3_value/context, so this is the sole per-runtime sqlite3_* consumer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB
#ifdef HL_ENABLE_SQLITE

#include "mod_buffer.h"            /* get_hl_js, HlJS */
#include "mod_db.h"                /* js_call_handle, new_bound_subobject */
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"    /* hl_db_sqlite_raw */

#ifdef HL_ENABLE_WASM
#include "hull/cap/db_udf.h"
#include "hull/cap/wasm.h"
#endif

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* Context for JS scalar UDF trampoline */
typedef struct {
    JSContext *ctx;
    JSValue    func;  /* borrowed — prevent GC via ref counting */
    int       *alive; /* points to HlJS.udf_runtime_alive */
} JsScalarUdfCtx;

/* Context for JS aggregate UDF trampoline */
typedef struct {
    JSContext *ctx;
    JSValue    step_fn;
    JSValue    finalize_fn;
    int       *alive; /* points to HlJS.udf_runtime_alive */
} JsAggUdfCtx;

/* Per-group state for JS aggregates (via sqlite3_aggregate_context) */
typedef struct {
    JSValue ctx_obj;    /* JS_UNDEFINED = not initialized */
    int     initialized;
} JsAggGroupState;

/* Push sqlite3_value as a JS value */
static JSValue js_from_sqlite_value(JSContext *ctx, sqlite3_value *val)
{
    switch (sqlite3_value_type(val)) {
    case SQLITE_INTEGER:
        return JS_NewInt64(ctx, sqlite3_value_int64(val));
    case SQLITE_FLOAT:
        return JS_NewFloat64(ctx, sqlite3_value_double(val));
    case SQLITE_TEXT:
        return JS_NewStringLen(ctx, (const char *)sqlite3_value_text(val),
                               (size_t)sqlite3_value_bytes(val));
    case SQLITE_BLOB:
        return JS_NewArrayBufferCopy(ctx,
                                      (const uint8_t *)sqlite3_value_blob(val),
                                      (size_t)sqlite3_value_bytes(val));
    default: /* SQLITE_NULL */
        return JS_NULL;
    }
}

/* Convert JS value to sqlite3_result */
static void js_to_sqlite_result(JSContext *ctx, sqlite3_context *sctx,
                                 JSValue val)
{
    int tag = JS_VALUE_GET_NORM_TAG(val);
    switch (tag) {
    case JS_TAG_INT:
        sqlite3_result_int64(sctx, (sqlite3_int64)JS_VALUE_GET_INT(val));
        break;
    case JS_TAG_FLOAT64: {
        double d;
        JS_ToFloat64(ctx, &d, val);
        sqlite3_result_double(sctx, d);
        break;
    }
    case JS_TAG_STRING: {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, val);
        if (s) {
            sqlite3_result_text(sctx, s, (int)len, SQLITE_TRANSIENT);
            JS_FreeCString(ctx, s);
        } else {
            sqlite3_result_null(sctx);
        }
        break;
    }
    case JS_TAG_BOOL:
        sqlite3_result_int(sctx, JS_VALUE_GET_BOOL(val));
        break;
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
    default:
        sqlite3_result_null(sctx);
        break;
    }
}

/* Scalar JS UDF callback */
static void js_scalar_udf_func(sqlite3_context *sctx, int argc,
                                sqlite3_value **argv)
{
    JsScalarUdfCtx *udf = (JsScalarUdfCtx *)sqlite3_user_data(sctx);
    JSContext *ctx = udf->ctx;

    JSValue *args = NULL;
    if (argc > 0) {
        args = js_malloc(ctx, (size_t)argc * sizeof(JSValue));
        if (!args) {
            sqlite3_result_error_nomem(sctx);
            return;
        }
        for (int i = 0; i < argc; i++)
            args[i] = js_from_sqlite_value(ctx, argv[i]);
    }

    JSValue result = JS_Call(ctx, udf->func, JS_UNDEFINED, argc, args);

    for (int i = 0; i < argc; i++)
        JS_FreeValue(ctx, args[i]);
    js_free(ctx, args);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        sqlite3_result_error(sctx, err ? err : "JS UDF error", -1);
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
    } else {
        js_to_sqlite_result(ctx, sctx, result);
    }
    JS_FreeValue(ctx, result);
}

/* Destroy callback for scalar JS UDF */
static void js_scalar_udf_destroy(void *data)
{
    JsScalarUdfCtx *udf = (JsScalarUdfCtx *)data;
    if (!udf) return;
    /* Free the JS value only if the JS runtime is still alive
     * (explicit unregister). During sqlite3_close the runtime is dead. */
    if (udf->alive && *udf->alive && udf->ctx)
        JS_FreeValue(udf->ctx, udf->func);
    free(udf);
}

/* Aggregate JS UDF step callback */
static void js_agg_step_func(sqlite3_context *sctx, int argc,
                              sqlite3_value **argv)
{
    JsAggUdfCtx *udf = (JsAggUdfCtx *)sqlite3_user_data(sctx);
    JSContext *ctx = udf->ctx;
    JsAggGroupState *gs = (JsAggGroupState *)sqlite3_aggregate_context(
        sctx, (int)sizeof(JsAggGroupState));
    if (!gs) {
        sqlite3_result_error_nomem(sctx);
        return;
    }

    /* Create context object on first call for this group */
    if (!gs->initialized) {
        gs->ctx_obj = JS_NewObject(ctx);
        gs->initialized = 1;
    }

    /* Call step(ctx_obj, arg1, arg2, ...) */
    int total_args = argc + 1;
    JSValue *args = js_malloc(ctx, (size_t)total_args * sizeof(JSValue));
    if (!args) {
        sqlite3_result_error_nomem(sctx);
        return;
    }
    args[0] = JS_DupValue(ctx, gs->ctx_obj);
    for (int i = 0; i < argc; i++)
        args[i + 1] = js_from_sqlite_value(ctx, argv[i]);

    JSValue result = JS_Call(ctx, udf->step_fn, JS_UNDEFINED, total_args, args);

    for (int i = 0; i < total_args; i++)
        JS_FreeValue(ctx, args[i]);
    js_free(ctx, args);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        sqlite3_result_error(sctx, err ? err : "JS UDF step error", -1);
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
}

/* Aggregate JS UDF finalize callback */
static void js_agg_finalize_func(sqlite3_context *sctx)
{
    JsAggUdfCtx *udf = (JsAggUdfCtx *)sqlite3_user_data(sctx);
    JSContext *ctx = udf->ctx;
    JsAggGroupState *gs = (JsAggGroupState *)sqlite3_aggregate_context(sctx, 0);

    if (!gs || !gs->initialized) {
        sqlite3_result_null(sctx);
        return;
    }

    /* Call finalize(ctx_obj) -> result */
    JSValue arg = JS_DupValue(ctx, gs->ctx_obj);
    JSValue result = JS_Call(ctx, udf->finalize_fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        sqlite3_result_error(sctx, err ? err : "JS UDF finalize error", -1);
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
    } else {
        js_to_sqlite_result(ctx, sctx, result);
    }
    JS_FreeValue(ctx, result);

    /* Clean up group state */
    JS_FreeValue(ctx, gs->ctx_obj);
    gs->ctx_obj = JS_UNDEFINED;
    gs->initialized = 0;
}

/* Destroy callback for aggregate JS UDF */
static void js_agg_udf_destroy(void *data)
{
    JsAggUdfCtx *udf = (JsAggUdfCtx *)data;
    if (!udf) return;
    /* Free JS values only if the JS runtime is still alive */
    if (udf->alive && *udf->alive && udf->ctx) {
        JS_FreeValue(udf->ctx, udf->step_fn);
        JS_FreeValue(udf->ctx, udf->finalize_fn);
    }
    free(udf);
}

/* Parse UDF options from JS object */
static void js_parse_udf_opts(JSContext *ctx, JSValueConst opts,
                               int *deterministic, int *nargs,
                               int *aggregate, int64_t *gas,
                               uint32_t *heap, uint32_t *stack_sz)
{
    *deterministic = 0;
    *nargs = -1;
    *aggregate = 0;
    *gas = 0;
    *heap = 0;
    *stack_sz = 0;

    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return;

    JSValue v;

    v = JS_GetPropertyStr(ctx, opts, "deterministic");
    if (JS_IsBool(v)) *deterministic = JS_VALUE_GET_BOOL(v);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "args");
    if (!JS_IsUndefined(v)) { int32_t n; JS_ToInt32(ctx, &n, v); *nargs = n; }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "aggregate");
    if (JS_IsBool(v)) *aggregate = JS_VALUE_GET_BOOL(v);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "gas");
    if (!JS_IsUndefined(v)) { int64_t g; JS_ToInt64(ctx, &g, v); *gas = g; }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "heap");
    if (!JS_IsUndefined(v)) { uint32_t h; JS_ToUint32(ctx, &h, v); *heap = h; }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, opts, "stack");
    if (!JS_IsUndefined(v)) { uint32_t s; JS_ToUint32(ctx, &s, v); *stack_sz = s; }
    JS_FreeValue(ctx, v);
}

/*
 * db.udf.register(name, impl, opts?)
 *
 * impl types:
 *   function          -> JS scalar UDF
 *   {step, finalize}  -> JS aggregate UDF
 *   string            -> WASM UDF (module name)
 */
static JSValue js_db_udf_register(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    HlJS *js = get_hl_js(ctx);
    HlDbHandle *conn = js_call_handle(ctx, this_val);
    sqlite3 *raw_db = hl_db_sqlite_raw(conn);
    if (!js || !raw_db)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "db.udf.register requires (name, impl, opts?)");

    const char *sql_name = JS_ToCString(ctx, argv[0]);
    if (!sql_name)
        return JS_EXCEPTION;

    if (strncmp(sql_name, "hull_", 5) != 0) {
        JS_FreeCString(ctx, sql_name);
        return JS_ThrowTypeError(ctx, "UDF name must start with 'hull_'");
    }

    int deterministic = 0, nargs = -1, aggregate = 0;
    int64_t gas = 0;
    uint32_t heap = 0, stack_sz = 0;
    JSValueConst opts = argc >= 3 ? argv[2] : JS_UNDEFINED;

    if (JS_IsString(argv[1])) {
        /* ── WASM UDF ──────────────────────────────────────────── */
#ifdef HL_ENABLE_WASM
        const char *module_name = JS_ToCString(ctx, argv[1]);
        if (!module_name) {
            JS_FreeCString(ctx, sql_name);
            return JS_EXCEPTION;
        }

        if (!js->base.wasm_cache) {
            JS_FreeCString(ctx, module_name);
            JS_FreeCString(ctx, sql_name);
            return JS_ThrowInternalError(ctx, "WASM compute not available");
        }

        js_parse_udf_opts(ctx, opts, &deterministic, &nargs, &aggregate,
                          &gas, &heap, &stack_sz);

        HlDbUdfOpts udf_opts = {
            .sql_name      = sql_name,
            .module_name   = module_name,
            .nargs         = nargs != -1 ? nargs : 1,
            .gas_per_call  = gas,
            .deterministic = deterministic,
            .is_aggregate  = aggregate,
            .heap_size     = heap,
            .stack_size    = stack_sz,
        };

        const char *err_msg = NULL;
        int rc = hl_cap_db_udf_register_wasm(
            conn, js->base.wasm_cache, &udf_opts,
            js->base.app_vfs, js->app_dir,
            js->base.alloc, &err_msg);

        JS_FreeCString(ctx, module_name);
        JS_FreeCString(ctx, sql_name);

        if (rc != 0)
            return JS_ThrowInternalError(ctx, "db.udf.register: %s",
                                         err_msg ? err_msg : "registration failed");
#else
        JS_FreeCString(ctx, sql_name);
        return JS_ThrowInternalError(ctx, "WASM UDF support not compiled in");
#endif
    } else if (JS_IsFunction(ctx, argv[1])) {
        /* ── JS scalar UDF ─────────────────────────────────────── */
        js_parse_udf_opts(ctx, opts, &deterministic, &nargs, &aggregate,
                          &gas, &heap, &stack_sz);

        JsScalarUdfCtx *udf_ctx = calloc(1, sizeof(*udf_ctx));
        if (!udf_ctx) {
            JS_FreeCString(ctx, sql_name);
            return JS_ThrowInternalError(ctx, "db.udf.register: out of memory");
        }

        udf_ctx->ctx = ctx;
        udf_ctx->alive = &js->udf_runtime_alive;
        udf_ctx->func = JS_DupValue(ctx, argv[1]);

        int encoding = SQLITE_UTF8;
        if (deterministic) encoding |= SQLITE_DETERMINISTIC;

        int rc = sqlite3_create_function_v2(
            raw_db, sql_name, nargs, encoding, udf_ctx,
            js_scalar_udf_func, NULL, NULL,
            js_scalar_udf_destroy);

        JS_FreeCString(ctx, sql_name);

        if (rc != SQLITE_OK) {
            js_scalar_udf_destroy(udf_ctx);
            return JS_ThrowInternalError(ctx, "db.udf.register: %s",
                                         sqlite3_errmsg(raw_db));
        }
    } else if (JS_IsObject(argv[1])) {
        /* ── JS aggregate UDF ({step, finalize}) ───────────────── */
        JSValue step_fn = JS_GetPropertyStr(ctx, argv[1], "step");
        JSValue final_fn = JS_GetPropertyStr(ctx, argv[1], "finalize");

        int has_step = JS_IsFunction(ctx, step_fn);
        int has_final = JS_IsFunction(ctx, final_fn);

        if (!has_step || !has_final) {
            JS_FreeValue(ctx, step_fn);
            JS_FreeValue(ctx, final_fn);
            JS_FreeCString(ctx, sql_name);
            return JS_ThrowTypeError(ctx,
                "db.udf.register: aggregate requires step and finalize functions");
        }

        js_parse_udf_opts(ctx, opts, &deterministic, &nargs, &aggregate,
                          &gas, &heap, &stack_sz);

        JsAggUdfCtx *udf_ctx = calloc(1, sizeof(*udf_ctx));
        if (!udf_ctx) {
            JS_FreeValue(ctx, step_fn);
            JS_FreeValue(ctx, final_fn);
            JS_FreeCString(ctx, sql_name);
            return JS_ThrowInternalError(ctx, "db.udf.register: out of memory");
        }

        udf_ctx->ctx = ctx;
        udf_ctx->alive = &js->udf_runtime_alive;
        udf_ctx->step_fn = step_fn;       /* ownership transferred */
        udf_ctx->finalize_fn = final_fn;   /* ownership transferred */

        int encoding = SQLITE_UTF8;
        if (deterministic) encoding |= SQLITE_DETERMINISTIC;

        int rc = sqlite3_create_function_v2(
            raw_db, sql_name, nargs, encoding, udf_ctx,
            NULL, js_agg_step_func, js_agg_finalize_func,
            js_agg_udf_destroy);

        JS_FreeCString(ctx, sql_name);

        if (rc != SQLITE_OK) {
            js_agg_udf_destroy(udf_ctx);
            return JS_ThrowInternalError(ctx, "db.udf.register: %s",
                                         sqlite3_errmsg(raw_db));
        }
    } else {
        JS_FreeCString(ctx, sql_name);
        return JS_ThrowTypeError(ctx,
            "db.udf.register: impl must be a function, object, or string");
    }

    return JS_UNDEFINED;
}

/* db.udf.unregister(name) */
static JSValue js_db_udf_unregister(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    HlJS *js = get_hl_js(ctx);
    HlDbHandle *conn = js_call_handle(ctx, this_val);
    if (!js || !conn)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.udf.unregister requires (name)");

    const char *sql_name = JS_ToCString(ctx, argv[0]);
    if (!sql_name)
        return JS_EXCEPTION;

#ifdef HL_ENABLE_WASM
    int rc_unreg = hl_cap_db_udf_unregister(conn, sql_name);
    JS_FreeCString(ctx, sql_name);
    if (rc_unreg != 0)
        return JS_ThrowInternalError(ctx, "db.udf.unregister: failed");
    return JS_UNDEFINED;
#else
    sqlite3 *raw_db = hl_db_sqlite_raw(conn);
    if (!raw_db) {
        JS_FreeCString(ctx, sql_name);
        return JS_ThrowInternalError(ctx, "database not available");
    }
    int rc = sqlite3_create_function_v2(
        raw_db, sql_name, -1, SQLITE_UTF8,
        NULL, NULL, NULL, NULL, NULL);

    JS_FreeCString(ctx, sql_name);

    if (rc != SQLITE_OK)
        return JS_ThrowInternalError(ctx, "db.udf.unregister: %s",
                                     sqlite3_errmsg(raw_db));

    return JS_UNDEFINED;
#endif
}


/* Attach a `udf` sub-object to the connection object @p conn, each method
 * bound to @p h via a HullDbConnection sub-object opaque (js_call_handle
 * resolves it). Strong override of the weak no-op in mod_db.c: present only
 * when this bridge is composed, so a base with no SQLite backend has no
 * db.udf. Best-effort: on sub-object OOM the connection is left without udf. */
void hl_js_db_attach_udf(JSContext *ctx, JSValue conn, HlDbHandle *h)
{
    JSValue udf = new_bound_subobject(ctx, h);
    if (JS_IsException(udf)) { JS_FreeValue(ctx, udf); return; }
    JS_SetPropertyStr(ctx, udf, "register",
                      JS_NewCFunction(ctx, js_db_udf_register, "register", 3));
    JS_SetPropertyStr(ctx, udf, "unregister",
                      JS_NewCFunction(ctx, js_db_udf_unregister, "unregister", 1));
    JS_SetPropertyStr(ctx, conn, "udf", udf);
}

#endif /* HL_ENABLE_SQLITE */
#endif /* HL_ENABLE_DB */
