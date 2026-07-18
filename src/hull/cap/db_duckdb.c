/*
 * cap/db_duckdb.c: DuckDB HlDbBackend vtable
 *
 * DuckDB as a statically-linked embedded OLAP backend behind the generic
 * HlDbBackend surface. Parameters bind through DuckDB prepared statements
 * (injection-safe; values never touch the SQL text), and result values decode
 * from DuckDB's columnar data chunks into HlValue via the stable vector API.
 *
 * Covers open/query/exec/txn against :memory: or a file, the dialect row, the
 * insert_if_absent/upsert/table_columns dialect helpers, and two security modes:
 * full lockdown (mode A, no external file access) and the manifest-driven
 * fs.read/fs.write -> allowed_directories mode (mode B). Mode B needs the
 * vendor/pledge rseq fix: a named connection opens post-sandbox and spawns
 * DuckDB's worker pool, whose threads must register rseq (see
 * docs/duckdb_backend_design.md §3.2). Deferred to follow-ups: temporal /
 * decimal / nested type decoding, and the signed side-load packaging. db.udf and
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

/* ── Manifest-driven file-access policy ────────────────────────────────
 *
 * The bounded-local-access mode (design §3.2 mode B): when the app declares
 * fs.read / fs.write paths, DuckDB SQL (read_csv / read_parquet / COPY) may
 * touch exactly those directories, nothing else. serve.c resolves the manifest
 * fs paths to absolute directories (in the sealed policy arena) and hands them
 * here once at boot, before any connection opens; duck_open reads the pointer.
 * The array + strings are sealed (RO) memory owned by the caller for process
 * life. When no fs paths are declared (or DuckDB isn't enabled) the policy is
 * empty and duck_open applies full lockdown (mode A). */
static const char *const *g_duck_allowed_dirs = NULL;
static int                g_duck_allowed_ndirs = 0;

void hl_db_duckdb_set_fs_policy(const char *const *dirs, int ndirs)
{
    g_duck_allowed_dirs  = (ndirs > 0) ? dirs : NULL;
    g_duck_allowed_ndirs = (ndirs > 0 && dirs) ? ndirs : 0;
}

/* Build the DuckDB list literal ['d1','d2',...] for the declared policy (single
 * quotes doubled), used for both the config-time allowed_directories option and
 * the post-connect SET. Returns a malloc'd string, or NULL (empty policy/OOM). */
static char *duck_alloc_dirs_list(void)
{
    if (g_duck_allowed_ndirs <= 0 || !g_duck_allowed_dirs) return NULL;
    size_t cap = 8;
    for (int i = 0; i < g_duck_allowed_ndirs; i++)
        cap += (g_duck_allowed_dirs[i] ? strlen(g_duck_allowed_dirs[i]) : 0) * 2 + 8;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t off = 0;
    s[off++] = '[';
    for (int i = 0; i < g_duck_allowed_ndirs; i++) {
        const char *d = g_duck_allowed_dirs[i] ? g_duck_allowed_dirs[i] : "";
        if (i > 0 && off < cap) s[off++] = ',';
        if (off < cap) s[off++] = '\'';
        for (const char *p = d; *p && off + 2 < cap; p++) {
            if (*p == '\'') s[off++] = '\'';
            s[off++] = *p;
        }
        if (off < cap) s[off++] = '\'';
    }
    if (off + 2 >= cap) { free(s); return NULL; }
    s[off++] = ']';
    s[off] = '\0';
    return s;
}

/* ── Security lockdown ─────────────────────────────────────────────────
 *
 * DuckDB SQL can drive its own file/network I/O (read_csv, read_parquet,
 * COPY, httpfs), which would bypass Hull's capability model. Network is
 * already structurally absent (httpfs / S3 are not in the linked archive
 * set). Here we forbid installing / loading extensions and gate external
 * (local file) access: OFF entirely (mode A) when no fs paths are declared,
 * or ON but bounded to allowed_directories (mode B, applied post-connect in
 * duck_open) when they are. lock_configuration (in duck_open, after connect)
 * then freezes it so app SQL cannot re-enable anything.
 *
 * These options are set at config time (before open); lock_configuration is
 * applied last so the config-time sets are not themselves rejected. */
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
    /* Pin memory + threads so DuckDB does NOT auto-detect them by probing /sys
     * and /proc. Even with those paths unveiled (see sandbox.c), auto-detection
     * hits some resource that fails under the sandbox and DuckDB NULL-derefs
     * (aborts) rather than degrading — pinning the values avoids that path
     * entirely. Fixed, conservative defaults; a future manifest option can make
     * them tunable (tracked in docs/duckdb_backend_design.md §3.2). */
    if (duckdb_set_config(config, "memory_limit", "2GB") != DuckDBSuccess)
        return -1;
    if (duckdb_set_config(config, "threads", "4") != DuckDBSuccess)
        return -1;
    /* Mode B enables external access but bounds it to allowed_directories;
     * mode A leaves it off (DB file + :memory: only). */
    const char *ext = (g_duck_allowed_ndirs > 0) ? "true" : "false";
    if (duckdb_set_config(config, "enable_external_access", ext) != DuckDBSuccess)
        return -1;
    return 0;
}

/* Emit `SET allowed_directories = ['d1','d2',...]` for the declared policy and
 * run it on @p con (single quotes in a path are doubled). Returns 0, or -1 on
 * allocation / execution failure (with *errmsg set via the caller). No-op
 * returning 0 when the policy is empty. Called after connect, before the lock. */
static int duck_apply_allowed_dirs(duckdb_connection con)
{
    if (g_duck_allowed_ndirs <= 0 || !g_duck_allowed_dirs) return 0;   /* mode A */
    char *list = duck_alloc_dirs_list();
    if (!list) return -1;
    size_t cap = strlen(list) + 32;   /* "SET allowed_directories = " + NUL */
    char *sql = malloc(cap);
    if (!sql) { free(list); return -1; }
    snprintf(sql, cap, "SET allowed_directories = %s", list);
    free(list);

    duckdb_result r;
    int ok = (duckdb_query(con, sql, &r) == DuckDBSuccess);
    duckdb_destroy_result(&r);
    free(sql);
    return ok ? 0 : -1;
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

    /* Mode B: bound external file access to the manifest-declared directories
     * (before the lock, so it sticks). No-op under mode A. */
    if (duck_apply_allowed_dirs(s->con) != 0) {
        duck_set_err(s, "duckdb allowed_directories failed");
        duckdb_disconnect(&s->con);
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

/* ── Dialect helpers ──────────────────────────────────────────────────
 *
 * DuckDB speaks the Postgres-family `ON CONFLICT (...) DO NOTHING / DO UPDATE
 * SET col = excluded.col` grammar and exposes information_schema.columns, so
 * these mirror the Postgres backend. The one simplification: DuckDB binds
 * positional params with '?' (no $n rewrite), so placeholders are emitted
 * literally and pass straight through duck_query's prepared-statement path. */

/* Append to a bounded buffer; latch overflow via *off >= cap (checked once at
 * the end, before NUL-terminating). */
static void duck_ap(char *buf, size_t cap, size_t *off, const char *str)
{
    size_t n = strlen(str);
    if (*off + n < cap) memcpy(buf + *off, str, n);
    *off += n;
}

static int duck_build_and_run_insert(HlDbHandle *h, const char *table,
                                     const char *const *conflict_cols,
                                     int n_conflict, const char *const *cols,
                                     const HlValue *values, int n_cols,
                                     int do_update)
{
    /* Budget each column up to three times (insert list, and both sides of
     * `col = excluded.col` for DO UPDATE) plus overhead, so a long column name
     * cannot overflow the estimate and silently truncate. */
    size_t cap = 64 + strlen(table);
    for (int i = 0; i < n_cols; i++) cap += strlen(cols[i]) * 3 + 40;
    for (int i = 0; i < n_conflict; i++) cap += strlen(conflict_cols[i]) + 4;
    char *sql = malloc(cap);
    if (!sql) return -1;
    size_t off = 0;

    duck_ap(sql, cap, &off, "INSERT INTO ");
    duck_ap(sql, cap, &off, table);
    duck_ap(sql, cap, &off, " (");
    for (int i = 0; i < n_cols; i++) {
        if (i) duck_ap(sql, cap, &off, ", ");
        duck_ap(sql, cap, &off, cols[i]);
    }
    duck_ap(sql, cap, &off, ") VALUES (");
    for (int i = 0; i < n_cols; i++)
        duck_ap(sql, cap, &off, i ? ", ?" : "?");
    duck_ap(sql, cap, &off, ") ON CONFLICT (");
    for (int i = 0; i < n_conflict; i++) {
        if (i) duck_ap(sql, cap, &off, ", ");
        duck_ap(sql, cap, &off, conflict_cols[i]);
    }
    duck_ap(sql, cap, &off, ") DO ");
    if (do_update) {
        duck_ap(sql, cap, &off, "UPDATE SET ");
        for (int i = 0; i < n_cols; i++) {
            if (i) duck_ap(sql, cap, &off, ", ");
            duck_ap(sql, cap, &off, cols[i]);
            duck_ap(sql, cap, &off, " = excluded.");
            duck_ap(sql, cap, &off, cols[i]);
        }
    } else {
        duck_ap(sql, cap, &off, "NOTHING");
    }

    if (off >= cap) {                            /* estimate was too small */
        free(sql);
        duck_set_err(h ? h->ctx : NULL,
                     "insert/upsert SQL exceeded its estimated buffer");
        return -1;
    }
    sql[off] = '\0';

    int rc = duck_exec(h, sql, values, n_cols);
    free(sql);
    return rc;
}

static int duck_insert_if_absent(HlDbHandle *h, const char *table,
                                 const char *const *conflict_cols, int n_conflict,
                                 const char *const *cols,
                                 const HlValue *values, int n_cols)
{
    return duck_build_and_run_insert(h, table, conflict_cols, n_conflict,
                                     cols, values, n_cols, 0);
}

static int duck_upsert(HlDbHandle *h, const char *table,
                       const char *const *conflict_cols, int n_conflict,
                       const char *const *cols,
                       const HlValue *values, int n_cols)
{
    return duck_build_and_run_insert(h, table, conflict_cols, n_conflict,
                                     cols, values, n_cols, 1);
}

typedef struct {
    HlDbColumnCallback cb;
    void              *cb_ctx;
} DuckColsFwd;

static int duck_table_columns_row(void *ctx, HlColumn *cols, int ncols)
{
    DuckColsFwd *fwd = ctx;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].value.type != HL_TYPE_TEXT || !cols[i].value.s)
            continue;
        /* duck_decode borrows the chunk's VARCHAR bytes, which are NOT
         * NUL-terminated (a 12-byte inline string fills the buffer with no
         * terminator). HlDbColumnCallback takes a C string, so copy + terminate
         * using the tracked length before handing it off. */
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

static int duck_table_columns(HlDbHandle *h, const char *table,
                              HlDbColumnCallback cb, void *cb_ctx)
{
    DuckColsFwd fwd = { cb, cb_ctx };
    HlValue p;
    p.type = HL_TYPE_TEXT;
    p.s = table;
    p.len = strlen(table);
    return duck_query(h,
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_name = ? ORDER BY ordinal_position",
        &p, 1, duck_table_columns_row, &fwd, NULL);
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
    .insert_if_absent     = duck_insert_if_absent,
    .upsert               = duck_upsert,
    .table_columns        = duck_table_columns,
    /* Raw native_handle stays NULL: udf + agent introspection are SQLite-only,
     * so no consumer needs the raw duckdb_connection yet. The native_tag
     * already distinguishes the backend if one ever does. */
    .native_handle        = NULL,
};

#endif /* HL_ENABLE_DUCKDB */
