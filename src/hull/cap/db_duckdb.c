/*
 * cap/db_duckdb.c: DuckDB HlDbBackend vtable
 *
 * DuckDB as a statically-linked embedded OLAP backend behind the generic
 * HlDbBackend surface. Parameters bind through DuckDB prepared statements
 * (injection-safe; values never touch the SQL text), and result values decode
 * from DuckDB's columnar data chunks into HlValue via the stable vector API.
 *
 * This is the thin vertical slice (open/query/exec/txn against :memory: or a
 * file, a core-lockdown security config, and the dialect row). Deferred to
 * follow-ups: the manifest-driven fs.read/fs.write -> allowed_directories mode,
 * the insert_if_absent/upsert/table_columns dialect helpers, temporal / decimal
 * / nested type decoding, and the signed side-load packaging. db.udf and
 * hull/search stay SQLite-only. See docs/duckdb_backend_design.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DUCKDB

#include "hull/cap/db_backend.h"
#include "hull/cap/db_duckdb.h"
#include "hull/cap/types.h"
#include "hull/utils/alloc.h"

/* duckdb.h (vendored) has a few prototype-less C-API declarations that trip
 * Hull's strict -Wstrict-prototypes; keep them from polluting our build. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <duckdb.h>
#pragma GCC diagnostic pop

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VARCHAR/BLOB inline threshold: a duckdb_string_t of length <= 12 stores its
 * bytes inline; longer strings live behind value.pointer.ptr. See duckdb.h. */
#define HL_DUCKDB_STRING_INLINE_MAX 12

typedef struct HlDbDuckCtx {
    duckdb_database    db;
    duckdb_connection  con;
    HlAllocator       *alloc;
    char               errmsg[512];
} HlDbDuckCtx;

static void duck_set_err(HlDbDuckCtx *s, const char *msg)
{
    if (!s) return;
    snprintf(s->errmsg, sizeof s->errmsg, "%s", msg ? msg : "duckdb error");
}

/* ── Security lockdown (core, manifest-independent) ────────────────────
 *
 * DuckDB SQL can drive its own file/network I/O (read_csv, read_parquet,
 * COPY, httpfs), which would bypass Hull's capability model. Network is
 * already structurally absent (httpfs / S3 are not in the linked archive
 * set). Here we additionally forbid installing / loading extensions and
 * disable all external (local file) access, then LOCK the configuration so
 * app SQL cannot re-enable any of it. This is the SQLite-equivalent full
 * lockdown; the bounded-local-access mode (fs.read/fs.write ->
 * allowed_directories) is a follow-up that needs the manifest at open time.
 *
 * Extension-install / access options are set at config time (before open);
 * lock_configuration is applied last, after connect, so the config-time sets
 * are not themselves rejected by the lock. */
static int duck_apply_config_security(duckdb_config config)
{
    /* Each returns DuckDBSuccess/Error; a rejected option name is fatal (the
     * pinned DuckDB version is known to accept these). */
    if (duckdb_set_config(config, "autoinstall_known_extensions", "false") != DuckDBSuccess)
        return -1;
    if (duckdb_set_config(config, "autoload_known_extensions", "false") != DuckDBSuccess)
        return -1;
    if (duckdb_set_config(config, "allow_unsigned_extensions", "false") != DuckDBSuccess)
        return -1;
    /* No fs.read/fs.write plumbing in this slice -> full lockdown: DB file +
     * :memory: only, no SQL-driven external file access. */
    if (duckdb_set_config(config, "enable_external_access", "false") != DuckDBSuccess)
        return -1;
    return 0;
}

/* ── Open / close ─────────────────────────────────────────────────── */

static int duck_open(void **out_ctx, const char *dsn, HlAllocator *alloc)
{
    HlDbDuckCtx *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->alloc = alloc;

    /* Strip an explicit "duckdb://" scheme: everything after it is the path or
     * ":memory:". A bare path and ":memory:" pass through unchanged. e.g.
     * "duckdb:///var/db.duckdb" -> "/var/db.duckdb", "duckdb://:memory:" ->
     * ":memory:". A NULL/empty path opens a private in-memory database. */
    const char *path = dsn ? dsn : ":memory:";
    if (strncmp(path, "duckdb://", 9) == 0)
        path += 9;
    if (path[0] == '\0')
        path = ":memory:";

    duckdb_config config;
    if (duckdb_create_config(&config) != DuckDBSuccess) {
        duck_set_err(s, "duckdb_create_config failed");
        free(s);
        return -1;
    }
    if (duck_apply_config_security(config) != 0) {
        duckdb_destroy_config(&config);
        duck_set_err(s, "duckdb security config rejected");
        free(s);
        return -1;
    }

    char *open_err = NULL;
    duckdb_state rc = duckdb_open_ext(path, &s->db, config, &open_err);
    duckdb_destroy_config(&config);
    if (rc != DuckDBSuccess) {
        duck_set_err(s, open_err ? open_err : "duckdb_open_ext failed");
        if (open_err) duckdb_free(open_err);
        free(s);
        return -1;
    }

    if (duckdb_connect(s->db, &s->con) != DuckDBSuccess) {
        duck_set_err(s, "duckdb_connect failed");
        duckdb_close(&s->db);
        free(s);
        return -1;
    }

    /* Keystone: freeze the configuration so a handler's SQL can no longer
     * re-enable external access or extension loading for this connection's
     * life. Run as a plain statement (no params). */
    duckdb_result r;
    if (duckdb_query(s->con, "SET lock_configuration = true;", &r) != DuckDBSuccess) {
        duck_set_err(s, "duckdb lock_configuration failed");
        duckdb_destroy_result(&r);
        duckdb_disconnect(&s->con);
        duckdb_close(&s->db);
        free(s);
        return -1;
    }
    duckdb_destroy_result(&r);

    *out_ctx = s;
    return 0;
}

static void duck_close(HlDbHandle *h)
{
    if (!h || !h->ctx) return;
    HlDbDuckCtx *s = h->ctx;
    duckdb_disconnect(&s->con);
    duckdb_close(&s->db);
    free(s);
    h->ctx = NULL;
}

/* ── Parameter binding (HlValue -> prepared-statement bind) ────────── */

static int duck_bind_params(duckdb_prepared_statement stmt,
                            const HlValue *params, int nparams)
{
    /* DuckDB parameter indices are 1-based. */
    for (int i = 0; i < nparams; i++) {
        idx_t idx = (idx_t)(i + 1);
        const HlValue *v = &params[i];
        duckdb_state rc;
        switch (v->type) {
        case HL_TYPE_NIL:
            rc = duckdb_bind_null(stmt, idx);
            break;
        case HL_TYPE_INT:
            rc = duckdb_bind_int64(stmt, idx, v->i);
            break;
        case HL_TYPE_DOUBLE:
            rc = duckdb_bind_double(stmt, idx, v->d);
            break;
        case HL_TYPE_BOOL:
            rc = duckdb_bind_boolean(stmt, idx, v->b ? true : false);
            break;
        case HL_TYPE_TEXT:
            rc = duckdb_bind_varchar_length(stmt, idx, v->s ? v->s : "",
                                            (idx_t)(v->s ? v->len : 0));
            break;
        case HL_TYPE_BLOB:
            rc = duckdb_bind_blob(stmt, idx, v->s ? (const void *)v->s : "",
                                  (idx_t)(v->s ? v->len : 0));
            break;
        default:
            rc = duckdb_bind_null(stmt, idx);
            break;
        }
        if (rc != DuckDBSuccess) return -1;
    }
    return 0;
}

/* ── Result decoding (data chunk vector -> HlValue) ───────────────────
 *
 * Decodes one row of one column from a chunk vector. `data` is the vector's
 * typed base pointer; `row` is the row index within the chunk. For a TEXT/BLOB
 * value, *out borrows bytes owned by the chunk (valid for the row callback's
 * duration, since the chunk outlives the callback). Unsupported column types
 * (temporal, decimal, nested) decode to NIL in this slice. */
static void duck_decode(duckdb_type type, void *data, idx_t row, HlValue *out)
{
    switch (type) {
    case DUCKDB_TYPE_BOOLEAN:
        out->type = HL_TYPE_BOOL;
        out->b = ((bool *)data)[row] ? 1 : 0;
        return;
    case DUCKDB_TYPE_TINYINT:
        out->type = HL_TYPE_INT; out->i = ((int8_t *)data)[row]; return;
    case DUCKDB_TYPE_SMALLINT:
        out->type = HL_TYPE_INT; out->i = ((int16_t *)data)[row]; return;
    case DUCKDB_TYPE_INTEGER:
        out->type = HL_TYPE_INT; out->i = ((int32_t *)data)[row]; return;
    case DUCKDB_TYPE_BIGINT:
        out->type = HL_TYPE_INT; out->i = ((int64_t *)data)[row]; return;
    case DUCKDB_TYPE_UTINYINT:
        out->type = HL_TYPE_INT; out->i = ((uint8_t *)data)[row]; return;
    case DUCKDB_TYPE_USMALLINT:
        out->type = HL_TYPE_INT; out->i = ((uint16_t *)data)[row]; return;
    case DUCKDB_TYPE_UINTEGER:
        out->type = HL_TYPE_INT; out->i = ((uint32_t *)data)[row]; return;
    case DUCKDB_TYPE_UBIGINT:
        /* May exceed int64 range; wraps into the signed slot (documented). */
        out->type = HL_TYPE_INT; out->i = (int64_t)((uint64_t *)data)[row]; return;
    case DUCKDB_TYPE_HUGEINT:
        out->type = HL_TYPE_DOUBLE;
        out->d = duckdb_hugeint_to_double(((duckdb_hugeint *)data)[row]);
        return;
    case DUCKDB_TYPE_FLOAT:
        out->type = HL_TYPE_DOUBLE; out->d = ((float *)data)[row]; return;
    case DUCKDB_TYPE_DOUBLE:
        out->type = HL_TYPE_DOUBLE; out->d = ((double *)data)[row]; return;
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_BLOB: {
        duckdb_string_t s = ((duckdb_string_t *)data)[row];
        uint32_t len = s.value.inlined.length;
        const char *p = (len <= HL_DUCKDB_STRING_INLINE_MAX)
            ? s.value.inlined.inlined : s.value.pointer.ptr;
        out->type = (type == DUCKDB_TYPE_BLOB) ? HL_TYPE_BLOB : HL_TYPE_TEXT;
        out->s = p;
        out->len = len;
        return;
    }
    default:
        /* Temporal / decimal / nested types land here in the slice. */
        out->type = HL_TYPE_NIL;
        return;
    }
}

/* ── Query / exec ─────────────────────────────────────────────────── */

static int duck_query(HlDbHandle *h, const char *sql,
                      const HlValue *params, int nparams,
                      HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    (void)alloc;
    if (!h || !h->ctx) return -1;
    HlDbDuckCtx *s = h->ctx;

    duckdb_prepared_statement stmt;
    if (duckdb_prepare(s->con, sql, &stmt) != DuckDBSuccess) {
        duck_set_err(s, duckdb_prepare_error(stmt));
        duckdb_destroy_prepare(&stmt);
        return -1;
    }
    if (duck_bind_params(stmt, params, nparams) != 0) {
        duck_set_err(s, "duckdb parameter bind failed");
        duckdb_destroy_prepare(&stmt);
        return -1;
    }

    duckdb_result result;
    if (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess) {
        duck_set_err(s, duckdb_result_error(&result));
        duckdb_destroy_result(&result);
        duckdb_destroy_prepare(&stmt);
        return -1;
    }

    int rc = 0;
    if (cb) {
        idx_t ncols = duckdb_column_count(&result);
        /* Column names + types survive across chunks (queried from the
         * result, not the chunk). */
        idx_t nchunks = duckdb_result_chunk_count(result);
        for (idx_t ci = 0; ci < nchunks && rc == 0; ci++) {
            duckdb_data_chunk chunk = duckdb_result_get_chunk(result, ci);
            if (!chunk) continue;
            idx_t nrows = duckdb_data_chunk_get_size(chunk);

            for (idx_t r = 0; r < nrows && rc == 0; r++) {
                HlColumn *cols = calloc(ncols ? ncols : 1, sizeof *cols);
                if (!cols) { rc = -1; duckdb_destroy_data_chunk(&chunk); break; }

                for (idx_t c = 0; c < ncols; c++) {
                    cols[c].name = duckdb_column_name(&result, c);
                    duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, c);
                    void     *vdata = duckdb_vector_get_data(vec);
                    uint64_t *valid = duckdb_vector_get_validity(vec);
                    if (valid && !duckdb_validity_row_is_valid(valid, r)) {
                        cols[c].value.type = HL_TYPE_NIL;
                    } else {
                        duck_decode(duckdb_column_type(&result, c),
                                    vdata, r, &cols[c].value);
                    }
                }

                if (cb(cb_ctx, cols, (int)ncols) != 0)
                    rc = 1;   /* caller asked to stop */
                free(cols);
            }
            duckdb_destroy_data_chunk(&chunk);
        }
    }

    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&stmt);
    return rc < 0 ? -1 : 0;
}

static int duck_exec(HlDbHandle *h, const char *sql,
                     const HlValue *params, int nparams)
{
    return duck_query(h, sql, params, nparams, NULL, NULL, NULL);
}

/* Multi-statement script, no params. duckdb_query runs the whole string
 * (semicolon-separated statements) and reports the first error. Used by the
 * migration runner. */
static int duck_exec_script(HlDbHandle *h, const char *sql)
{
    if (!h || !h->ctx) return -1;
    HlDbDuckCtx *s = h->ctx;
    duckdb_result r;
    if (duckdb_query(s->con, sql, &r) != DuckDBSuccess) {
        duck_set_err(s, duckdb_result_error(&r));
        duckdb_destroy_result(&r);
        return -1;
    }
    duckdb_destroy_result(&r);
    return 0;
}

static int duck_begin(HlDbHandle *h)    { return duck_exec(h, "BEGIN TRANSACTION", NULL, 0); }
static int duck_commit(HlDbHandle *h)   { return duck_exec(h, "COMMIT", NULL, 0); }
static int duck_rollback(HlDbHandle *h) { return duck_exec(h, "ROLLBACK", NULL, 0); }

static int64_t duck_last_id(HlDbHandle *h)
{
    (void)h;
    /* DuckDB has no last-insert-rowid; identity comes from sequences. Callers
     * use RETURNING (supports_returning = 1). */
    return -1;
}

static const char *duck_errmsg(HlDbHandle *h)
{
    if (!h || !h->ctx) return "no connection";
    HlDbDuckCtx *s = h->ctx;
    return s->errmsg[0] ? s->errmsg : "";
}

/* ── Vtable (const, lands in .rodata) ─────────────────────────────── */

static const char *const duck_schemes[] = { "duckdb", NULL };

const HlDbBackend hl_db_backend_duckdb = {
    .name                 = "duckdb",
    .schemes              = duck_schemes,
    .dialect = {
        .identifier_quote             = '"',
        .placeholder                  = "?",
        .upsert_style                 = "on_conflict",
        .supports_returning           = 1,
        .supports_index_if_not_exists = 1,
        .identity_column              = "BIGINT DEFAULT nextval('%s') PRIMARY KEY",
        .identity_sequence            = "CREATE SEQUENCE %s",
    },
    .native_tag           = HL_DB_NATIVE_DUCKDB,
    .supports_udf         = 0,   /* db.udf stays SQLite-only */
    .open                 = duck_open,
    .close                = duck_close,
    .query                = duck_query,
    .exec                 = duck_exec,
    .exec_script          = duck_exec_script,
    .begin                = duck_begin,
    .commit               = duck_commit,
    .rollback             = duck_rollback,
    .last_id              = duck_last_id,
    .errmsg               = duck_errmsg,
    .guard_stale_txn      = NULL,
    /* Dialect helpers (insert_if_absent / upsert / table_columns) and a raw
     * native_handle are deferred to the next slice; NULL here. */
    .insert_if_absent     = NULL,
    .upsert               = NULL,
    .table_columns        = NULL,
    .native_handle        = NULL,
};

#endif /* HL_ENABLE_DUCKDB */
