/*
 * mod_db.c — hull:db module (query, exec, batch, async, udf)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "mod_buffer.h"
#include "mod_db.h"               /* js_call_handle / new_bound_subobject seam */
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/db_dynamic.h"
#include "hull/worker_db.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"

#include <keel/server.h>

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

/* A caller-owned (db.open) connection. Its opaque is a malloc'd owner box that
 * this object owns; box->h is the connection, nulled by close(). The box itself
 * stays as the opaque so js_call_handle can tell "owned but closed" (box != NULL,
 * box->h == NULL, fail closed) apart from "not an owned object" (box == NULL,
 * fall through to the default). close() and the finalizer both release the
 * handle exactly once (NULL-guarded).
 *
 * The box is shared by the connection object AND its conn.async sub-object (both
 * are this class), so it is refcounted: each object holds one ref, the finalizer
 * decrements, and the last one out closes the handle + frees the box + dsn.
 * Refcounting (rather than the borrowed connection's no-op finalizer) is needed
 * because the box is malloc'd C state QuickJS does not GC-manage, and the
 * sub-object may outlive the parent. box->dsn is the validated DSN, for
 * conn.async to key the worker pool. Distinct class id from the borrowed
 * connection so this finalizer only ever runs on owned handles. */
typedef struct {
    HlDbHandle *h;        /* owned; NULL after close (idempotent) */
    char       *dsn;      /* owned; validated DSN, for conn.async */
    int         refcount; /* live objects sharing this box (conn + conn.async) */
} HlJsOwnedConn;
static JSClassID hull_db_owned_conn_class_id;
static void js_db_owned_conn_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJsOwnedConn *box =
        (HlJsOwnedConn *)JS_GetOpaque(val, hull_db_owned_conn_class_id);
    if (!box) return;
    if (--box->refcount > 0) return;   /* another sharer still alive */
    if (box->h) hl_db_dynamic_close(box->h);
    free(box->dsn);
    free(box);
}
static JSClassDef js_db_owned_conn_class = {
    "HullDbOwnedConnection",
    .finalizer = js_db_owned_conn_finalizer,
};

/* The default connection, resolved from the registry (there is no separate
 * default-handle field). NULL under --no-db. */
static HlDbHandle *default_db(HlJS *js)
{
    return js ? hl_db_registry_default(js->base.db_registry) : NULL;
}

/* Resolve the connection this call operates on: the bound handle when invoked
 * as a connection-object method (this is a HullDbConnection), else the default
 * connection. Internal-table access is gated by a caller check at each call
 * site, not by a separate handle. */
/* Non-static: shared with the composed SQLite UDF bridge (mod_db.h). */
HlDbHandle *js_call_handle(JSContext *ctx, JSValueConst this_val)
{
    HlDbHandle *bound = (HlDbHandle *)JS_GetOpaque(this_val, hull_db_conn_class_id);
    if (bound) return bound;
    /* A db.open() object carries an owner box; box->h is NULL once closed, so a
     * post-close method call fails closed rather than silently hitting the
     * default connection. A non-owned object has a NULL box and falls through. */
    HlJsOwnedConn *box =
        (HlJsOwnedConn *)JS_GetOpaque(this_val, hull_db_owned_conn_class_id);
    if (box) return box->h;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    return js ? default_db(js) : NULL;
}

/* Resolve the DSN this call's connection opened from, symmetric with
 * js_call_handle: an owner box (db.open) carries its own validated DSN; a
 * borrowed handle (db.connect/default) is matched in the registry. Used only by
 * conn.async to tell the worker pool which database to open its per-thread
 * connection against. NULL = unknown → the worker falls back to its default. */
static const char *js_call_dsn(JSContext *ctx, JSValueConst this_val)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js) return NULL;
    HlDbHandle *bound = (HlDbHandle *)JS_GetOpaque(this_val, hull_db_conn_class_id);
    if (bound) return hl_db_registry_dsn_for(js->base.db_registry, bound);
    HlJsOwnedConn *box =
        (HlJsOwnedConn *)JS_GetOpaque(this_val, hull_db_owned_conn_class_id);
    if (box) return box->dsn;
    return hl_db_registry_dsn_for(js->base.db_registry, default_db(js));
}

/* db.query implementation */
static JSValue js_db_query_impl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js_call_handle(ctx, this_val))
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
    if (!js || !js_call_handle(ctx, this_val))
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
    if (!js || !js_call_handle(ctx, this_val))
        return JS_ThrowInternalError(ctx, "database not available");

    return JS_NewInt64(ctx, hl_db_last_id(js_call_handle(ctx, this_val)));
}

/* db.batch(fn) — execute fn() inside a transaction (BEGIN IMMEDIATE..COMMIT) */
static JSValue js_db_batch(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js_call_handle(ctx, this_val))
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
    if (!js || !js_call_handle(ctx, this_val))
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
    if (!js || !js_call_handle(ctx, this_val))
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

/* db.quoteIdentifier(name) -> dialect-quoted identifier string. Wraps a table /
 * column name in the backend's identifier-quote char (doubling any internal
 * occurrence) so a reserved word or special char is safe when interpolated into
 * SQL. Mirrors the Lua conn.quote_identifier. */
static JSValue js_db_quote_identifier(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.quoteIdentifier requires (name)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    char buf[512];
    int n = hl_db_quote_ident(js_call_handle(ctx, this_val), name,
                              buf, sizeof buf);
    JS_FreeCString(ctx, name);
    if (n < 0)
        return JS_ThrowInternalError(ctx, "db.quoteIdentifier: name too long");
    return JS_NewStringLen(ctx, buf, (size_t)n);
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

    if (op->kind == HL_WORK_DB_WAIT_NOTIFY) {
        return JS_NewBool(ctx, op->notified);
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
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx,
            "db.async not available (no thread pool)");
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx,
            "db.async requires an active event loop");
    /* Require a live bound connection: a closed db.open() handle resolves to
     * NULL here, so async-after-close fails closed instead of silently
     * targeting the default database. */
    if (!js_call_handle(ctx, this_val))
        return JS_ThrowInternalError(ctx, "database not available");

    /* Input differs by kind: WAIT_NOTIFY takes (channel, timeoutMs); the
     * query/exec kinds take (sql, params). */
    HlWorkerDbOp *op = NULL;

    if (kind == HL_WORK_DB_WAIT_NOTIFY) {
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "waitNotify requires (channel, timeoutMs?)");
        const char *channel = JS_ToCString(ctx, argv[0]);
        if (!channel)
            return JS_EXCEPTION;
        int32_t timeout_ms = 1000;
        if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
            JS_ToInt32(ctx, &timeout_ms, argv[1]) < 0) {
            /* A throwing valueOf/Symbol.toPrimitive on the timeout arg left a
             * pending exception; propagate it rather than proceeding. */
            JS_FreeCString(ctx, channel);
            return JS_EXCEPTION;
        }
        op = calloc(1, sizeof(HlWorkerDbOp));
        if (!op) {
            JS_FreeCString(ctx, channel);
            return JS_ThrowInternalError(ctx, "db.async: out of memory");
        }
        op->kind = kind;
        op->server = js->server;
        op->alloc = js->base.alloc;
        op->channel = strdup(channel);
        op->timeout_ms = (int)timeout_ms;
        JS_FreeCString(ctx, channel);
        if (!op->channel) {
            free(op);
            return JS_ThrowInternalError(ctx, "db.async: out of memory");
        }
    } else {
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
        op = calloc(1, sizeof(HlWorkerDbOp));
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
    }

    /* Target the database this async call is bound to: db.connect(name).async
     * hits that named connection's DSN; db.open(dsn).async its dynamic DSN;
     * db.default().async resolves to the default. js_call_dsn recovers it from
     * whichever carrier this_val is; NULL (unknown) yields the worker default. */
    {
        const char *dsn = js_call_dsn(ctx, this_val);
        if (dsn) {
            op->dsn = strdup(dsn);
            if (!op->dsn) {
                hl_worker_db_op_free(op);
                free(op);
                return JS_ThrowInternalError(ctx, "db.async: out of memory");
            }
        }
    }

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

/* conn.waitNotify(channel, timeoutMs) -> Promise<boolean>. Yielding
 * low-latency wakeup: parks on the db.async worker pool while a worker blocks
 * on the backend's LISTEN/NOTIFY primitive; resolves true (notified) or false
 * (timeout / unsupported backend). Callers gate on conn.dialect.supportsNotify. */
static JSValue js_db_wait_notify(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_db_async_common(ctx, this_val, argc, argv, HL_WORK_DB_WAIT_NOTIFY);
}

/* ── db.udf — user-defined SQL functions (JS) ────────────────────────── */
/* The SQLite UDF bindings live in the composed per-runtime bridge
 * (mod_db_udf.c → libhull_feature-sqlite-js.a) so THIS base runtime archive
 * carries no sqlite3_* references (Phase C.2, docs/sqlite_feature.md). The
 * bridge provides a strong hl_js_db_attach_udf; this weak default (no udf
 * sub-object) stands in when no SQLite backend is composed. */
__attribute__((weak)) void hl_js_db_attach_udf(JSContext *ctx, JSValue conn,
                                               HlDbHandle *h)
{
    (void)ctx; (void)conn; (void)h;
}

/* A sub-object (conn.async / conn.udf) that is itself a HullDbConnection
 * carrying the SAME handle @p h, so a call on conn.async.* / conn.udf.*
 * recovers the bound handle via js_call_handle(this_val) exactly like the sync
 * methods. The class finalizer is a no-op (the handle is registry-owned), so
 * sharing the opaque across the object + its sub-objects is safe. */
/* Non-static: shared with the composed SQLite UDF bridge (mod_db.h). */
JSValue new_bound_subobject(JSContext *ctx, HlDbHandle *h)
{
    JSValue o = JS_NewObjectClass(ctx, (int)hull_db_conn_class_id);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, h);
    return o;
}

/* Build a connection object (HullDbConnection instance) carrying @p h as its
 * opaque. The sync methods are the same C functions the top-level bridge uses;
 * js_call_handle picks the bound handle over the runtime default. async targets
 * this connection's database via the worker pool's per-DSN connections; udf
 * registers on this connection (SQLite only). */

/* Set obj.dialect: a read-only snapshot of the backend's SQL dialect descriptor
 * (the single home for quoting / placeholder / upsert / RETURNING / identity
 * DDL that the query & schema builders read). */
static void js_set_dialect(JSContext *ctx, JSValue obj, const HlDbBackend *be)
{
    JSValue d = JS_NewObject(ctx);
    char qs[2] = { (be && be->dialect.identifier_quote)
                   ? be->dialect.identifier_quote : '"', '\0' };
    JS_SetPropertyStr(ctx, d, "identifierQuote", JS_NewString(ctx, qs));
    JS_SetPropertyStr(ctx, d, "placeholder",
                      JS_NewString(ctx, (be && be->dialect.placeholder)
                                        ? be->dialect.placeholder : "?"));
    JS_SetPropertyStr(ctx, d, "upsertStyle",
                      JS_NewString(ctx, (be && be->dialect.upsert_style)
                                        ? be->dialect.upsert_style : "on_conflict"));
    JS_SetPropertyStr(ctx, d, "supportsReturning",
                      JS_NewBool(ctx, be && be->dialect.supports_returning));
    JS_SetPropertyStr(ctx, d, "supportsIndexIfNotExists",
                      JS_NewBool(ctx, be && be->dialect.supports_index_if_not_exists));
    JS_SetPropertyStr(ctx, d, "supportsSkipLocked",
                      JS_NewBool(ctx, be && be->dialect.supports_skip_locked));
    JS_SetPropertyStr(ctx, d, "supportsNotify",
                      JS_NewBool(ctx, be && be->dialect.supports_notify));
    JS_SetPropertyStr(ctx, d, "identityColumn",
                      JS_NewString(ctx, (be && be->dialect.identity_column)
                                        ? be->dialect.identity_column
                                        : "INTEGER PRIMARY KEY"));
    if (be && be->dialect.identity_sequence)
        JS_SetPropertyStr(ctx, d, "identitySequence",
                          JS_NewString(ctx, be->dialect.identity_sequence));
    JS_SetPropertyStr(ctx, obj, "dialect", d);
}

static JSValue push_conn_object(JSContext *ctx, HlDbHandle *h)
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
    JS_SetPropertyStr(ctx, obj, "quoteIdentifier",
                      JS_NewCFunction(ctx, js_db_quote_identifier,
                                       "quoteIdentifier", 1));
    JS_SetPropertyStr(ctx, obj, "waitNotify",
                      JS_NewCFunction(ctx, js_db_wait_notify, "waitNotify", 2));
    /* async targets this connection's database via the worker pool's per-DSN
     * connections; udf registers on this connection's SQLite handle (a udf on
     * a non-SQLite connection errors at call time). Both sub-objects share the
     * bound handle, so a named connection is fully featured. */
    JSValue async_obj = new_bound_subobject(ctx, h);
    if (JS_IsException(async_obj)) { JS_FreeValue(ctx, obj); return async_obj; }
    JS_SetPropertyStr(ctx, async_obj, "query",
                      JS_NewCFunction(ctx, js_db_async_query, "query", 2));
    JS_SetPropertyStr(ctx, async_obj, "exec",
                      JS_NewCFunction(ctx, js_db_async_exec, "exec", 2));
    JS_SetPropertyStr(ctx, obj, "async", async_obj);

    const HlDbBackend *be = h ? h->backend : NULL;
    /* udf only when the backend supports it (§2.5). The sub-object itself is
     * attached by the composed SQLite bridge (strong hl_js_db_attach_udf); a
     * base with no SQLite backend gets the weak no-op and thus no udf. */
    if (be && be->supports_udf)
        hl_js_db_attach_udf(ctx, obj, h);
    JS_SetPropertyStr(ctx, obj, "backendName",
                      JS_NewString(ctx, be ? be->name : "none"));
    JS_SetPropertyStr(ctx, obj, "autoincrementIdDdl",
                      JS_NewString(ctx, (be && be->dialect.identity_column)
                                        ? be->dialect.identity_column
                                        : "INTEGER PRIMARY KEY"));
    js_set_dialect(ctx, obj, be);
    return obj;
}

/* conn.close() → release a db.open() handle now (idempotent; the finalizer is
 * the backstop). */
static JSValue js_db_owned_close(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    HlJsOwnedConn *box =
        (HlJsOwnedConn *)JS_GetOpaque(this_val, hull_db_owned_conn_class_id);
    if (box && box->h) {
        hl_db_dynamic_close(box->h);
        box->h = NULL;
    }
    return JS_UNDEFINED;
}

/* A conn.async sub-object for an owned connection: itself a HullDbOwnedConnection
 * sharing @p box (refcount++ on success), so js_call_handle / js_call_dsn resolve
 * the same live handle + DSN as the parent's sync methods. */
static JSValue new_owned_subobject(JSContext *ctx, HlJsOwnedConn *box)
{
    JSValue o = JS_NewObjectClass(ctx, (int)hull_db_owned_conn_class_id);
    if (JS_IsException(o)) return o;   /* refcount untouched on failure */
    box->refcount++;
    JS_SetOpaque(o, box);
    return o;
}

/* Build a caller-owned connection object wrapping @p h (from hl_db_dynamic_open),
 * opened from @p dsn. Unlike push_conn_object, this object owns its handle: it
 * adds a close() method and a finalizer that both release it via
 * hl_db_dynamic_close. It carries sync methods + async (a sub-object sharing the
 * refcounted owner box, so conn.async targets @p dsn through the worker pool).
 * udf is intentionally absent: worker-side udf re-registration is keyed off the
 * registry a dynamic handle is not in (tracked follow-up, §2.2). */
static JSValue push_owned_conn_object(JSContext *ctx, HlDbHandle *h,
                                      const char *dsn)
{
    HlJsOwnedConn *box = malloc(sizeof *box);
    if (!box) {
        hl_db_dynamic_close(h);
        return JS_ThrowOutOfMemory(ctx);
    }
    box->h = h;
    box->refcount = 1;   /* the connection object created just below */
    box->dsn = NULL;
    if (dsn) {
        box->dsn = strdup(dsn);
        if (!box->dsn) {
            free(box);
            hl_db_dynamic_close(h);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    JSValue obj = JS_NewObjectClass(ctx, (int)hull_db_owned_conn_class_id);
    if (JS_IsException(obj)) {
        free(box->dsn);
        free(box);
        hl_db_dynamic_close(h);
        return obj;
    }
    JS_SetOpaque(obj, box);   /* obj holds ref 1; its finalizer decrements */
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
    JS_SetPropertyStr(ctx, obj, "quoteIdentifier",
                      JS_NewCFunction(ctx, js_db_quote_identifier,
                                       "quoteIdentifier", 1));
    JS_SetPropertyStr(ctx, obj, "waitNotify",
                      JS_NewCFunction(ctx, js_db_wait_notify, "waitNotify", 2));
    JS_SetPropertyStr(ctx, obj, "close",
                      JS_NewCFunction(ctx, js_db_owned_close, "close", 0));

    /* async: shares the refcounted box; targets box->dsn via js_call_dsn. No
     * udf (see the function comment). On failure, freeing obj runs its
     * finalizer (refcount 1 -> 0), which closes the handle + frees the box. */
    JSValue async_obj = new_owned_subobject(ctx, box);
    if (JS_IsException(async_obj)) { JS_FreeValue(ctx, obj); return async_obj; }
    JS_SetPropertyStr(ctx, async_obj, "query",
                      JS_NewCFunction(ctx, js_db_async_query, "query", 2));
    JS_SetPropertyStr(ctx, async_obj, "exec",
                      JS_NewCFunction(ctx, js_db_async_exec, "exec", 2));
    JS_SetPropertyStr(ctx, obj, "async", async_obj);

    const HlDbBackend *be = h ? h->backend : NULL;
    JS_SetPropertyStr(ctx, obj, "backendName",
                      JS_NewString(ctx, be ? be->name : "none"));
    JS_SetPropertyStr(ctx, obj, "autoincrementIdDdl",
                      JS_NewString(ctx, (be && be->dialect.identity_column)
                                        ? be->dialect.identity_column
                                        : "INTEGER PRIMARY KEY"));
    js_set_dialect(ctx, obj, be);
    return obj;
}

/* db.open(dsn) → caller-owned connection object for a runtime-computed DSN,
 * validated against manifest.databases.dynamic (host/scheme allowlist, fs gate
 * for file backends). The app owns the result: call conn.close(), or let GC
 * finalize it. */
static JSValue js_db_open(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.open requires (dsn)");
    const char *dsn = JS_ToCString(ctx, argv[0]);
    if (!dsn)
        return JS_EXCEPTION;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db_registry) {
        JS_FreeCString(ctx, dsn);
        return JS_ThrowInternalError(ctx, "no database registry available");
    }
    const HlManifestDbDynamic *policy =
        hl_db_registry_dynamic_policy(js->base.db_registry);
    const char *err = NULL;
    HlDbHandle *h = hl_db_dynamic_open(dsn, policy, js->base.fs_cfg, &err);
    if (!h) {
        JS_FreeCString(ctx, dsn);
        return JS_ThrowInternalError(ctx, "%s", err ? err : "db.open: denied");
    }
    JSValue obj = push_owned_conn_object(ctx, h, dsn);   /* strdups dsn */
    JS_FreeCString(ctx, dsn);
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
    return push_conn_object(ctx, js ? default_db(js) : NULL);
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
    return push_conn_object(ctx, h);
}

/* ── Module init ─────────────────────────────────────────────────────── */

static int js_db_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/db", "hull:db") != 0) return -1;

    /* Register the connection-object classes once (idempotent per runtime). */
    if (hull_db_conn_class_id == 0) {
        JS_NewClassID(&hull_db_conn_class_id);
        JS_NewClass(JS_GetRuntime(ctx), hull_db_conn_class_id, &js_db_conn_class);
    }
    if (hull_db_owned_conn_class_id == 0) {
        JS_NewClassID(&hull_db_owned_conn_class_id);
        JS_NewClass(JS_GetRuntime(ctx), hull_db_owned_conn_class_id,
                    &js_db_owned_conn_class);
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
    JS_SetPropertyStr(ctx, db, "open",
                      JS_NewCFunction(ctx, js_db_open, "open", 1));

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
