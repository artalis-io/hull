/*
 * cap/db_postgres.c: PostgreSQL HlDbBackend vtable
 *
 * Phase 2.4: maps the generic HlDbBackend surface onto the pgwire codec +
 * connection (cap/pgwire.c, cap/pg_conn.c). Parameters are bound in text
 * format out-of-band (never spliced into SQL), and result values are decoded
 * from text into HlValue by type OID. db.udf and hull/search stay SQLite-only
 * (see docs/postgres_backend_design.md); last_id is unsupported on Postgres
 * (use RETURNING).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_POSTGRES

#include "hull/cap/db_backend.h"
#include "hull/cap/pg_conn.h"
#include "hull/cap/types.h"
#include "hull/utils/alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HlDbPgCtx {
    HlPgConn     conn;
    HlAllocator *alloc;
} HlDbPgCtx;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ── Open / close ─────────────────────────────────────────────────── */

static int pg_open(void **out_ctx, const char *dsn, HlAllocator *alloc)
{
    HlDbPgCtx *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->alloc = alloc;

    HlPgDsn parsed;
    char err[128];
    if (hl_pg_dsn_parse(dsn, &parsed, err, sizeof err) != 0) {
        free(s);
        return -1;
    }
    int rc = hl_pg_conn_open(&s->conn, &parsed, 10000 /* 10s connect */);
    hl_pg_dsn_scrub(&parsed);   /* password is secret material */
    if (rc != 0) {
        free(s);
        return -1;
    }
    *out_ctx = s;
    return 0;
}

static void pg_close(HlDbHandle *h)
{
    if (!h || !h->ctx) return;
    HlDbPgCtx *s = h->ctx;
    hl_pg_conn_close(&s->conn);
    free(s);
    h->ctx = NULL;
}

/* ── Parameter encoding (HlValue -> text) ─────────────────────────── */

/* Builds an HlPgParam array plus a parallel scratch array of heap strings
 * for numeric / bytea renderings (NULL entries for values that borrow). The
 * caller frees each scratch[i], scratch, and the returned array. Returns
 * NULL on allocation failure (only when n > 0). */
static HlPgParam *encode_params(const HlValue *params, int n, char ***scratch_out)
{
    *scratch_out = NULL;
    if (n <= 0) return NULL;

    HlPgParam *pp = calloc((size_t)n, sizeof *pp);
    char **scratch = calloc((size_t)n, sizeof *scratch);
    if (!pp || !scratch) { free(pp); free(scratch); return NULL; }

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        const HlValue *v = &params[i];
        switch (v->type) {
        case HL_TYPE_NIL:
            pp[i].text = NULL; pp[i].len = -1;
            break;
        case HL_TYPE_INT: {
            char *b = malloc(32);
            if (!b) goto oom;
            int l = snprintf(b, 32, "%lld", (long long)v->i);
            scratch[i] = b; pp[i].text = b; pp[i].len = l;
            break;
        }
        case HL_TYPE_DOUBLE: {
            char *b = malloc(32);
            if (!b) goto oom;
            int l = snprintf(b, 32, "%.17g", v->d);
            scratch[i] = b; pp[i].text = b; pp[i].len = l;
            break;
        }
        case HL_TYPE_BOOL:
            pp[i].text = v->b ? "t" : "f"; pp[i].len = 1;
            break;
        case HL_TYPE_TEXT:
            pp[i].text = v->s ? v->s : "";
            pp[i].len = v->s ? (int32_t)v->len : 0;
            break;
        case HL_TYPE_BLOB: {
            size_t bl = v->len;
            if (bl > (SIZE_MAX - 4) / 2) goto oom;   /* overflow guard */
            size_t hl = 2 + bl * 2;
            char *b = malloc(hl + 1);
            if (!b) goto oom;
            b[0] = '\\'; b[1] = 'x';
            const unsigned char *src = (const unsigned char *)v->s;
            for (size_t j = 0; j < bl; j++) {
                b[2 + j * 2]     = hex[src[j] >> 4];
                b[2 + j * 2 + 1] = hex[src[j] & 0xf];
            }
            b[hl] = '\0';
            scratch[i] = b; pp[i].text = b; pp[i].len = (int32_t)hl;
            break;
        }
        default:
            pp[i].text = NULL; pp[i].len = -1;
            break;
        }
    }
    *scratch_out = scratch;
    return pp;

oom:
    for (int i = 0; i < n; i++) free(scratch[i]);
    free(scratch);
    free(pp);
    return NULL;
}

/* ── Result decoding (text -> HlValue) ────────────────────────────── */

/* Decode one text-format value by type OID. For BLOB, *freeme receives a
 * heap buffer the caller frees after the row callback; otherwise NULL. TEXT
 * values borrow the wire bytes (valid for the callback's duration). */
static void decode_value(int32_t oid, const char *text, int32_t len,
                         HlValue *out, char **freeme)
{
    *freeme = NULL;
    if (!text) { out->type = HL_TYPE_NIL; return; }

    char numbuf[64];
    switch (oid) {
    case 16:  /* bool */
        out->type = HL_TYPE_BOOL;
        out->b = (len >= 1 && (text[0] == 't' || text[0] == 'T' || text[0] == '1'));
        return;
    case 20: case 21: case 23:  /* int8 / int2 / int4 */
        if (len >= 0 && len < (int32_t)sizeof numbuf) {
            memcpy(numbuf, text, (size_t)len);
            numbuf[len] = '\0';
            out->type = HL_TYPE_INT;
            out->i = (int64_t)strtoll(numbuf, NULL, 10);
            return;
        }
        break;
    case 700: case 701:  /* float4 / float8 */
        if (len >= 0 && len < (int32_t)sizeof numbuf) {
            memcpy(numbuf, text, (size_t)len);
            numbuf[len] = '\0';
            out->type = HL_TYPE_DOUBLE;
            out->d = strtod(numbuf, NULL);
            return;
        }
        break;
    case 17:  /* bytea, text format "\x...." */
        if (len >= 2 && text[0] == '\\' && text[1] == 'x') {
            size_t blen = ((size_t)len - 2) / 2;
            char *buf = malloc(blen ? blen : 1);
            if (buf) {
                int ok = 1;
                for (size_t j = 0; j < blen; j++) {
                    int hi = hexval(text[2 + j * 2]);
                    int lo = hexval(text[2 + j * 2 + 1]);
                    if (hi < 0 || lo < 0) { ok = 0; break; }
                    buf[j] = (char)((hi << 4) | lo);
                }
                if (ok) {
                    out->type = HL_TYPE_BLOB;
                    out->s = buf; out->len = blen;
                    *freeme = buf;
                    return;
                }
                free(buf);
            }
        }
        break;
    default:
        break;
    }
    /* Fallback: text, borrowing the wire bytes. */
    out->type = HL_TYPE_TEXT;
    out->s = text;
    out->len = (size_t)(len < 0 ? 0 : len);
}

/* ── Query / exec ─────────────────────────────────────────────────── */

typedef struct {
    HlRowCallback user_cb;
    void         *user_ctx;
    char        **names;   /* copied field names (survive past RowDescription) */
    int32_t      *oids;
    int           nfields;
} PgAdapter;

static void adapter_desc(void *ctx, const HlPgField *fields, int nf)
{
    PgAdapter *a = ctx;
    a->nfields = nf;
    if (nf <= 0) return;
    a->names = calloc((size_t)nf, sizeof *a->names);
    a->oids  = calloc((size_t)nf, sizeof *a->oids);
    if (!a->names || !a->oids) { a->nfields = 0; return; }
    for (int i = 0; i < nf; i++) {
        const char *nm = fields[i].name ? fields[i].name : "";
        a->names[i] = strdup(nm);
        a->oids[i]  = fields[i].oid;
    }
}

static int adapter_row(void *ctx, const char *const *vals,
                       const int32_t *lens, int nc)
{
    PgAdapter *a = ctx;
    if (!a->user_cb) return 0;
    if (a->nfields > 0 && nc > a->nfields) nc = a->nfields;

    HlColumn *cols = calloc((size_t)(nc > 0 ? nc : 1), sizeof *cols);
    char **frees = calloc((size_t)(nc > 0 ? nc : 1), sizeof *frees);
    if (!cols || !frees) { free(cols); free(frees); return 1; }

    for (int i = 0; i < nc; i++) {
        cols[i].name = (a->names && a->names[i]) ? a->names[i] : "";
        int32_t oid = a->oids ? a->oids[i] : 0;
        decode_value(oid, vals[i], vals[i] ? lens[i] : -1,
                     &cols[i].value, &frees[i]);
    }
    int rc = a->user_cb(a->user_ctx, cols, nc);
    for (int i = 0; i < nc; i++) free(frees[i]);
    free(cols);
    free(frees);
    return rc ? 1 : 0;
}

static int pg_query(HlDbHandle *h, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    (void)alloc;
    if (!h || !h->ctx) return -1;
    HlDbPgCtx *s = h->ctx;

    char **scratch = NULL;
    HlPgParam *pp = encode_params(params, nparams, &scratch);
    if (nparams > 0 && !pp) return -1;

    PgAdapter a;
    memset(&a, 0, sizeof a);
    a.user_cb = cb;
    a.user_ctx = cb_ctx;

    int rc = hl_pg_query(&s->conn, sql, pp, nparams,
                         cb ? adapter_desc : NULL,
                         cb ? adapter_row : NULL, &a, NULL);

    if (a.names)
        for (int i = 0; i < a.nfields; i++) free(a.names[i]);
    free(a.names);
    free(a.oids);
    if (scratch)
        for (int i = 0; i < nparams; i++) free(scratch[i]);
    free(scratch);
    free(pp);
    return rc;
}

static int pg_exec(HlDbHandle *h, const char *sql,
                   const HlValue *params, int nparams)
{
    return pg_query(h, sql, params, nparams, NULL, NULL, NULL);
}

static int pg_begin(HlDbHandle *h)    { return pg_exec(h, "BEGIN", NULL, 0); }
static int pg_commit(HlDbHandle *h)   { return pg_exec(h, "COMMIT", NULL, 0); }
static int pg_rollback(HlDbHandle *h) { return pg_exec(h, "ROLLBACK", NULL, 0); }

static int64_t pg_last_id(HlDbHandle *h)
{
    (void)h;
    /* PostgreSQL has no last-insert-rowid; callers use RETURNING. */
    return -1;
}

static const char *pg_errmsg(HlDbHandle *h)
{
    if (!h || !h->ctx) return "no connection";
    HlDbPgCtx *s = h->ctx;
    return s->conn.errmsg[0] ? s->conn.errmsg : "";
}

/* ── Dialect helpers ──────────────────────────────────────────────── */

/* Append to a bounded buffer; latch failure via *off > cap. */
static void ap(char *buf, size_t cap, size_t *off, const char *str)
{
    size_t n = strlen(str);
    if (*off + n < cap) memcpy(buf + *off, str, n);
    *off += n;
}

static int build_and_run_insert(HlDbHandle *h, const char *verb_suffix,
                                const char *table,
                                const char *const *conflict_cols, int n_conflict,
                                const char *const *cols,
                                const HlValue *values, int n_cols,
                                int do_update)
{
    (void)verb_suffix;
    /* Estimate a generous size and build INSERT ... ON CONFLICT ... */
    size_t cap = 64 + strlen(table);
    for (int i = 0; i < n_cols; i++) cap += strlen(cols[i]) + 24;
    for (int i = 0; i < n_conflict; i++) cap += strlen(conflict_cols[i]) + 4;
    char *sql = malloc(cap);
    if (!sql) return -1;
    size_t off = 0;

    ap(sql, cap, &off, "INSERT INTO ");
    ap(sql, cap, &off, table);
    ap(sql, cap, &off, " (");
    for (int i = 0; i < n_cols; i++) {
        if (i) ap(sql, cap, &off, ", ");
        ap(sql, cap, &off, cols[i]);
    }
    ap(sql, cap, &off, ") VALUES (");
    for (int i = 0; i < n_cols; i++) {
        char ph[16];
        snprintf(ph, sizeof ph, "%s$%d", i ? ", " : "", i + 1);
        ap(sql, cap, &off, ph);
    }
    ap(sql, cap, &off, ") ON CONFLICT (");
    for (int i = 0; i < n_conflict; i++) {
        if (i) ap(sql, cap, &off, ", ");
        ap(sql, cap, &off, conflict_cols[i]);
    }
    ap(sql, cap, &off, ") DO ");
    if (do_update) {
        ap(sql, cap, &off, "UPDATE SET ");
        for (int i = 0; i < n_cols; i++) {
            if (i) ap(sql, cap, &off, ", ");
            ap(sql, cap, &off, cols[i]);
            ap(sql, cap, &off, " = excluded.");
            ap(sql, cap, &off, cols[i]);
        }
    } else {
        ap(sql, cap, &off, "NOTHING");
    }

    if (off >= cap) { free(sql); return -1; }   /* estimate was too small */
    sql[off] = '\0';

    int rc = pg_exec(h, sql, values, n_cols);
    free(sql);
    return rc;
}

static int pg_insert_if_absent(HlDbHandle *h, const char *table,
                               const char *const *conflict_cols, int n_conflict,
                               const char *const *cols,
                               const HlValue *values, int n_cols)
{
    return build_and_run_insert(h, "", table, conflict_cols, n_conflict,
                                cols, values, n_cols, 0);
}

static int pg_upsert(HlDbHandle *h, const char *table,
                     const char *const *conflict_cols, int n_conflict,
                     const char *const *cols,
                     const HlValue *values, int n_cols)
{
    return build_and_run_insert(h, "", table, conflict_cols, n_conflict,
                                cols, values, n_cols, 1);
}

typedef struct {
    HlDbColumnCallback cb;
    void              *cb_ctx;
} PgColsFwd;

static int pg_table_columns_row(void *ctx, HlColumn *cols, int ncols)
{
    PgColsFwd *fwd = ctx;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].value.type != HL_TYPE_TEXT || !cols[i].value.s)
            continue;
        /* The text value borrows non-NUL-terminated wire bytes (the row
         * decoder tracks length separately). HlDbColumnCallback takes a C
         * string, so copy + terminate using the tracked length; otherwise
         * the callback over-reads past the field into the next message. */
        size_t len = cols[i].value.len;
        char stackbuf[128];
        char *name = (len < sizeof stackbuf) ? stackbuf : malloc(len + 1);
        if (!name)
            continue;
        memcpy(name, cols[i].value.s, len);
        name[len] = '\0';
        fwd->cb(fwd->cb_ctx, name);
        if (name != stackbuf)
            free(name);
    }
    return 0;
}

static int pg_table_columns(HlDbHandle *h, const char *table,
                            HlDbColumnCallback cb, void *cb_ctx)
{
    PgColsFwd fwd = { cb, cb_ctx };
    HlValue p;
    p.type = HL_TYPE_TEXT;
    p.s = table;
    p.len = strlen(table);
    return pg_query(h,
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_name = ? ORDER BY ordinal_position",
        &p, 1, pg_table_columns_row, &fwd, NULL);
}

/* ── Vtable (const, lands in .rodata) ─────────────────────────────── */

const HlDbBackend hl_db_backend_postgres = {
    .name                 = "postgres",
    .autoincrement_id_ddl = "BIGSERIAL PRIMARY KEY",
    .open                 = pg_open,
    .close                = pg_close,
    .query                = pg_query,
    .exec                 = pg_exec,
    .begin                = pg_begin,
    .commit               = pg_commit,
    .rollback             = pg_rollback,
    .last_id              = pg_last_id,
    .errmsg               = pg_errmsg,
    .guard_stale_txn      = NULL,
    .insert_if_absent     = pg_insert_if_absent,
    .upsert               = pg_upsert,
    .table_columns        = pg_table_columns,
};

#endif /* HL_ENABLE_POSTGRES */
