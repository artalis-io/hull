/*
 * cap/db_mysql.c - MySQL / MariaDB backend (HlDbBackend vtable adapter)
 *
 * Pure-C wire client, no libmysql/libmariadb (mirrors the PostgreSQL backend:
 * cap/mysqlwire.c codec + cap/mysql_conn.c connection + this vtable adapter).
 * One backend serves `mysql://` and `mariadb://` (shared protocol; MariaDB is a
 * MySQL fork).
 *
 * Provides connect, the COM_QUERY text protocol (param-less), and parameterized
 * query / exec via the binary prepared-statement protocol (COM_STMT_PREPARE /
 * EXECUTE / CLOSE). The insert_if_absent / upsert dialect helpers use MySQL's
 * INSERT IGNORE / ON DUPLICATE KEY syntax and the information_schema-backed
 * table_columns, plus multi-statement exec_script (migrations).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_MYSQL

#include "hull/cap/db_backend.h"
#include "hull/cap/db_mysql.h"
#include "hull/cap/mysql_conn.h"
#include "hull/cap/mysqlwire.h"
#include "hull/cap/db_sql_kw.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

typedef struct HlDbMyCtx {
    HlMyConn conn;
    char    *dsn;       /* to reconnect (secret: scrubbed on close) */
    /* InnoDB rolled the open transaction back under a failed statement (a
     * deadlock, or a lock-wait timeout under innodb_rollback_on_timeout):
     * later statements would each autocommit and the eventual
     * COMMIT would succeed, so calls refuse until a ROLLBACK (my_ready). */
    int      txn_aborted;
} HlDbMyCtx;

static int my_connect(HlMyConn *conn, const char *dsn)
{
    HlMyDsn parsed;
    char err[128];
    if (hl_my_dsn_parse(dsn, &parsed, err, sizeof err) != 0) {
        /* A parse that failed part-way (a bad port after the password) has
         * already copied the password into @p parsed. */
        hl_my_dsn_scrub(&parsed);
        return -1;
    }
    int rc = hl_my_conn_open(conn, &parsed, 10000 /* 10s connect */);
    hl_my_dsn_scrub(&parsed);   /* password is secret material */
    return rc;
}

/* 1 when @p sql is a ROLLBACK of the whole transaction (comments and
 * ROLLBACK WORK included; not ROLLBACK TO SAVEPOINT). A connection lost inside
 * a transaction lost the transaction with it - the server rolled it back - so
 * a ROLLBACK for it has already happened. */
static int sql_is_rollback(const char *sql)
{
    return hl_sql_txn_kind(sql) == HL_SQL_TXN_ROLLBACK;
}

/* 1 when @p sql has text MySQL runs or skips differently from the shared
 * reader (cap/db_sql_kw.h): an executable comment (slash-star-bang, and
 * MariaDB's slash-star-M-bang), which MySQL runs though the reader skips it, or a
 * '#' line comment, which the reader does not skip (audit 9 L1). A '#' in a
 * string literal counts too: misreading such a statement as unrecognised
 * only refuses a resume, never makes one. */
static int my_sql_unreadable(const char *sql)
{
    return sql && (strstr(sql, "/*!") || strstr(sql, "/*M!") ||
                   strchr(sql, '#'));
}

/* hl_sql_txn_kind for MySQL: a statement the reader cannot read reliably is
 * HL_SQL_TXN_OTHER - no implicit-commit resume after it, so a transaction it
 * ended is reported lost by the batch. */
static HlSqlTxnKind my_sql_txn_kind(const char *sql)
{
    return my_sql_unreadable(sql) ? HL_SQL_TXN_OTHER : hl_sql_txn_kind(sql);
}

static void scrub_free(char *s)
{
    if (!s) return;
    volatile char *p = s;
    while (*p) *p++ = 0;
    free(s);
}

/* Before any use: replace a connection left out of step (HlMyConn.broken), as
 * db_postgres.c's pg_ready does - outside a transaction only. Inside one the
 * server rolled the transaction back when the connection went, and statements
 * run on a new connection would each commit alone, so every call refuses until
 * a ROLLBACK (@p sql), which has in effect already happened and returns 1.
 * Returns 0 to go ahead, 1 done, -1 refused. A @p pinned handle (it holds a
 * session-scoped lock: hl_migrate_run) is not reconnected either: the lock
 * went with the session, and the run would go on without it (audit 6 L5). */
static int my_ready(HlDbMyCtx *s, const char *sql, int pinned)
{
    if (s->txn_aborted) {
        if (sql_is_rollback(sql)) {   /* the server already rolled back */
            s->txn_aborted = 0;
            s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
            return 1;
        }
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the server rolled this transaction back (a deadlock or a "
                 "lock-wait timeout), so none of its statements were "
                 "applied; roll back and retry");
        return -1;
    }
    if (!s->conn.broken)
        return 0;
    if ((s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS) && sql_is_rollback(sql)) {
        s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
        return 1;
    }
    if (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the connection was lost inside a transaction, which the "
                 "server rolled back; roll back and retry");
        return -1;
    }
    if (pinned) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the connection was lost while it held a session lock (the "
                 "migration lock), which went with it; not reconnecting");
        return -1;
    }
    HlMyConn fresh;
    memset(&fresh, 0, sizeof fresh);
    if (my_connect(&fresh, s->dsn) != 0) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the connection was lost, and reconnecting failed: %.150s",
                 fresh.errmsg[0] ? fresh.errmsg : "unknown error");
        return -1;
    }
    hl_my_conn_close(&s->conn);
    s->conn = fresh;
    return 0;
}

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
    /* By the server's error code, not its message (audit 9 L2): the message
     * follows lc_messages, and an app-chosen index name can put the words in
     * another error's text. */
    if (s->conn.last_err_code == HL_MY_ER_DUP_KEYNAME) {
        /* Already present. The DDL still committed the open transaction
         * before it failed, which the ERR does not say (audit 6 L1). */
        if (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS)
            (void)hl_my_conn_ping(&s->conn);
        return 0;
    }
    return -1;
}

/*
 * Hull's own tables (`_hull_*`) are created with a binary collation. The
 * stdlib keys them on VARCHAR columns (role names, inbox ids, dedup keys,
 * job / cron / subscription names) whose comparisons must be exact; under
 * MySQL 8's default utf8mb4_0900_ai_ci, 'Admin' = 'admin' and 'josé' =
 * 'jose', so rbac.has_role matched a role differing in case and two inbox
 * ids differing in case were one duplicate. Every `CREATE TABLE [IF NOT
 * EXISTS] _hull_...(...)` in @p sql gets `DEFAULT CHARSET=utf8mb4
 * COLLATE=utf8mb4_bin` after its column list, unless it names a collation
 * already. Tables created before keep theirs. Returns a malloc'd rewrite, or
 * NULL when nothing changed (or out of memory: the SQL then runs as given).
 */
static char *mysql_bin_collate_hull_tables(const char *sql)
{
    static const char opt[] = " DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin";
    size_t len = strlen(sql);
    char *out = NULL;
    size_t olen = 0, ocap = 0, copied = 0;
    const char *p = sql;
    while ((p = ci_strstr(p, "create table")) != NULL) {
        const char *q = p + strlen("create table");
        p = q;
        while (isspace((unsigned char)*q)) q++;
        if (strncasecmp(q, "if not exists", 13) == 0) {
            q += 13;
            while (isspace((unsigned char)*q)) q++;
        }
        if (*q == '`' || *q == '"') q++;
        if (strncasecmp(q, "_hull_", 6) != 0) continue;
        const char *lp = strchr(q, '(');
        if (!lp) break;
        /* The column list's closing parenthesis, past quoted text. */
        int depth = 0;
        char quote = 0;
        const char *r = lp;
        for (; *r; r++) {
            if (quote) { if (*r == quote) quote = 0; continue; }
            if (*r == '\'' || *r == '"' || *r == '`') { quote = *r; continue; }
            if (*r == '(') depth++;
            else if (*r == ')' && --depth == 0) break;
        }
        if (*r != ')') break;
        const char *stmt_end = strchr(r, ';');
        size_t tail = stmt_end ? (size_t)(stmt_end - r) : strlen(r);
        int named = 0;
        for (size_t i = 0; i + 7 <= tail; i++)
            if (strncasecmp(r + i, "collate", 7) == 0) { named = 1; break; }
        p = r + 1;
        if (named) continue;
        size_t chunk = (size_t)(r + 1 - (sql + copied));
        size_t need = olen + chunk + sizeof opt;
        if (need > ocap) {
            size_t ncap = ocap ? ocap * 2 : len + 256;
            while (ncap < need) ncap *= 2;
            char *nb = realloc(out, ncap);
            if (!nb) { free(out); return NULL; }
            out = nb; ocap = ncap;
        }
        memcpy(out + olen, sql + copied, chunk);
        olen += chunk;
        memcpy(out + olen, opt, sizeof opt - 1);
        olen += sizeof opt - 1;
        copied = (size_t)(r + 1 - sql);
    }
    if (!out) return NULL;
    size_t rest = len - copied;
    if (olen + rest + 1 > ocap) {
        char *nb = realloc(out, olen + rest + 1);
        if (!nb) { free(out); return NULL; }
        out = nb;
    }
    memcpy(out + olen, sql + copied, rest + 1);
    return out;
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
        errno = 0;
        long long v = strtoll(buf, NULL, 10);
        if (errno == ERANGE) {   /* BIGINT UNSIGNED above INT64_MAX: its text */
            out->type = HL_TYPE_TEXT;
            out->s = text;
            out->len = len;
            return;
        }
        out->type = HL_TYPE_INT;
        out->i = (int64_t)v;
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
    int           nnames;   /* entries allocated in names (what free walks) */
    int           bad_row;  /* a row did not match its column definitions */
} MyAdapter;

static void free_names(char **names, int n)
{
    if (names)
        for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* A result set's column definitions. A multi-statement query sends one per
 * result set: the next replaced the arrays without freeing them, and the
 * cleanup freed names[0..nfields) of the LAST count over whichever array
 * was left - out of bounds when it grew. On an allocation failure nfields
 * is -1 and every row is refused. */
static void adapter_desc(void *ctx, const HlMyField *fields, int nf)
{
    MyAdapter *a = ctx;
    free_names(a->names, a->nnames);
    free(a->types);
    a->names = NULL; a->types = NULL; a->nnames = 0;
    a->nfields = 0;
    if (nf <= 0) return;
    a->names = calloc((size_t)nf, sizeof *a->names);
    a->types = calloc((size_t)nf, sizeof *a->types);
    if (!a->names || !a->types) {
        free(a->names); free(a->types);
        a->names = NULL; a->types = NULL;
        a->nfields = -1;
        return;
    }
    a->nnames = nf;
    a->nfields = nf;
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
    /* Every row carries its result set's column count. Clamped only from
     * above, a short row reached a consumer that sized its rows from the
     * first (db.async's collector), which read past it. */
    if (nc != a->nfields) { a->bad_row = 1; return 1; }

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
    int           nnames;
    int           bad_row;
} MyBinAdapter;

static void bin_adapter_desc(void *ctx, const HlMyField *fields, int nf)
{
    MyBinAdapter *a = ctx;
    free_names(a->names, a->nnames);          /* see adapter_desc */
    a->names = NULL; a->nnames = 0;
    a->nfields = 0;
    if (nf <= 0) return;
    a->names = calloc((size_t)nf, sizeof *a->names);
    if (!a->names) { a->nfields = -1; return; }
    a->nnames = nf;
    a->nfields = nf;
    for (int i = 0; i < nf; i++)
        a->names[i] = strdup(fields[i].name ? fields[i].name : "");
}

static int bin_adapter_row(void *ctx, const HlMyVal *vals, int nc)
{
    MyBinAdapter *a = ctx;
    if (!a->user_cb) return 0;
    if (nc != a->nfields) { a->bad_row = 1; return 1; }   /* see adapter_row */

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
    s->dsn = strdup(dsn);
    if (!s->dsn || my_connect(&s->conn, dsn) != 0) {
        scrub_free(s->dsn);
        free(s);
        return -1;
    }
    *out_ctx = s;
    return 0;
}

static void mysql_close(HlDbHandle *h)
{
    if (!h || !h->ctx) return;
    HlDbMyCtx *s = h->ctx;
    hl_my_conn_close(&s->conn);
    scrub_free(s->dsn);
    free(s);
    h->ctx = NULL;
}

static int mysql_query_raw(HlDbHandle *h, const char *sql,
                           const HlValue *params, int nparams,
                           HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    (void)params; (void)alloc;
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;

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
        free_names(ba.names, ba.nnames);
        free(pp);
        if (rc == 0 && ba.bad_row) {
            snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                     "server sent a row whose column count does not match "
                     "its column definitions");
            rc = -1;
        }
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
    free_names(a.names, a.nnames);
    free(a.types);
    if (rc == 0 && a.bad_row) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "server sent a row whose column count does not match its "
                 "column definitions");
        rc = -1;
    }
    return rc;
}

static int mysql_exec_raw(HlDbHandle *h, const char *sql,
                          const HlValue *params, int nparams)
{
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;
    int64_t affected = 0;

    char *collated = NULL;
    if (nparams == 0) {
        int handled = 0;
        int shim = mysql_create_index_shim(s, sql, &handled);
        if (handled) return shim == 0 ? 0 : -1;   /* DDL: 0 rows affected */
        collated = mysql_bin_collate_hull_tables(sql);
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

    int qrc = hl_my_conn_query(&s->conn, collated ? collated : sql,
                               NULL, NULL, NULL, &affected);
    free(collated);
    if (qrc != 0) {
        if (s->conn.broken && sql_is_rollback(sql)) {   /* see mysql_txn */
            s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
            return 0;
        }
        return -1;
    }
    return (int)(affected < 0 ? 0 : affected);
}

/* A statement that failed inside a transaction with ER_LOCK_DEADLOCK took
 * the whole transaction with it (InnoDB rolls back the victim entirely, not
 * only the statement). The server status still read "in a transaction" from
 * the last OK, and nothing told the app: its next statements autocommitted
 * and COMMIT succeeded with the earlier writes gone. Remember it instead. */
/* 1 when @p sql starts with a statement MySQL commits the open transaction
 * before running - so before it fails, too (MySQL 8 "Statements That Cause
 * an Implicit Commit"): DDL, account management, LOCK / UNLOCK TABLES, the
 * table-maintenance and administration statements, replication control.
 * A failure of anything else that ends the transaction was a rollback, so
 * an unrecognised statement is refused (txn_aborted), never resumed: the
 * list errs on the short side. Not CREATE / DROP TEMPORARY TABLE nor LOAD
 * DATA / LOAD XML (audit 8 c_db L2): they commit nothing, and a lock-wait
 * timeout in one under innodb_rollback_on_timeout was resumed as DDL. Only
 * the first statement of a multi-statement text is read, so a later DDL
 * statement that failed is taken for a rollback: refused, not resumed. */
static int my_sql_commits_implicitly(const char *sql)
{
    static const char *const any[] = {   /* every form of the statement */
        "alter", "analyze", "cache", "check", "flush", "grant", "optimize",
        "rename", "repair", "revoke", "truncate",
    };
    if (my_sql_unreadable(sql)) return 0;   /* refused, never resumed */
    char w[16], w2[16], w3[16];
    const char *p = hl_sql_next_word(sql ? sql : "", w, sizeof w);
    p = hl_sql_next_word(p, w2, sizeof w2);
    for (size_t i = 0; i < sizeof any / sizeof any[0]; i++)
        if (strcmp(w, any[i]) == 0) return 1;
    if (strcmp(w, "create") == 0 || strcmp(w, "drop") == 0)
        return strcmp(w2, "temporary") != 0;
    if (strcmp(w, "lock") == 0 || strcmp(w, "unlock") == 0)
        return strcmp(w2, "tables") == 0 || strcmp(w2, "table") == 0;
    if (strcmp(w, "load") == 0)           /* LOAD INDEX INTO CACHE */
        return strcmp(w2, "index") == 0;
    if (strcmp(w, "install") == 0 || strcmp(w, "uninstall") == 0)
        return strcmp(w2, "plugin") == 0;
    if (strcmp(w, "reset") == 0)          /* all but RESET PERSIST */
        return w2[0] != '\0' && strcmp(w2, "persist") != 0;
    if (strcmp(w, "start") == 0 || strcmp(w, "stop") == 0)
        return strcmp(w2, "replica") == 0 || strcmp(w2, "slave") == 0;
    if (strcmp(w, "change") == 0) {       /* CHANGE MASTER / REPLICATION SOURCE TO */
        if (strcmp(w2, "master") == 0) return 1;
        (void)hl_sql_next_word(p, w3, sizeof w3);
        return strcmp(w2, "replication") == 0 && strcmp(w3, "source") == 0;
    }
    if (strcmp(w, "set") == 0)            /* SET PASSWORD (mysql.user) */
        return strcmp(w2, "password") == 0;
    return 0;
}

static void my_note_failure(HlDbMyCtx *s, const char *sql, int was_in_trans,
                            int rc)
{
    if (rc < 0 && was_in_trans && !s->conn.broken &&
        s->conn.last_err_code == HL_MY_ER_LOCK_DEADLOCK) {
        s->txn_aborted = 1;
        s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
        return;
    }
    /* Any other failure inside a transaction: an ERR carries no status, and a
     * failed DDL statement had already committed the transaction - ask
     * (audit 6 L1), so the implicit-commit resume and the batch see it. */
    if (rc < 0 && was_in_trans && !s->conn.broken && !s->txn_aborted) {
        if (hl_my_conn_ping(&s->conn) != 0 ||
            (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS))
            return;
        /* Gone, and not committed by a DDL statement: the server rolled the
         * whole transaction back under the failed statement (a lock-wait
         * timeout under innodb_rollback_on_timeout, ...). The resume would
         * have opened a new one and let the batch commit what followed
         * without what came before (audit 7 L1): refuse until a ROLLBACK, as
         * for a deadlock. A COMMIT / ROLLBACK / savepoint statement ends or
         * keeps the transaction on its own terms. */
        if (my_sql_unreadable(sql) ||   /* unrecognised: refused (audit 9 L1) */
            (hl_sql_txn_kind(sql) == HL_SQL_TXN_NONE &&
             !my_sql_commits_implicitly(sql)))
            s->txn_aborted = 1;
    }
}

static int my_in_trans(const HlDbHandle *h)
{
    const HlDbMyCtx *s = h ? h->ctx : NULL;
    return s && (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS);
}

static int mysql_txn_raw(HlDbHandle *h, const char *sql);

/* MySQL commits implicitly before and after DDL (CREATE / ALTER / DROP ...),
 * and before a DDL statement that then fails. Inside a db.batch that ended the
 * batch's transaction, and every later statement autocommitted - then the
 * batch reported its transaction lost (hl_db_batch_lost_), failing the
 * stdlib's own idempotent schema batches. When a statement that is not itself
 * COMMIT / ROLLBACK ends the transaction a batch holds, open a new one: what
 * ran before the DDL is committed (MySQL semantics, which no client can
 * change), and the rest of the batch is transactional again. A raw COMMIT /
 * ROLLBACK is still reported as lost.
 *
 * Not inside a NESTED batch (audit 6 L3): the commit also dropped the outer
 * batches' savepoints, so the inner batch could no longer roll back on its
 * own - its writes after the DDL committed with the outer batch even when it
 * failed. The transaction stays ended instead, and every enclosing batch's
 * leave reports it lost. */
static void my_resume_after_implicit_commit(HlDbHandle *h, const char *sql,
                                            int was)
{
    if (!was || !h || h->batch_depth != 1 || my_in_trans(h)) return;
    HlDbMyCtx *s = h->ctx;
    if (!s || s->txn_aborted || s->conn.broken) return;
    HlSqlTxnKind k = my_sql_txn_kind(sql);
    if (k == HL_SQL_TXN_COMMIT || k == HL_SQL_TXN_ROLLBACK || k == HL_SQL_TXN_OTHER)
        return;
    (void)mysql_txn_raw(h, "START TRANSACTION");
}

static int mysql_query(HlDbHandle *h, const char *sql,
                       const HlValue *params, int nparams,
                       HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    int was = my_in_trans(h);
    int rc = mysql_query_raw(h, sql, params, nparams, cb, cb_ctx, alloc);
    if (h && h->ctx) my_note_failure(h->ctx, sql, was, rc);
    my_resume_after_implicit_commit(h, sql, was);
    return rc;
}

static int mysql_exec(HlDbHandle *h, const char *sql,
                      const HlValue *params, int nparams)
{
    int was = my_in_trans(h);
    int rc = mysql_exec_raw(h, sql, params, nparams);
    if (h && h->ctx) my_note_failure(h->ctx, sql, was, rc);
    my_resume_after_implicit_commit(h, sql, was);
    return rc;
}

static int mysql_exec_script(HlDbHandle *h, const char *sql)
{
    /* Migrations are multi-statement; the connection advertises
     * CLIENT_MULTI_STATEMENTS so the whole script runs as one COM_QUERY. */
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;
    char *collated = mysql_bin_collate_hull_tables(sql);
    int rc = hl_my_conn_exec_multi(&s->conn, collated ? collated : sql);
    free(collated);
    return rc;
}

static int mysql_txn_raw(HlDbHandle *h, const char *sql)
{
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;
    int rc = hl_my_conn_query(&s->conn, sql, NULL, NULL, NULL, NULL);
    /* A ROLLBACK that failed because the connection went: the server rolled
     * the transaction back with it. */
    if (rc != 0 && s->conn.broken && sql_is_rollback(sql)) {
        s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
        rc = 0;
    }
    return rc;
}

static int mysql_txn(HlDbHandle *h, const char *sql)
{
    int was = my_in_trans(h);
    int rc = mysql_txn_raw(h, sql);
    if (h && h->ctx) my_note_failure(h->ctx, sql, was, rc);
    return rc;
}

static int mysql_begin(HlDbHandle *h)    { return mysql_txn(h, "START TRANSACTION"); }
static int mysql_commit(HlDbHandle *h)   { return mysql_txn(h, "COMMIT"); }
static int mysql_rollback(HlDbHandle *h) { return mysql_txn(h, "ROLLBACK"); }

/* See pg_guard_stale_txn: a transaction a request left open is rolled back
 * before the next request runs. */
static int mysql_in_txn(HlDbHandle *h)
{
    if (!h || !h->ctx) return 0;
    HlDbMyCtx *s = h->ctx;
    return (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS) ||
           s->txn_aborted;
}

static void mysql_guard_stale_txn(HlDbHandle *h)
{
    if (!mysql_in_txn(h)) return;
    fprintf(stderr, "[hull:c] rolling back stale transaction from previous request\n");
    (void)mysql_rollback(h);
}

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
    .dialect = {
        .identifier_quote             = '`',
        .placeholder                  = "?",
        .upsert_style                 = "on_duplicate_key",
        .supports_returning           = 0,   /* MySQL 8; MariaDB is 1 (see design doc) */
        .supports_index_if_not_exists = 0,   /* rewritten by the backend shim */
        .supports_skip_locked         = 1,   /* MySQL 8+ / MariaDB 10.6+ */
        .identity_column              = "BIGINT AUTO_INCREMENT PRIMARY KEY",
        .identity_sequence            = NULL,
    },
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
    .guard_stale_txn       = mysql_guard_stale_txn,
    .in_txn                = mysql_in_txn,
    .last_id               = mysql_last_id,
    .errmsg                = mysql_errmsg,
    .insert_if_absent      = mysql_insert_if_absent,
    .upsert                = mysql_upsert,
    .table_columns         = mysql_table_columns,
    /* native_handle: no consumer (udf / agent introspection are SQLite-only). */
};

#endif /* HL_ENABLE_MYSQL */
