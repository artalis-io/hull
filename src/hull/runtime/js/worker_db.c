/*
 * js_worker_db.c — Bridge: db.* for worker JS VMs
 *
 * Registers sync db.query/exec/batch/lastId into per-worker JS VMs using
 * the worker's TLS SQLite connection. This is the ONLY file that
 * combines JS + DB concerns.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "hull/worker_db.h"
#include "hull/cap/db.h"

#include "quickjs.h"

#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>

/* ── Bind params helper (simplified for worker VMs) ─────────────── */

static int worker_js_bind_params(sqlite3_stmt *stmt, JSContext *ctx,
                                 JSValueConst params)
{
    if (JS_IsUndefined(params) || JS_IsNull(params))
        return 0;

    JSValue len_val = JS_GetPropertyStr(ctx, params, "length");
    if (JS_IsException(len_val))
        return -1;

    int64_t len = 0;
    JS_ToInt64(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    for (int64_t i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, params, (uint32_t)i);
        int pidx = (int)(i + 1);
        int rc;

        if (JS_IsNull(elem) || JS_IsUndefined(elem)) {
            rc = sqlite3_bind_null(stmt, pidx);
        } else if (JS_IsBool(elem)) {
            rc = sqlite3_bind_int(stmt, pidx, JS_ToBool(ctx, elem) ? 1 : 0);
        } else if (JS_IsNumber(elem)) {
            double d;
            JS_ToFloat64(ctx, &d, elem);
            /* Check if integer */
            if (d == (double)(int64_t)d && d >= -9007199254740992.0 &&
                d <= 9007199254740992.0)
                rc = sqlite3_bind_int64(stmt, pidx, (sqlite3_int64)d);
            else
                rc = sqlite3_bind_double(stmt, pidx, d);
        } else if (JS_IsString(elem)) {
            size_t slen;
            const char *s = JS_ToCStringLen(ctx, &slen, elem);
            rc = sqlite3_bind_text(stmt, pidx, s, (int)slen, SQLITE_TRANSIENT);
            JS_FreeCString(ctx, s);
        } else {
            rc = sqlite3_bind_null(stmt, pidx);
        }

        JS_FreeValue(ctx, elem);
        if (rc != SQLITE_OK) return -1;
    }
    return 0;
}

/* ── db.query for worker VMs ────────────────────────────────────── */

static JSValue worker_js_db_query(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return JS_ThrowInternalError(ctx, "database not available in worker");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.query requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(wdb->db, sql, -1, &stmt, NULL);
    JS_FreeCString(ctx, sql);
    if (rc != SQLITE_OK)
        return JS_ThrowInternalError(ctx, "prepare: %s",
                                     sqlite3_errmsg(wdb->db));

    if (argc >= 2) {
        if (worker_js_bind_params(stmt, ctx, argv[1]) != 0) {
            sqlite3_finalize(stmt);
            return JS_ThrowInternalError(ctx, "bind: %s",
                                         sqlite3_errmsg(wdb->db));
        }
    }

    JSValue rows = JS_NewArray(ctx);
    int row_num = 0;
    int ncols = 0;
    int first = 1;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (first) {
            ncols = sqlite3_column_count(stmt);
            first = 0;
        }
        JSValue row = JS_NewObject(ctx);
        for (int i = 0; i < ncols; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            JSValue val;
            switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_INTEGER:
                val = JS_NewInt64(ctx, sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                val = JS_NewFloat64(ctx, sqlite3_column_double(stmt, i));
                break;
            case SQLITE_TEXT:
                val = JS_NewStringLen(ctx,
                    (const char *)sqlite3_column_text(stmt, i),
                    (size_t)sqlite3_column_bytes(stmt, i));
                break;
            case SQLITE_NULL:
            default:
                val = JS_NULL;
                break;
            }
            JS_SetPropertyStr(ctx, row, name ? name : "?", val);
        }
        JS_SetPropertyUint32(ctx, rows, (uint32_t)row_num, row);
        row_num++;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        JS_FreeValue(ctx, rows);
        return JS_ThrowInternalError(ctx, "query: %s",
                                     sqlite3_errmsg(wdb->db));
    }

    return rows;
}

/* ── db.exec for worker VMs ──────────────────────────────────────── */

static JSValue worker_js_db_exec(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return JS_ThrowInternalError(ctx, "database not available in worker");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.exec requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(wdb->db, sql, -1, &stmt, NULL);
    JS_FreeCString(ctx, sql);
    if (rc != SQLITE_OK)
        return JS_ThrowInternalError(ctx, "prepare: %s",
                                     sqlite3_errmsg(wdb->db));

    if (argc >= 2) {
        if (worker_js_bind_params(stmt, ctx, argv[1]) != 0) {
            sqlite3_finalize(stmt);
            return JS_ThrowInternalError(ctx, "bind: %s",
                                         sqlite3_errmsg(wdb->db));
        }
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
        return JS_ThrowInternalError(ctx, "exec: %s",
                                     sqlite3_errmsg(wdb->db));

    return JS_NewInt32(ctx, sqlite3_changes(wdb->db));
}

/* ── db.batch for worker VMs ─────────────────────────────────────── */

static JSValue worker_js_db_batch(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return JS_ThrowInternalError(ctx, "database not available in worker");

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "db.batch requires a function");

    if (hl_cap_db_begin(wdb->db) != 0)
        return JS_ThrowInternalError(ctx, "BEGIN failed: %s",
                                     sqlite3_errmsg(wdb->db));

    JSValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);

    if (JS_IsException(result)) {
        hl_cap_db_rollback(wdb->db);
        return JS_EXCEPTION;
    }

    if (hl_cap_db_commit(wdb->db) != 0) {
        hl_cap_db_rollback(wdb->db);
        JS_FreeValue(ctx, result);
        return JS_ThrowInternalError(ctx, "COMMIT failed: %s",
                                     sqlite3_errmsg(wdb->db));
    }

    return result;
}

/* ── db.lastId for worker VMs ───────────────────────────────────── */

static JSValue worker_js_db_last_id(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db)
        return JS_ThrowInternalError(ctx, "database not available in worker");
    return JS_NewInt64(ctx, (int64_t)hl_cap_db_last_id(wdb->db));
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
