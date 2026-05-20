/*
 * worker_db.c — Runtime-agnostic DB worker capability
 *
 * Per-worker SQLite connections via pthread TLS, and KlWorkItem callbacks
 * for db.async.query/exec operations. Zero Lua/JS knowledge.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/worker_db.h"
#include "hull/cap/db.h"
#include "hull/async.h"
#include "hull/async_backend.h"
#include "hull/alloc.h"

#include <keel/thread_pool.h>
#include <keel/async.h>

#include <sqlite3.h>
#include <pthread.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "log.h"

/* ── TLS for per-worker DB connections ─────────────────────────────── */

static pthread_key_t  worker_db_key;
static pthread_once_t worker_db_once = PTHREAD_ONCE_INIT;
static const char    *worker_db_path;   /* set once, read-only after */

static void worker_db_destructor(void *ptr)
{
    if (!ptr) return;
    HlWorkerDb *wdb = (HlWorkerDb *)ptr;
    hl_stmt_cache_destroy(&wdb->cache);
    if (wdb->db) {
        hl_cap_db_shutdown(wdb->db);
        sqlite3_close(wdb->db);
    }
    free(wdb);
}

static void worker_db_key_create(void)
{
    pthread_key_create(&worker_db_key, worker_db_destructor);
}

void hl_worker_db_init(const char *db_path)
{
    /* Resolve to absolute path so worker threads open the same path
     * the sandbox registered (realpath resolves /var → /private/var on macOS) */
    static char resolved[PATH_MAX];
    if (realpath(db_path, resolved))
        worker_db_path = resolved;
    else
        worker_db_path = db_path;
    pthread_once(&worker_db_once, worker_db_key_create);
}

HlWorkerDb *hl_worker_db_get(void)
{
    HlWorkerDb *wdb = (HlWorkerDb *)pthread_getspecific(worker_db_key);
    if (wdb) return wdb;

    if (!worker_db_path) return NULL;

    wdb = calloc(1, sizeof(HlWorkerDb));
    if (!wdb) return NULL;

    int rc = sqlite3_open(worker_db_path, &wdb->db);
    if (rc != SQLITE_OK) {
        log_error("[hull:worker_db] sqlite3_open failed: rc=%d (%s) path=%s errno=%d",
                  rc, sqlite3_errmsg(wdb->db), worker_db_path, errno);
        sqlite3_close(wdb->db);
        free(wdb);
        return NULL;
    }

    if (hl_cap_db_init(wdb->db) != 0) {
        log_error("[hull:worker_db] hl_cap_db_init failed");
        sqlite3_close(wdb->db);
        free(wdb);
        return NULL;
    }

    hl_stmt_cache_init(&wdb->cache, wdb->db, NULL);
    pthread_setspecific(worker_db_key, wdb);
    return wdb;
}

/* ── Shared "get + check + prepare" for runtime bindings ──────────── */

int hl_worker_db_get_and_prepare(const char    *sql,
                                 sqlite3      **out_db,
                                 sqlite3_stmt **out_stmt,
                                 const char   **out_err)
{
    static const char *e_no_db     = "database not available in worker";
    static const char *e_namespace = "access denied: _hull_* tables are reserved";

    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->db) {
        if (out_err) *out_err = e_no_db;
        return -1;
    }
    if (out_db) *out_db = wdb->db;

    if (hl_cap_db_check_namespace(sql) != 0) {
        if (out_err) *out_err = e_namespace;
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(wdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (out_err) *out_err = sqlite3_errmsg(wdb->db);
        return -1;
    }

    if (out_stmt) *out_stmt = stmt;
    return 0;
}

/* ── HlDbResult helpers ────────────────────────────────────────────── */

void hl_db_result_free(HlDbResult *r)
{
    if (!r) return;
    if (r->col_names) {
        for (int i = 0; i < r->ncols; i++)
            free(r->col_names[i]);
        free(r->col_names);
    }
    if (r->values) {
        int total = r->nrows * r->ncols;
        for (int i = 0; i < total; i++) {
            if (r->values[i].s)
                free(r->values[i].s);
        }
        free(r->values);
    }
    memset(r, 0, sizeof(*r));
}

/* Grow result to hold at least one more row */
static int db_result_grow(HlDbResult *r)
{
    if (r->nrows < r->capacity)
        return 0;
    int new_cap = r->capacity ? r->capacity * 2 : 16;
    if (new_cap > 100000) return -1; /* sanity cap */
    if (r->ncols > 0 &&
        (size_t)new_cap > SIZE_MAX / ((size_t)r->ncols * sizeof(HlDbValue)))
        return -1; /* overflow guard */
    HlDbValue *nv = realloc(r->values,
                             (size_t)new_cap * (size_t)r->ncols * sizeof(HlDbValue));
    if (!nv) return -1;
    r->values = nv;
    r->capacity = new_cap;
    return 0;
}

/* Deep-copy a SQLite column value into an HlDbValue */
static void materialize_column(sqlite3_stmt *stmt, int col, HlDbValue *out)
{
    memset(out, 0, sizeof(*out));
    switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_INTEGER:
        out->type = HL_TYPE_INT;
        out->i    = sqlite3_column_int64(stmt, col);
        break;
    case SQLITE_FLOAT:
        out->type = HL_TYPE_DOUBLE;
        out->d    = sqlite3_column_double(stmt, col);
        break;
    case SQLITE_TEXT: {
        out->type = HL_TYPE_TEXT;
        const char *text = (const char *)sqlite3_column_text(stmt, col);
        size_t len = (size_t)sqlite3_column_bytes(stmt, col);
        if (!text) { out->type = HL_TYPE_NIL; break; }
        out->s = malloc(len + 1);
        if (out->s) {
            memcpy(out->s, text, len);
            out->s[len] = '\0';
            out->len = len;
        } else {
            out->type = HL_TYPE_NIL;
        }
        break;
    }
    case SQLITE_BLOB: {
        out->type = HL_TYPE_BLOB;
        const void *blob = sqlite3_column_blob(stmt, col);
        size_t len = (size_t)sqlite3_column_bytes(stmt, col);
        if (!blob) { out->type = HL_TYPE_NIL; break; }
        out->s = malloc(len);
        if (out->s) {
            memcpy(out->s, blob, len);
            out->len = len;
        } else {
            out->type = HL_TYPE_NIL;
        }
        break;
    }
    case SQLITE_NULL:
    default:
        out->type = HL_TYPE_NIL;
        break;
    }
}

/* ── Bind params (local copy of shared logic) ──────────────────────── */

static int worker_bind_params(sqlite3_stmt *stmt, const HlValue *params, int n)
{
    for (int i = 0; i < n; i++) {
        int rc;
        int idx = i + 1;
        switch (params[i].type) {
        case HL_TYPE_NIL:
            rc = sqlite3_bind_null(stmt, idx);
            break;
        case HL_TYPE_INT:
            rc = sqlite3_bind_int64(stmt, idx, params[i].i);
            break;
        case HL_TYPE_DOUBLE:
            rc = sqlite3_bind_double(stmt, idx, params[i].d);
            break;
        case HL_TYPE_TEXT:
            if (params[i].len > (size_t)INT_MAX) return -1;
            rc = sqlite3_bind_text(stmt, idx, params[i].s,
                                   (int)params[i].len, SQLITE_TRANSIENT);
            break;
        case HL_TYPE_BLOB:
            if (params[i].len > (size_t)INT_MAX) return -1;
            rc = sqlite3_bind_blob(stmt, idx, params[i].s,
                                   (int)params[i].len, SQLITE_TRANSIENT);
            break;
        case HL_TYPE_BOOL:
            rc = sqlite3_bind_int(stmt, idx, params[i].b ? 1 : 0);
            break;
        default:
            return -1;
        }
        if (rc != SQLITE_OK)
            return -1;
    }
    return 0;
}

/* ── KlWorkItem callbacks ──────────────────────────────────────────── */

static void db_work_fn(void *ud)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)ud;
    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "failed to open worker DB connection");
        return;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(wdb->db, op->sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "prepare: %s", sqlite3_errmsg(wdb->db));
        return;
    }

    if (op->nparams > 0 && worker_bind_params(stmt, op->params, op->nparams) != 0) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "bind: %s", sqlite3_errmsg(wdb->db));
        sqlite3_finalize(stmt);
        return;
    }

    if (op->kind == HL_WORK_DB_EXEC) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            op->error = 1;
            snprintf(op->error_msg, sizeof(op->error_msg),
                     "exec: %s", sqlite3_errmsg(wdb->db));
        } else {
            op->exec_changes = sqlite3_changes(wdb->db);
            op->last_id = sqlite3_last_insert_rowid(wdb->db);
        }
        sqlite3_finalize(stmt);
        return;
    }

    /* HL_WORK_DB_QUERY — materialize all rows */
    HlDbResult *r = &op->result;
    memset(r, 0, sizeof(*r));

    int first = 1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (first) {
            r->ncols = sqlite3_column_count(stmt);
            r->col_names = calloc((size_t)r->ncols, sizeof(char *));
            if (!r->col_names) {
                op->error = 1;
                snprintf(op->error_msg, sizeof(op->error_msg), "out of memory");
                sqlite3_finalize(stmt);
                return;
            }
            for (int i = 0; i < r->ncols; i++) {
                const char *name = sqlite3_column_name(stmt, i);
                r->col_names[i] = strdup(name ? name : "?");
                if (!r->col_names[i]) {
                    op->error = 1;
                    snprintf(op->error_msg, sizeof(op->error_msg),
                             "out of memory");
                    sqlite3_finalize(stmt);
                    return;
                }
            }
            first = 0;
        }

        if (db_result_grow(r) != 0) {
            op->error = 1;
            snprintf(op->error_msg, sizeof(op->error_msg), "too many rows");
            sqlite3_finalize(stmt);
            return;
        }

        HlDbValue *row = &r->values[r->nrows * r->ncols];
        for (int i = 0; i < r->ncols; i++)
            materialize_column(stmt, i, &row[i]);
        r->nrows++;
    }

    if (rc != SQLITE_DONE) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "query: %s", sqlite3_errmsg(wdb->db));
    }

    sqlite3_finalize(stmt);
}

static void db_done_fn(void *ud)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)ud;
    if (op->cancelled) {
        /* Connection closed while work was in flight — clean up.
         * The cont was already freed by hl_worker_db_async_cancel. */
        HlAsyncCtx *ctx = op->async_ctx;
        hl_worker_db_op_free(op);
        free(op);
        if (ctx) hl_async_ctx_free(ctx);
        return;
    }
    kl_async_complete(op->server, &op->async_ctx->op);
}

static void db_cancel_fn(void *ud)
{
    /* Called by kl_thread_pool_free for items that never ran.
     * The async cancel handler already freed the cont (if it ran). */
    HlWorkerDbOp *op = (HlWorkerDbOp *)ud;
    HlAsyncCtx *ctx = op->async_ctx;
    hl_worker_db_op_free(op);
    free(op);
    if (ctx) hl_async_ctx_free(ctx);
}

/* ── Submit / free ─────────────────────────────────────────────────── */

int hl_worker_db_submit(HlAsyncBackendPool *pool, HlWorkerDbOp *op)
{
    if (!pool || !op) return -1;
    const HlAsyncBackend *be = hl_async_backend();
    return be->pool_submit(pool, db_work_fn, db_done_fn, db_cancel_fn, op);
}

/* Deep-copy helper for HlValue params */
HlValue *hl_deep_copy_params(const HlValue *src, int n)
{
    if (n <= 0 || !src) return NULL;
    HlValue *dst = calloc((size_t)n, sizeof(HlValue));
    if (!dst) return NULL;
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
        if ((src[i].type == HL_TYPE_TEXT || src[i].type == HL_TYPE_BLOB) &&
            src[i].s && src[i].len > 0) {
            dst[i].s = malloc(src[i].len + 1);
            if (!dst[i].s) {
                /* Clean up already-copied strings */
                for (int j = 0; j < i; j++) {
                    if ((dst[j].type == HL_TYPE_TEXT || dst[j].type == HL_TYPE_BLOB) && dst[j].s)
                        free((void *)dst[j].s);
                }
                free(dst);
                return NULL;
            }
            memcpy((void *)dst[i].s, src[i].s, src[i].len);
            ((char *)dst[i].s)[src[i].len] = '\0';
        }
    }
    return dst;
}

void hl_worker_db_op_free(HlWorkerDbOp *op)
{
    if (!op) return;
    free(op->sql);
    if (op->params) {
        for (int i = 0; i < op->nparams; i++) {
            if ((op->params[i].type == HL_TYPE_TEXT ||
                 op->params[i].type == HL_TYPE_BLOB) && op->params[i].s)
                free((void *)op->params[i].s);
        }
        free(op->params);
    }
    hl_db_result_free(&op->result);
}

void hl_worker_db_op_free_all(void *ptr)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)ptr;
    hl_worker_db_op_free(op);
    free(op);
}

void hl_worker_db_async_cancel(KlAsyncOp *kl_op, void *user_data)
{
    (void)kl_op;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlWorkerDbOp *op = (HlWorkerDbOp *)ctx->driver;

    /* Mark as cancelled — done_fn will handle cleanup */
    op->cancelled = 1;

    /* Cancel and destroy the runtime continuation */
    if (ctx->cont) {
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
    }
    /* Don't free ctx or op — done_fn needs them */
}

#endif /* HL_ENABLE_DB */
