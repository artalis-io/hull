/*
 * mod_db.c — hull:db module (query, exec, batch, async)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/db.h"
#include "hull/worker_db.h"
#include "hull/async.h"
#include "hull/alloc.h"

#include <keel/server.h>
#include <sqlite3.h>

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

/* Check if the immediate JS caller is a stdlib module (module name starts
 * with "hull:").  User modules start with "./" — so a simple prefix check
 * is sufficient.  Returns 1 for stdlib, 0 for user code.
 *
 * n_stack_levels=1 skips the C function stack frame (level 0) to reach
 * the JS caller's frame. */
static int js_is_stdlib_caller(JSContext *ctx)
{
    JSAtom name = JS_GetScriptOrModuleName(ctx, 1);
    if (name == JS_ATOM_NULL)
        return 0;
    const char *str = JS_AtomToCString(ctx, name);
    JS_FreeAtom(ctx, name);
    if (!str)
        return 0;
    int is_stdlib = (strncmp(str, "hull:", 5) == 0);
    JS_FreeCString(ctx, str);
    return is_stdlib;
}

/* db.query implementation */
static JSValue js_db_query_impl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.stmt_cache)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.query requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (!js_is_stdlib_caller(ctx) && hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

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

    int rc = hl_cap_db_query(js->base.stmt_cache, sql, params, nparams,
                               js_query_row_cb, &qc, js->base.alloc);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc != 0) {
        JS_FreeValue(ctx, qc.array);
        return JS_ThrowInternalError(ctx, "query failed: %s",
                                     sqlite3_errmsg(js->base.db));
    }

    return qc.array;
}

/* db.exec implementation */
static JSValue js_db_exec_impl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.stmt_cache)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.exec requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (!js_is_stdlib_caller(ctx) && hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    HlValue *params = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (js_to_hl_values(ctx, argv[1], &params, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "params must be an array");
        }
    }

    int rc = hl_cap_db_exec(js->base.stmt_cache, sql, params, nparams);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc < 0)
        return JS_ThrowInternalError(ctx, "exec failed: %s",
                                     sqlite3_errmsg(js->base.db));

    return JS_NewInt32(ctx, rc);
}

static JSValue js_db_query(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{ return js_db_query_impl(ctx, this_val, argc, argv); }

static JSValue js_db_exec(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{ return js_db_exec_impl(ctx, this_val, argc, argv); }

/* db.lastId() */
static JSValue js_db_last_id(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db)
        return JS_ThrowInternalError(ctx, "database not available");

    return JS_NewInt64(ctx, hl_cap_db_last_id(js->base.db));
}

/* db.batch(fn) — execute fn() inside a transaction (BEGIN IMMEDIATE..COMMIT) */
static JSValue js_db_batch(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "db.batch requires a function argument");

    if (hl_cap_db_begin(js->base.db) != 0)
        return JS_ThrowInternalError(ctx, "BEGIN failed: %s",
                                     sqlite3_errmsg(js->base.db));

    JSValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);

    if (JS_IsException(result)) {
        hl_cap_db_rollback(js->base.db);
        return result; /* propagate exception */
    }
    JS_FreeValue(ctx, result);

    if (hl_cap_db_commit(js->base.db) != 0) {
        hl_cap_db_rollback(js->base.db);
        return JS_ThrowInternalError(ctx, "COMMIT failed: %s",
                                     sqlite3_errmsg(js->base.db));
    }

    return JS_UNDEFINED;
}

/* ── db.async.query / db.async.exec ─────────────────────────────────── */

/* push_result callback: convert HlWorkerDbOp result to JSValue */
static JSValue js_push_worker_db_result(JSContext *ctx, void *driver)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)driver;

    if (op->error) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "error",
                          JS_NewString(ctx, op->error_msg));
        return obj;
    }

    if (op->kind == HL_WORK_DB_EXEC) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "changes",
                          JS_NewInt32(ctx, op->exec_changes));
        JS_SetPropertyStr(ctx, obj, "lastId",
                          JS_NewInt64(ctx, op->last_id));
        return obj;
    }

    /* HL_WORK_DB_QUERY — array of row objects */
    HlDbResult *r = &op->result;
    JSValue arr = JS_NewArray(ctx);

    for (int row = 0; row < r->nrows; row++) {
        JSValue obj = JS_NewObject(ctx);
        HlDbValue *vals = &r->values[row * r->ncols];
        for (int col = 0; col < r->ncols; col++) {
            JSValue v;
            switch (vals[col].type) {
            case HL_TYPE_INT:
                v = JS_NewInt64(ctx, vals[col].i);
                break;
            case HL_TYPE_DOUBLE:
                v = JS_NewFloat64(ctx, vals[col].d);
                break;
            case HL_TYPE_TEXT:
            case HL_TYPE_BLOB:
                v = JS_NewStringLen(ctx, vals[col].s, vals[col].len);
                break;
            case HL_TYPE_NIL:
            default:
                v = JS_NULL;
                break;
            }
            JS_SetPropertyStr(ctx, obj, r->col_names[col], v);
        }
        JS_SetPropertyUint32(ctx, arr, (uint32_t)row, obj);
    }

    return arr;
}

/* Common implementation for db.async.query and db.async.exec */
static JSValue js_db_async_common(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv,
                                   HlWorkerDbKind kind)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx,
            "db.async not available (no thread pool)");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx,
            "db.async can only be called from a request handler");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.async requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (!js_is_stdlib_caller(ctx) && hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    /* Parse params */
    HlValue *params = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (js_to_hl_values(ctx, argv[1], &params, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "params must be an array");
        }
    }

    /* Allocate op */
    HlWorkerDbOp *op = calloc(1, sizeof(HlWorkerDbOp));
    if (!op) {
        js_free_hl_values(ctx, params, nparams);
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }

    op->kind = kind;
    op->server = js->server;
    op->alloc = js->base.alloc;
    op->sql = strdup(sql);
    JS_FreeCString(ctx, sql);
    if (!op->sql) {
        js_free_hl_values(ctx, params, nparams);
        free(op);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }

    /* Deep-copy params */
    if (nparams > 0) {
        op->params = hl_deep_copy_params(params, nparams);
        op->nparams = nparams;
        if (!op->params) {
            js_free_hl_values(ctx, params, nparams);
            free(op->sql);
            free(op);
            return JS_ThrowInternalError(ctx, "db.async: out of memory");
        }
    }
    js_free_hl_values(ctx, params, nparams);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.alloc);
    if (!actx) {
        hl_worker_db_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    /* Create JS continuation */
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_db_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_db_op_free_all;
    actx->op.on_cancel = hl_worker_db_async_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_worker_db_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "db.async: thread pool full");
    }

    /* Suspend the connection */
    if (kl_async_suspend(js->server, js->active_conn, &actx->op) < 0) {
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "db.async: failed to suspend connection");
    }

    return promise;
}

static JSValue js_db_async_query(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return js_db_async_common(ctx, this_val, argc, argv, HL_WORK_DB_QUERY);
}

static JSValue js_db_async_exec(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_db_async_common(ctx, this_val, argc, argv, HL_WORK_DB_EXEC);
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
    JS_SetPropertyStr(ctx, db, "batch",
                      JS_NewCFunction(ctx, js_db_batch, "batch", 1));

    /* db.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "query",
                      JS_NewCFunction(ctx, js_db_async_query, "query", 2));
    JS_SetPropertyStr(ctx, async_obj, "exec",
                      JS_NewCFunction(ctx, js_db_async_exec, "exec", 2));
    JS_SetPropertyStr(ctx, db, "async", async_obj);

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
