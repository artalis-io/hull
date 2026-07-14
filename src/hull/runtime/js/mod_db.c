/*
 * mod_db.c — hull:db module (query, exec, batch, async, udf)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "mod_buffer.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"
#include "hull/worker_db.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"

#ifdef HL_ENABLE_WASM
#include "hull/cap/db_udf.h"
#include "hull/cap/wasm.h"
#endif

#include <keel/server.h>
#include <sqlite3.h> /* needed for UDF: sqlite3_value, sqlite3_context, etc. */

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

/* Marshal a JS array of strings into a `const char **` for the vtable
 * helpers (insert_if_absent / upsert).  Returns 0 on success.  Caller must
 * free each string via JS_FreeCString and the array via js_free. */
static int js_to_string_array(JSContext *ctx, JSValueConst arr,
                               const char ***out_strs, int *out_count)
{
    *out_strs = NULL;
    *out_count = 0;

    if (!JS_IsArray(ctx, arr))
        return -1;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    if (len <= 0)
        return -1;

    if ((size_t)len > SIZE_MAX / sizeof(char *))
        return -1;

    const char **strs = js_mallocz(ctx, (size_t)len * sizeof(char *));
    if (!strs)
        return -1;

    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char *s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s) {
            for (int32_t j = 0; j < i; j++)
                JS_FreeCString(ctx, strs[j]);
            js_free(ctx, strs);
            return -1;
        }
        strs[i] = s;
    }

    *out_strs = strs;
    *out_count = len;
    return 0;
}

static void js_free_string_array(JSContext *ctx, const char **strs, int count)
{
    if (!strs)
        return;
    for (int i = 0; i < count; i++) {
        if (strs[i])
            JS_FreeCString(ctx, strs[i]);
    }
    js_free(ctx, strs);
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

/* ── Connection objects: db.connect(name) / db.default() ──────────── */

/* The connection handle is registry-owned, so the object's finalizer frees
 * nothing; the opaque is just a borrowed HlDbHandle*. Non-forgeable: only C
 * sets the opaque, so app JS cannot fabricate a connection over an arbitrary
 * pointer (a plain object's JS_GetOpaque against this class id is NULL). */
static JSClassID hull_db_conn_class_id;
static void js_db_conn_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt; (void)val;   /* handle owned by the registry; nothing to free */
}
static JSClassDef js_db_conn_class = {
    "HullDbConnection",
    .finalizer = js_db_conn_finalizer,
};

/* Resolve the connection this call operates on: the bound handle when invoked
 * as a connection-object method (this is a HullDbConnection), else the runtime
 * default handle (the top-level db.* bridge). Internal-table access is gated
 * by a caller check at each call site, not by a separate handle. */
static HlDbHandle *js_call_handle(JSContext *ctx, JSValueConst this_val)
{
    HlDbHandle *bound = (HlDbHandle *)JS_GetOpaque(this_val, hull_db_conn_class_id);
    if (bound) return bound;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    return js ? js->base.db_handle : NULL;
}

/* db.query implementation */
static JSValue js_db_query_impl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_handle)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.query requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    int is_stdlib = js_is_stdlib_caller(ctx);
    HlDbHandle *h = js_call_handle(ctx, this_val);

    if (!is_stdlib && hl_cap_db_check_namespace(sql) != 0) {
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

    int rc = hl_db_query(h, sql, params, nparams,
                         js_query_row_cb, &qc, js->base.alloc);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc != 0) {
        JS_FreeValue(ctx, qc.array);
        return JS_ThrowInternalError(ctx, "query failed: %s",
                                     hl_db_errmsg(h));
    }

    return qc.array;
}

/* db.exec implementation */
static JSValue js_db_exec_impl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_handle)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.exec requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    int is_stdlib = js_is_stdlib_caller(ctx);
    HlDbHandle *h = js_call_handle(ctx, this_val);

    if (!is_stdlib && hl_cap_db_check_namespace(sql) != 0) {
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

    int rc = hl_db_exec(h, sql, params, nparams);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc < 0)
        return JS_ThrowInternalError(ctx, "exec failed: %s",
                                     hl_db_errmsg(h));

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
    (void)argc; (void)argv;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_handle)
        return JS_ThrowInternalError(ctx, "database not available");

    return JS_NewInt64(ctx, hl_db_last_id(js_call_handle(ctx, this_val)));
}

/* db.batch(fn) — execute fn() inside a transaction (BEGIN IMMEDIATE..COMMIT) */
static JSValue js_db_batch(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_handle)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "db.batch requires a function argument");

    HlDbHandle *h = js_call_handle(ctx, this_val);

    if (hl_db_begin(h) != 0)
        return JS_ThrowInternalError(ctx, "BEGIN failed: %s",
                                     hl_db_errmsg(h));

    JSValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);

    if (JS_IsException(result)) {
        hl_db_rollback(h);
        return result; /* propagate exception */
    }
    JS_FreeValue(ctx, result);

    if (hl_db_commit(h) != 0) {
        hl_db_rollback(h);
        return JS_ThrowInternalError(ctx, "COMMIT failed: %s",
                                     hl_db_errmsg(h));
    }

    return JS_UNDEFINED;
}

/* ── Dialect-aware helpers (insert_if_absent / upsert / table_columns) ── */

/* Shared body for db.insertIfAbsent / db.upsert — they differ only in
 * which vtable method they dispatch through. */
static JSValue js_db_dialect_write(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int is_upsert,
                                    const char *name)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_handle)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 4)
        return JS_ThrowTypeError(ctx,
            "%s requires (table, conflictCols, cols, values)", name);

    const char *table = JS_ToCString(ctx, argv[0]);
    if (!table)
        return JS_EXCEPTION;

    int is_stdlib = js_is_stdlib_caller(ctx);
    HlDbHandle *h = js_call_handle(ctx, this_val);

    if (!is_stdlib && strncmp(table, "_hull_", 6) == 0) {
        JS_FreeCString(ctx, table);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    const char **conflict_cols = NULL;
    int n_conflict = 0;
    const char **cols = NULL;
    int n_cols = 0;
    HlValue *values = NULL;
    int n_values = 0;

    if (js_to_string_array(ctx, argv[1], &conflict_cols, &n_conflict) != 0) {
        JS_FreeCString(ctx, table);
        return JS_ThrowTypeError(ctx, "conflictCols must be a non-empty array");
    }
    if (js_to_string_array(ctx, argv[2], &cols, &n_cols) != 0) {
        js_free_string_array(ctx, conflict_cols, n_conflict);
        JS_FreeCString(ctx, table);
        return JS_ThrowTypeError(ctx, "cols must be a non-empty array");
    }
    if (js_to_hl_values(ctx, argv[3], &values, &n_values) != 0
        || n_values != n_cols) {
        js_free_hl_values(ctx, values, n_values);
        js_free_string_array(ctx, cols, n_cols);
        js_free_string_array(ctx, conflict_cols, n_conflict);
        JS_FreeCString(ctx, table);
        return JS_ThrowTypeError(ctx, "values length must match cols length");
    }

    int rc = is_upsert
        ? hl_db_upsert(h, table, conflict_cols, n_conflict,
                       cols, values, n_cols)
        : hl_db_insert_if_absent(h, table, conflict_cols, n_conflict,
                                  cols, values, n_cols);

    js_free_hl_values(ctx, values, n_values);
    js_free_string_array(ctx, cols, n_cols);
    js_free_string_array(ctx, conflict_cols, n_conflict);
    JS_FreeCString(ctx, table);

    if (rc < 0)
        return JS_ThrowInternalError(ctx, "%s failed: %s",
                                     name, hl_db_errmsg(h));

    return JS_NewInt32(ctx, rc);
}

static JSValue js_db_insert_if_absent(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    return js_db_dialect_write(ctx, this_val, argc, argv, 0, "db.insertIfAbsent");
}

static JSValue js_db_upsert(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    return js_db_dialect_write(ctx, this_val, argc, argv, 1, "db.upsert");
}

/* db.tableColumns(table) -> string[] */
typedef struct {
    JSContext *ctx;
    JSValue    array;
    uint32_t   n;
} JsColumnsCtx;

static void js_table_columns_cb(void *cb_ctx, const char *col_name)
{
    JsColumnsCtx *cc = (JsColumnsCtx *)cb_ctx;
    JS_SetPropertyUint32(cc->ctx, cc->array, cc->n,
                          JS_NewString(cc->ctx, col_name));
    cc->n++;
}

static JSValue js_db_table_columns(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_handle)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.tableColumns requires (table)");

    const char *table = JS_ToCString(ctx, argv[0]);
    if (!table)
        return JS_EXCEPTION;

    int is_stdlib = js_is_stdlib_caller(ctx);
    HlDbHandle *h = js_call_handle(ctx, this_val);

    if (!is_stdlib && strncmp(table, "_hull_", 6) == 0) {
        JS_FreeCString(ctx, table);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    JsColumnsCtx cc = { .ctx = ctx, .array = JS_NewArray(ctx), .n = 0 };
    int rc = hl_db_table_columns(h, table, js_table_columns_cb, &cc);
    JS_FreeCString(ctx, table);

    if (rc != 0) {
        JS_FreeValue(ctx, cc.array);
        return JS_ThrowInternalError(ctx, "db.tableColumns failed: %s",
                                     hl_db_errmsg(h));
    }
    return cc.array;
}

/* ── db.async.query / db.async.exec ─────────────────────────────────── */

/* push_result callback: convert HlWorkerDbOp result to JSValue.
 * On error, returns JS_EXCEPTION (after throwing) — the async resume
 * machinery rejects the awaiting Promise. Successful exec returns
 * { changes, lastId }; query returns the rows array directly. */
static JSValue js_push_worker_db_result(JSContext *ctx, void *driver)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)driver;

    if (op->error) {
        return JS_ThrowInternalError(ctx, "db.async: %s", op->error_msg);
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
            case HL_TYPE_BOOL:
                /* Postgres bool columns arrive as HL_TYPE_BOOL (flag in .i);
                 * SQLite never emits this type. */
                v = JS_NewBool(ctx, vals[col].i != 0);
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
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx,
            "db.async requires an active event loop");

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
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx, js->base.alloc);
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
    actx->detached = (js->active_conn == NULL);

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

    /* Suspend the FD (attached only). */
    if (!actx->detached &&
        hl_net_op_suspend(js->base.net_ctx, (HlReqHandle *)js->active_conn, (HlSuspendOp *)&actx->op) < 0) {
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

/* ── db.udf — user-defined SQL functions (JS) ────────────────────────── */

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
    (void)this_val;
    HlJS *js = get_hl_js(ctx);
    sqlite3 *raw_db = js ? hl_db_sqlite_raw(js->base.db_handle) : NULL;
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
            js->base.db_handle, js->base.wasm_cache, &udf_opts,
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
    (void)this_val;
    HlJS *js = get_hl_js(ctx);
    if (!js || !js->base.db_handle)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.udf.unregister requires (name)");

    const char *sql_name = JS_ToCString(ctx, argv[0]);
    if (!sql_name)
        return JS_EXCEPTION;

#ifdef HL_ENABLE_WASM
    int rc_unreg = hl_cap_db_udf_unregister(js->base.db_handle, sql_name);
    JS_FreeCString(ctx, sql_name);
    if (rc_unreg != 0)
        return JS_ThrowInternalError(ctx, "db.udf.unregister: failed");
    return JS_UNDEFINED;
#else
    sqlite3 *raw_db = hl_db_sqlite_raw(js->base.db_handle);
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

/* Build a connection object (HullDbConnection instance) carrying @p h as its
 * opaque. The sync methods are the same C functions the top-level bridge uses;
 * js_call_handle picks the bound handle over the runtime default. async / udf
 * stay on the top-level module for now (the worker layer is single-DSN). */
static JSValue push_conn_object(JSContext *ctx, HlDbHandle *h, int is_default)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)hull_db_conn_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, h);
    JS_SetPropertyStr(ctx, obj, "query",
                      JS_NewCFunction(ctx, js_db_query, "query", 2));
    JS_SetPropertyStr(ctx, obj, "exec",
                      JS_NewCFunction(ctx, js_db_exec, "exec", 2));
    JS_SetPropertyStr(ctx, obj, "lastId",
                      JS_NewCFunction(ctx, js_db_last_id, "lastId", 0));
    JS_SetPropertyStr(ctx, obj, "batch",
                      JS_NewCFunction(ctx, js_db_batch, "batch", 1));
    JS_SetPropertyStr(ctx, obj, "insertIfAbsent",
                      JS_NewCFunction(ctx, js_db_insert_if_absent,
                                       "insertIfAbsent", 4));
    JS_SetPropertyStr(ctx, obj, "upsert",
                      JS_NewCFunction(ctx, js_db_upsert, "upsert", 4));
    JS_SetPropertyStr(ctx, obj, "tableColumns",
                      JS_NewCFunction(ctx, js_db_table_columns,
                                       "tableColumns", 1));
    /* async / udf dispatch to the per-thread worker + the SQLite raw handle,
     * both of which are the DEFAULT connection. Only the default connection
     * object carries them; named connections get the sync surface only until
     * per-connection workers land. */
    if (is_default) {
        JSValue async_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, async_obj, "query",
                          JS_NewCFunction(ctx, js_db_async_query, "query", 2));
        JS_SetPropertyStr(ctx, async_obj, "exec",
                          JS_NewCFunction(ctx, js_db_async_exec, "exec", 2));
        JS_SetPropertyStr(ctx, obj, "async", async_obj);

        JSValue udf_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, udf_obj, "register",
                          JS_NewCFunction(ctx, js_db_udf_register, "register", 3));
        JS_SetPropertyStr(ctx, udf_obj, "unregister",
                          JS_NewCFunction(ctx, js_db_udf_unregister, "unregister", 1));
        JS_SetPropertyStr(ctx, obj, "udf", udf_obj);
    }
    const HlDbBackend *be = h ? h->backend : NULL;
    JS_SetPropertyStr(ctx, obj, "backendName",
                      JS_NewString(ctx, be ? be->name : "none"));
    JS_SetPropertyStr(ctx, obj, "autoincrementIdDdl",
                      JS_NewString(ctx, (be && be->autoincrement_id_ddl)
                                        ? be->autoincrement_id_ddl
                                        : "INTEGER PRIMARY KEY"));
    return obj;
}

/* db.default() → connection object for the "default" connection. Returns an
 * object even when no DB is configured; its methods error lazily on use, so
 * importing a db-using module in a no-db context does not fail at load. */
static JSValue js_db_default(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    return push_conn_object(ctx, js ? js->base.db_handle : NULL, 1);
}

/* db.connect(name) → connection object for a manifest-declared database. */
static JSValue js_db_connect(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.connect requires (name)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_registry) {
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "no database registry available");
    }
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(js->base.db_registry, name, &err);
    if (!h) {
        JSValue e = JS_ThrowInternalError(ctx, "db.connect('%s'): %s",
                                          name, err ? err : "unknown database");
        JS_FreeCString(ctx, name);
        return e;
    }
    JS_FreeCString(ctx, name);
    return push_conn_object(ctx, h, 0);
}

/* ── Module init ─────────────────────────────────────────────────────── */

static int js_db_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/db", "hull:db") != 0) return -1;

    /* Register the connection-object class once (idempotent per runtime). */
    if (hull_db_conn_class_id == 0) {
        JS_NewClassID(&hull_db_conn_class_id);
        JS_NewClass(JS_GetRuntime(ctx), hull_db_conn_class_id, &js_db_conn_class);
    }

    /* The DB module exposes only connection acquisition: every query goes
     * through a connection object from db.connect(name) or db.default(). The
     * historical top-level db.query / exec / async / udf / backendName / ...
     * bridge is gone; those live on the connection object (db.default()). */
    JSValue db = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, db, "connect",
                      JS_NewCFunction(ctx, js_db_connect, "connect", 1));
    JS_SetPropertyStr(ctx, db, "default",
                      JS_NewCFunction(ctx, js_db_default, "default", 0));

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

#endif /* HL_ENABLE_DB */
