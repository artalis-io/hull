/*
 * cap/db_mysql.c - MySQL / MariaDB backend (HlDbBackend vtable adapter)
 *
 * Pure-C wire client, no libmysql/libmariadb (mirrors the PostgreSQL backend:
 * cap/mysqlwire.c codec + cap/mysql_conn.c connection + this vtable adapter).
 * One backend serves `mysql://` and `mariadb://` (shared protocol; MariaDB is a
 * MySQL fork).
 *
 * Phase 2c: connect + COM_QUERY text protocol. Parameterized queries + the
 * insert_if_absent / upsert / table_columns dialect helpers need the binary
 * prepared-statement protocol and fail with a clear message until Phase 3;
 * multi-statement exec_script (migrations) is Phase 4.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_MYSQL

#include "hull/cap/db_backend.h"
#include "hull/cap/db_mysql.h"
#include "hull/cap/mysql_conn.h"
#include "hull/cap/mysqlwire.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct HlDbMyCtx {
    HlMyConn conn;
} HlDbMyCtx;

/* ── Text value -> HlValue by column type ─────────────────────────── */

static void decode_my_value(uint8_t type, const char *text, size_t len,
                            HlValue *out)
{
    if (!text) { out->type = HL_TYPE_NIL; return; }
    switch (type) {
    case HL_MY_TYPE_TINY:  case HL_MY_TYPE_SHORT: case HL_MY_TYPE_LONG:
    case HL_MY_TYPE_LONGLONG: case HL_MY_TYPE_INT24: case HL_MY_TYPE_YEAR: {
        char buf[32];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, text, n); buf[n] = '\0';
        out->type = HL_TYPE_INT;
        out->i = (int64_t)strtoll(buf, NULL, 10);
        return;
    }
    case HL_MY_TYPE_FLOAT: case HL_MY_TYPE_DOUBLE: {
        char buf[64];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, text, n); buf[n] = '\0';
        out->type = HL_TYPE_DOUBLE;
        out->d = strtod(buf, NULL);
        return;
    }
    default:   /* strings / blobs / decimals / dates: keep as borrowed text */
        out->type = HL_TYPE_TEXT;
        out->s = text;   /* points into the frame; valid for the row cb */
        out->len = len;
        return;
    }
}

/* ── Adapter: hl_my_conn_query callbacks -> HlRowCallback ──────────── */

typedef struct {
    HlRowCallback user_cb;
    void         *user_ctx;
    char        **names;   /* copied field names */
    uint8_t      *types;
    int           nfields;
} MyAdapter;

static void adapter_desc(void *ctx, const HlMyField *fields, int nf)
{
    MyAdapter *a = ctx;
    a->nfields = nf;
    if (nf <= 0) return;
    a->names = calloc((size_t)nf, sizeof *a->names);
    a->types = calloc((size_t)nf, sizeof *a->types);
    if (!a->names || !a->types) { a->nfields = 0; return; }
    for (int i = 0; i < nf; i++) {
        a->names[i] = strdup(fields[i].name ? fields[i].name : "");
        a->types[i] = fields[i].type;
    }
}

static int adapter_row(void *ctx, const char *const *vals,
                       const size_t *lens, int nc)
{
    MyAdapter *a = ctx;
    if (!a->user_cb) return 0;
    if (a->nfields > 0 && nc > a->nfields) nc = a->nfields;

    HlColumn *cols = calloc((size_t)(nc > 0 ? nc : 1), sizeof *cols);
    if (!cols) return 1;
    for (int i = 0; i < nc; i++) {
        cols[i].name = (a->names && a->names[i]) ? a->names[i] : "";
        uint8_t type = a->types ? a->types[i] : HL_MY_TYPE_STRING;
        decode_my_value(type, vals[i], vals[i] ? lens[i] : 0, &cols[i].value);
    }
    int rc = a->user_cb(a->user_ctx, cols, nc);
    free(cols);
    return rc ? 1 : 0;
}

/* ── Vtable methods ───────────────────────────────────────────────── */

static int mysql_open(void **out_ctx, const char *dsn, HlAllocator *alloc)
{
    (void)alloc;
    HlDbMyCtx *s = calloc(1, sizeof *s);
    if (!s) return -1;

    HlMyDsn parsed;
    char err[128];
    if (hl_my_dsn_parse(dsn, &parsed, err, sizeof err) != 0) { free(s); return -1; }
    int rc = hl_my_conn_open(&s->conn, &parsed, 10000 /* 10s connect */);
    hl_my_dsn_scrub(&parsed);   /* password is secret material */
    if (rc != 0) { free(s); return -1; }

    *out_ctx = s;
    return 0;
}

static void mysql_close(HlDbHandle *h)
{
    if (!h || !h->ctx) return;
    HlDbMyCtx *s = h->ctx;
    hl_my_conn_close(&s->conn);
    free(s);
    h->ctx = NULL;
}

static int mysql_query(HlDbHandle *h, const char *sql,
                       const HlValue *params, int nparams,
                       HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    (void)params; (void)alloc;
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    if (nparams > 0) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "MySQL parameterized queries need prepared statements (Phase 3)");
        return -1;
    }
    MyAdapter a;
    memset(&a, 0, sizeof a);
    a.user_cb = cb;
    a.user_ctx = cb_ctx;
    int rc = hl_my_conn_query(&s->conn, sql,
                              cb ? adapter_desc : NULL,
                              cb ? adapter_row : NULL, &a, NULL);
    if (a.names)
        for (int i = 0; i < a.nfields; i++) free(a.names[i]);
    free(a.names);
    free(a.types);
    return rc;
}

static int mysql_exec(HlDbHandle *h, const char *sql,
                      const HlValue *params, int nparams)
{
    (void)params;
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    if (nparams > 0) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "MySQL parameterized exec needs prepared statements (Phase 3)");
        return -1;
    }
    int64_t affected = 0;
    if (hl_my_conn_query(&s->conn, sql, NULL, NULL, NULL, &affected) != 0)
        return -1;
    return (int)(affected < 0 ? 0 : affected);
}

static int mysql_exec_script(HlDbHandle *h, const char *sql)
{
    /* Single statement only for now: without CLIENT_MULTI_STATEMENTS a
     * multi-statement COM_QUERY errors. Multi-statement migrations are Phase 4. */
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    return hl_my_conn_query(&s->conn, sql, NULL, NULL, NULL, NULL);
}

static int mysql_txn(HlDbHandle *h, const char *sql)
{
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    return hl_my_conn_query(&s->conn, sql, NULL, NULL, NULL, NULL);
}

static int mysql_begin(HlDbHandle *h)    { return mysql_txn(h, "START TRANSACTION"); }
static int mysql_commit(HlDbHandle *h)   { return mysql_txn(h, "COMMIT"); }
static int mysql_rollback(HlDbHandle *h) { return mysql_txn(h, "ROLLBACK"); }

static int64_t mysql_last_id(HlDbHandle *h)
{
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    return (int64_t)s->conn.last_insert_id;
}

static const char *mysql_errmsg(HlDbHandle *h)
{
    if (!h || !h->ctx) return "no database";
    HlDbMyCtx *s = h->ctx;
    return s->conn.errmsg[0] ? s->conn.errmsg : "no error";
}

/* Dialect helpers below need bound parameters (prepared statements) or safe
 * identifier interpolation; both land in Phase 3. */
static int mysql_insert_if_absent(HlDbHandle *h, const char *table,
                                  const char *const *conflict_cols,
                                  int n_conflict, const char *const *cols,
                                  const HlValue *values, int n_cols)
{
    (void)table; (void)conflict_cols; (void)n_conflict;
    (void)cols; (void)values; (void)n_cols;
    if (h && h->ctx) {
        HlDbMyCtx *s = h->ctx;
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "MySQL insert_if_absent needs prepared statements (Phase 3)");
    }
    return -1;
}

static int mysql_upsert(HlDbHandle *h, const char *table,
                        const char *const *conflict_cols, int n_conflict,
                        const char *const *cols,
                        const HlValue *values, int n_cols)
{
    (void)table; (void)conflict_cols; (void)n_conflict;
    (void)cols; (void)values; (void)n_cols;
    if (h && h->ctx) {
        HlDbMyCtx *s = h->ctx;
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "MySQL upsert needs prepared statements (Phase 3)");
    }
    return -1;
}

static int mysql_table_columns(HlDbHandle *h, const char *table,
                               HlDbColumnCallback cb, void *cb_ctx)
{
    (void)table; (void)cb; (void)cb_ctx;
    if (h && h->ctx) {
        HlDbMyCtx *s = h->ctx;
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "MySQL table_columns not implemented yet (Phase 4)");
    }
    return -1;
}

/* Both schemes route here; MariaDB shares the MySQL protocol. */
static const char *const mysql_schemes[] = { "mysql", "mariadb", NULL };

const HlDbBackend hl_db_backend_mysql = {
    .name                  = "mysql",
    .schemes               = mysql_schemes,
    .autoincrement_id_ddl  = "BIGINT AUTO_INCREMENT PRIMARY KEY",
    .identifier_quote      = '`',
    .native_tag            = HL_DB_NATIVE_MYSQL,
    .supports_udf          = 0,
    .open                  = mysql_open,
    .close                 = mysql_close,
    .query                 = mysql_query,
    .exec                  = mysql_exec,
    .exec_script           = mysql_exec_script,
    .begin                 = mysql_begin,
    .commit                = mysql_commit,
    .rollback              = mysql_rollback,
    .last_id               = mysql_last_id,
    .errmsg                = mysql_errmsg,
    .insert_if_absent      = mysql_insert_if_absent,
    .upsert                = mysql_upsert,
    .table_columns         = mysql_table_columns,
    /* native_handle: no consumer (udf / agent introspection are SQLite-only). */
};

#endif /* HL_ENABLE_MYSQL */
