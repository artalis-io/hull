/*
 * js_worker_db.c - Bridge: db.* for worker JS VMs
 *
 * Registers sync db.query/exec/batch/lastId into per-worker JS VMs using
 * the worker thread's own backend connection (HlDbBackend vtable), so the
 * path is transparent across SQLite and PostgreSQL. This is the ONLY file
 * that combines JS + DB concerns for worker VMs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/runtime/js.h"
#include "internal.h"
#include "hull/worker_db.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"

#include "quickjs.h"

#include <string.h>
#include <stdlib.h>

/* ── Param marshalling (JS array → HlValue[]) ───────────────────── */

/* Marshal a JS array at @p arr into a malloc'd HlValue[]. String values
 * borrow QuickJS C-strings which must outlive the query, so their owning
 * pointers are returned in @p *out_strs for the caller to free afterward
 * via worker_js_free_values. Returns 0 / -1. */
static int worker_js_to_hl_values(JSContext *ctx, JSValueConst arr,
                                  HlValue **out_params, const char ***out_strs,
                                  int *out_count)
{
    *out_params = NULL;
    *out_strs = NULL;
    *out_count = 0;

    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(len_val))
        return -1;
    int64_t len = 0;
    JS_ToInt64(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);
    if (len <= 0)
        return 0;
    if ((size_t)len > SIZE_MAX / sizeof(HlValue))
        return -1;

    HlValue *params = calloc((size_t)len, sizeof(HlValue));
    const char **strs = calloc((size_t)len, sizeof(char *));
    if (!params || !strs) {
        free(params);
        free(strs);
        return -1;
    }

    for (int64_t i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);

        if (JS_IsNull(elem) || JS_IsUndefined(elem)) {
            params[i].type = HL_TYPE_NIL;
        } else if (JS_IsBool(elem)) {
            params[i].type = HL_TYPE_BOOL;
            params[i].b = JS_ToBool(ctx, elem) ? 1 : 0;
        } else if (JS_IsNumber(elem)) {
            double d = 0;
            JS_ToFloat64(ctx, &d, elem);
            if (d == (double)(int64_t)d && d >= -9007199254740992.0 &&
                d <= 9007199254740992.0) {
                params[i].type = HL_TYPE_INT;
                params[i].i = (int64_t)d;
            } else {
                params[i].type = HL_TYPE_DOUBLE;
                params[i].d = d;
            }
        } else if (JS_IsString(elem)) {
            size_t slen = 0;
            const char *s = JS_ToCStringLen(ctx, &slen, elem);
            if (!s) {
                JS_FreeValue(ctx, elem);
                for (int64_t j = 0; j < i; j++)
                    if (strs[j]) JS_FreeCString(ctx, strs[j]);
                free(strs);
                free(params);
                return -1;
            }
            strs[i] = s;   /* freed by worker_js_free_values */
            params[i].type = HL_TYPE_TEXT;
            params[i].s = s;
            params[i].len = slen;
        } else {
            params[i].type = HL_TYPE_NIL;
        }

        JS_FreeValue(ctx, elem);
    }

    *out_params = params;
    *out_strs = strs;
    *out_count = (int)len;
    return 0;
}

static void worker_js_free_values(JSContext *ctx, HlValue *params,
                                  const char **strs, int count)
{
    if (strs) {
        for (int i = 0; i < count; i++)
            if (strs[i]) JS_FreeCString(ctx, strs[i]);
        free(strs);
    }
    free(params);
}

/* ── Row callback (backend-agnostic) ────────────────────────────── */

typedef struct {
    JSContext *ctx;
    JSValue    rows;
    int        row_num;
} WorkerJsQueryCtx;

static int worker_js_row_cb(void *opaque, HlColumn *cols, int ncols)
{
    WorkerJsQueryCtx *qc = (WorkerJsQueryCtx *)opaque;
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
        case HL_TYPE_BLOB:
            val = JS_NewStringLen(qc->ctx, cols[i].value.s, cols[i].value.len);
            break;
        case HL_TYPE_BOOL:
            val = JS_NewBool(qc->ctx, cols[i].value.b);
            break;
        case HL_TYPE_NIL:
        default:
            val = JS_NULL;
            break;
        }
        JS_SetPropertyStr(qc->ctx, row, cols[i].name ? cols[i].name : "?", val);
    }
    JS_SetPropertyUint32(qc->ctx, qc->rows, (uint32_t)qc->row_num, row);
    qc->row_num++;
    return 0;
}

/* ── db.query for worker VMs ────────────────────────────────────── */

static JSValue worker_js_db_query(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.query requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    const char *err = NULL;
    HlDbHandle *h = hl_worker_db_handle_checked(sql, &err);
    if (!h) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx, "%s", err ? err : "worker db");
    }

    HlValue *params = NULL;
    const char **strs = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (worker_js_to_hl_values(ctx, argv[1], &params, &strs, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowInternalError(ctx, "params must be an array");
        }
    }

    JSValue rows = JS_NewArray(ctx);
    WorkerJsQueryCtx qc = { .ctx = ctx, .rows = rows, .row_num = 0 };
    int rc = hl_db_query(h, sql, params, nparams, worker_js_row_cb, &qc, NULL);

    worker_js_free_values(ctx, params, strs, nparams);
    JS_FreeCString(ctx, sql);

    if (rc != 0) {
        JS_FreeValue(ctx, rows);
        return JS_ThrowInternalError(ctx, "query: %s", hl_db_errmsg(h));
    }
    return rows;
}

/* ── db.exec for worker VMs ──────────────────────────────────────── */

static JSValue worker_js_db_exec(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.exec requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    const char *err = NULL;
    HlDbHandle *h = hl_worker_db_handle_checked(sql, &err);
    if (!h) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx, "%s", err ? err : "worker db");
    }

    HlValue *params = NULL;
    const char **strs = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (worker_js_to_hl_values(ctx, argv[1], &params, &strs, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowInternalError(ctx, "params must be an array");
        }
    }

    int rc = hl_db_exec(h, sql, params, nparams);

    worker_js_free_values(ctx, params, strs, nparams);
    JS_FreeCString(ctx, sql);

    if (rc < 0)
        return JS_ThrowInternalError(ctx, "exec: %s", hl_db_errmsg(h));

    return JS_NewInt32(ctx, rc);
}

/* ── db.batch for worker VMs ─────────────────────────────────────── */

static JSValue worker_js_db_batch(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->handle.backend)
        return JS_ThrowInternalError(ctx, "database not available in worker");
    HlDbHandle *h = &wdb->handle;

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "db.batch requires a function");

    if (hl_db_begin(h) != 0)
        return JS_ThrowInternalError(ctx, "BEGIN failed: %s", hl_db_errmsg(h));

    JSValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);

    if (JS_IsException(result)) {
        hl_db_rollback(h);
        return JS_EXCEPTION;
    }

    if (hl_db_commit(h) != 0) {
        hl_db_rollback(h);
        JS_FreeValue(ctx, result);
        return JS_ThrowInternalError(ctx, "COMMIT failed: %s", hl_db_errmsg(h));
    }

    return result;
}

/* ── db.lastId for worker VMs ───────────────────────────────────── */

static JSValue worker_js_db_last_id(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->handle.backend)
        return JS_ThrowInternalError(ctx, "database not available in worker");
    return JS_NewInt64(ctx, (int64_t)hl_db_last_id(&wdb->handle));
}

/* ── Init hook ──────────────────────────────────────────────────── */

static int js_worker_db_init_hook(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue db = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, db, "query",
                      JS_NewCFunction(ctx, worker_js_db_query, "query", 2));
    JS_SetPropertyStr(ctx, db, "exec",
                      JS_NewCFunction(ctx, worker_js_db_exec, "exec", 2));
    JS_SetPropertyStr(ctx, db, "batch",
                      JS_NewCFunction(ctx, worker_js_db_batch, "batch", 1));
    JS_SetPropertyStr(ctx, db, "lastId",
                      JS_NewCFunction(ctx, worker_js_db_last_id, "lastId", 0));

    JS_SetPropertyStr(ctx, global, "db", db);
    JS_FreeValue(ctx, global);
    return 0;
}

void hl_js_worker_db_init(void)
{
    hl_js_worker_register_init(js_worker_db_init_hook);
}

#endif /* HL_ENABLE_DB */
