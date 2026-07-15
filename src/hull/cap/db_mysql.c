/*
 * cap/db_mysql.c - MySQL / MariaDB backend (HlDbBackend vtable adapter)
 *
 * Pure-C wire client, no libmysql/libmariadb (mirrors the PostgreSQL backend:
 * cap/mysqlwire.c codec + cap/mysql_conn.c connection + this vtable adapter).
 * One backend serves `mysql://` and `mariadb://` (shared protocol; MariaDB is a
 * MySQL fork).
 *
 * Phase 3: connect, COM_QUERY text protocol (param-less), and parameterized
 * query / exec via the binary prepared-statement protocol (COM_STMT_PREPARE /
 * EXECUTE / CLOSE). The insert_if_absent / upsert dialect helpers need MySQL's
 * INSERT IGNORE / ON DUPLICATE KEY syntax and the information_schema-backed
 * table_columns; both, plus multi-statement exec_script (migrations), land in
 * Phase 4.
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
#include <strings.h>   /* strncasecmp */
#include <stdio.h>
#include <ctype.h>

typedef struct HlDbMyCtx {
    HlMyConn conn;
} HlDbMyCtx;

/* Case-insensitive substring search (strcasestr is not standard C). */
static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return hay;
    return NULL;
}

/*
 * MySQL 8 has no `CREATE INDEX ... IF NOT EXISTS` (MariaDB, SQLite, and
 * Postgres all do). The DB-backed stdlib writes idempotent index DDL in that
 * portable form, so on a MySQL connection rewrite it to a plain CREATE INDEX
 * and treat a later duplicate-index error as success. Sets *handled when the
 * statement was a CREATE INDEX ... IF NOT EXISTS. Returns 0 / -1.
 *
 * The `index` / `if not exists` match is a loose case-insensitive substring
 * scan: acceptable because the ONLY callers are Hull's own stdlib index DDL
 * (trusted, fixed strings), never app-supplied SQL. App queries never reach
 * here as a CREATE INDEX IF NOT EXISTS, and parameter values are bound out of
 * band, so this is not an injection surface.
 */
static int mysql_create_index_shim(HlDbMyCtx *s, const char *sql, int *handled)
{
    *handled = 0;
    const char *p = sql;
    while (isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "create ", 7) != 0) return 0;
    const char *idx = ci_strstr(p, "index");
    const char *ine = ci_strstr(p, "if not exists");
    if (!idx || !ine || idx > ine) return 0;   /* not CREATE INDEX IF NOT EXISTS */

    *handled = 1;
    size_t pre = (size_t)(ine - sql);
    const char *rest = ine + strlen("if not exists");
    while (*rest == ' ') rest++;
    char *rw = malloc(pre + strlen(rest) + 1);
    if (!rw) { snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory"); return -1; }
    memcpy(rw, sql, pre);
    memcpy(rw + pre, rest, strlen(rest) + 1);

    int rc = hl_my_conn_query(&s->conn, rw, NULL, NULL, NULL, NULL);
    free(rw);
    if (rc == 0) return 0;
    if (ci_strstr(s->conn.errmsg, "Duplicate key name") != NULL) return 0;  /* already present */
    return -1;
}

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

/* ── Adapter: prepared-statement binary rows -> HlRowCallback ──────── */

typedef struct {
    HlRowCallback user_cb;
    void         *user_ctx;
    char        **names;
    int           nfields;
} MyBinAdapter;

static void bin_adapter_desc(void *ctx, const HlMyField *fields, int nf)
{
    MyBinAdapter *a = ctx;
    a->nfields = nf;
    if (nf <= 0) return;
    a->names = calloc((size_t)nf, sizeof *a->names);
    if (!a->names) { a->nfields = 0; return; }
    for (int i = 0; i < nf; i++)
        a->names[i] = strdup(fields[i].name ? fields[i].name : "");
}

static int bin_adapter_row(void *ctx, const HlMyVal *vals, int nc)
{
    MyBinAdapter *a = ctx;
    if (!a->user_cb) return 0;
    if (a->nfields > 0 && nc > a->nfields) nc = a->nfields;

    HlColumn *cols = calloc((size_t)(nc > 0 ? nc : 1), sizeof *cols);
    if (!cols) return 1;
    for (int i = 0; i < nc; i++) {
        cols[i].name = (a->names && a->names[i]) ? a->names[i] : "";
        switch (vals[i].kind) {
        case HL_MY_VAL_INT:
            cols[i].value.type = HL_TYPE_INT;    cols[i].value.i = vals[i].v.i; break;
        case HL_MY_VAL_DOUBLE:
            cols[i].value.type = HL_TYPE_DOUBLE; cols[i].value.d = vals[i].v.d; break;
        case HL_MY_VAL_STR:
            cols[i].value.type = HL_TYPE_TEXT;
            cols[i].value.s = vals[i].v.s.ptr;   cols[i].value.len = vals[i].v.s.len; break;
        case HL_MY_VAL_NULL:
        default:
            cols[i].value.type = HL_TYPE_NIL; break;
        }
    }
    int rc = a->user_cb(a->user_ctx, cols, nc);
    free(cols);
    return rc ? 1 : 0;
}

/* Convert Hull's HlValue params to wire-level HlMyParam. Strings/blobs borrow
 * the HlValue bytes, so the returned array is valid only while @p params is. */
static HlMyParam *encode_my_params(const HlValue *params, int n)
{
    if (n <= 0) return NULL;
    HlMyParam *p = calloc((size_t)n, sizeof *p);
    if (!p) return NULL;
    for (int i = 0; i < n; i++) {
        switch (params[i].type) {
        case HL_TYPE_INT:
            p[i].type = HL_MY_TYPE_LONGLONG;  p[i].v.i = params[i].i; break;
        case HL_TYPE_BOOL:
            p[i].type = HL_MY_TYPE_TINY;      p[i].v.i = params[i].b ? 1 : 0; break;
        case HL_TYPE_DOUBLE:
            p[i].type = HL_MY_TYPE_DOUBLE;    p[i].v.d = params[i].d; break;
        case HL_TYPE_TEXT:
            p[i].type = HL_MY_TYPE_VAR_STRING;
            p[i].v.s.ptr = params[i].s;       p[i].v.s.len = params[i].len; break;
        case HL_TYPE_BLOB:
            p[i].type = HL_MY_TYPE_BLOB;
            p[i].v.s.ptr = params[i].s;       p[i].v.s.len = params[i].len; break;
        case HL_TYPE_NIL:
        default:
            p[i].is_null = 1; p[i].type = HL_MY_TYPE_NULL; break;
        }
    }
    return p;
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

    /* Parameterized: bind through the binary prepared-statement protocol so the
     * values never touch the SQL text (injection-safe). */
    if (nparams > 0) {
        HlMyParam *pp = encode_my_params(params, nparams);
        if (!pp) { snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory"); return -1; }
        MyBinAdapter ba;
        memset(&ba, 0, sizeof ba);
        ba.user_cb = cb;
        ba.user_ctx = cb_ctx;
        int rc = hl_my_conn_query_prepared(&s->conn, sql, pp, nparams,
                                           cb ? bin_adapter_desc : NULL,
                                           cb ? bin_adapter_row : NULL, &ba, NULL);
        if (ba.names)
            for (int i = 0; i < ba.nfields; i++) free(ba.names[i]);
        free(ba.names);
        free(pp);
        return rc;
    }

    /* No params: the simpler COM_QUERY text protocol. */
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
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int64_t affected = 0;

    if (nparams == 0) {
        int handled = 0;
        int shim = mysql_create_index_shim(s, sql, &handled);
        if (handled) return shim == 0 ? 0 : -1;   /* DDL: 0 rows affected */
    }

    if (nparams > 0) {
        HlMyParam *pp = encode_my_params(params, nparams);
        if (!pp) { snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory"); return -1; }
        int rc = hl_my_conn_query_prepared(&s->conn, sql, pp, nparams,
                                           NULL, NULL, NULL, &affected);
        free(pp);
        if (rc != 0) return -1;
        return (int)(affected < 0 ? 0 : affected);
    }

    if (hl_my_conn_query(&s->conn, sql, NULL, NULL, NULL, &affected) != 0)
        return -1;
    return (int)(affected < 0 ? 0 : affected);
}

static int mysql_exec_script(HlDbHandle *h, const char *sql)
{
    /* Migrations are multi-statement; the connection advertises
     * CLIENT_MULTI_STATEMENTS so the whole script runs as one COM_QUERY. */
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    return hl_my_conn_exec_multi(&s->conn, sql);
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

/* ── Dialect helpers ──────────────────────────────────────────────── */

/* Append to a bounded buffer; latch overflow via *off >= cap (checked once). */
static void ap(char *buf, size_t cap, size_t *off, const char *str)
{
    size_t n = strlen(str);
    if (*off + n < cap) memcpy(buf + *off, str, n);
    *off += n;
}

/*
 * Build and run `INSERT [IGNORE] INTO t (cols) VALUES (?,...) [ON DUPLICATE KEY
 * UPDATE c = VALUES(c), ...]`, binding @p values through the prepared-statement
 * path. MySQL keys the conflict off any UNIQUE / PRIMARY index, so
 * @p conflict_cols is unused (kept for the vtable signature parity).
 */
static int build_and_run_insert(HlDbHandle *h, const char *table,
                                const char *const *cols,
                                const HlValue *values, int n_cols, int do_update)
{
    /* Each column can appear up to three times (insert list + both sides of
     * `c = VALUES(c)`); budget 3x + overhead so a long name never truncates. */
    size_t cap = 64 + strlen(table);
    for (int i = 0; i < n_cols; i++) cap += strlen(cols[i]) * 3 + 24;
    char *sql = malloc(cap);
    if (!sql) return -1;
    size_t off = 0;

    ap(sql, cap, &off, do_update ? "INSERT INTO " : "INSERT IGNORE INTO ");
    ap(sql, cap, &off, table);
    ap(sql, cap, &off, " (");
    for (int i = 0; i < n_cols; i++) {
        if (i) ap(sql, cap, &off, ", ");
        ap(sql, cap, &off, cols[i]);
    }
    ap(sql, cap, &off, ") VALUES (");
    for (int i = 0; i < n_cols; i++)
        ap(sql, cap, &off, i ? ", ?" : "?");
    ap(sql, cap, &off, ")");
    if (do_update) {
        ap(sql, cap, &off, " ON DUPLICATE KEY UPDATE ");
        for (int i = 0; i < n_cols; i++) {
            if (i) ap(sql, cap, &off, ", ");
            ap(sql, cap, &off, cols[i]);
            ap(sql, cap, &off, " = VALUES(");
            ap(sql, cap, &off, cols[i]);
            ap(sql, cap, &off, ")");
        }
    }

    if (off >= cap) {                            /* estimate was too small */
        free(sql);
        HlDbMyCtx *s = h ? h->ctx : NULL;
        if (s)
            snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                     "insert/upsert SQL exceeded its estimated buffer");
        return -1;
    }
    sql[off] = '\0';

    int rc = mysql_exec(h, sql, values, n_cols);
    free(sql);
    return rc;
}

static int mysql_insert_if_absent(HlDbHandle *h, const char *table,
                                  const char *const *conflict_cols,
                                  int n_conflict, const char *const *cols,
                                  const HlValue *values, int n_cols)
{
    (void)conflict_cols; (void)n_conflict;
    return build_and_run_insert(h, table, cols, values, n_cols, 0);
}

static int mysql_upsert(HlDbHandle *h, const char *table,
                        const char *const *conflict_cols, int n_conflict,
                        const char *const *cols,
                        const HlValue *values, int n_cols)
{
    (void)conflict_cols; (void)n_conflict;
    return build_and_run_insert(h, table, cols, values, n_cols, 1);
}

typedef struct {
    HlDbColumnCallback cb;
    void              *cb_ctx;
} MyColsFwd;

static int mysql_table_columns_row(void *ctx, HlColumn *cols, int ncols)
{
    MyColsFwd *fwd = ctx;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].value.type != HL_TYPE_TEXT || !cols[i].value.s)
            continue;
        /* The text value borrows non-NUL-terminated wire bytes; copy + terminate
         * with the tracked length before handing a C string to the callback. */
        size_t len = cols[i].value.len;
        char stackbuf[128];
        char *name = (len < sizeof stackbuf) ? stackbuf : malloc(len + 1);
        if (!name) continue;
        memcpy(name, cols[i].value.s, len);
        name[len] = '\0';
        fwd->cb(fwd->cb_ctx, name);
        if (name != stackbuf) free(name);
    }
    return 0;
}

static int mysql_table_columns(HlDbHandle *h, const char *table,
                               HlDbColumnCallback cb, void *cb_ctx)
{
    MyColsFwd fwd = { cb, cb_ctx };
    HlValue p;
    memset(&p, 0, sizeof p);
    p.type = HL_TYPE_TEXT;
    p.s = table;
    p.len = strlen(table);
    /* Scope to the connection's own schema so a same-named table in another
     * database doesn't leak columns. */
    return mysql_query(h,
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_name = ? AND table_schema = DATABASE() "
        "ORDER BY ordinal_position",
        &p, 1, mysql_table_columns_row, &fwd, NULL);
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
