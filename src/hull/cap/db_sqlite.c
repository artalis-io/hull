/*
 * cap/db_sqlite.c — SQLite backend for the database vtable
 *
 * Wraps existing hl_cap_db_* functions into the HlDbBackend vtable.
 * Statement cache, init/shutdown, and all SQLite-specific operations
 * are encapsulated in HlDbSqliteCtx.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/db_backend.h"
#include "hull/cap/db.h"
#include "hull/alloc.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* ── SQLite context ───────────────────────────────────────────────── */

typedef struct {
    sqlite3     *db;
    HlStmtCache  cache;
    HlAllocator *alloc;
} HlDbSqliteCtx;

/* ── Vtable implementations ──────────────────────────────────────── */

static int sqlite_open(void **ctx, const char *dsn, HlAllocator *alloc)
{
    HlDbSqliteCtx *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    s->alloc = alloc;

    int rc = sqlite3_open(dsn, &s->db);
    if (rc != SQLITE_OK) {
        if (s->db) sqlite3_close(s->db);
        free(s);
        return -1;
    }

    if (hl_cap_db_init(s->db) != 0) {
        sqlite3_close(s->db);
        free(s);
        return -1;
    }

    hl_stmt_cache_init(&s->cache, s->db, alloc);

    *ctx = s;
    return 0;
}

static void sqlite_close(void *ctx)
{
    if (!ctx) return;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;

    hl_stmt_cache_destroy(&s->cache);
    hl_cap_db_shutdown(s->db);
    sqlite3_close(s->db);
    free(s);
}

static int sqlite_query(void *ctx, const char *sql,
                        const HlValue *params, int nparams,
                        HlRowCallback cb, void *cb_ctx,
                        HlAllocator *alloc)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    return hl_cap_db_query(&s->cache, sql, params, nparams,
                           cb, cb_ctx, alloc);
}

static int sqlite_exec(void *ctx, const char *sql,
                       const HlValue *params, int nparams)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    return hl_cap_db_exec(&s->cache, sql, params, nparams);
}

static int sqlite_begin(void *ctx)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    return hl_cap_db_begin(s->db);
}

static int sqlite_commit(void *ctx)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    return hl_cap_db_commit(s->db);
}

static int sqlite_rollback(void *ctx)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    return hl_cap_db_rollback(s->db);
}

static int64_t sqlite_last_id(void *ctx)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    return hl_cap_db_last_id(s->db);
}

static const char *sqlite_errmsg(void *ctx)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    return sqlite3_errmsg(s->db);
}

static void sqlite_guard_stale_txn(void *ctx)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)ctx;
    hl_cap_db_guard_stale_txn(s->db);
}

/* ── Exported backend ─────────────────────────────────────────────── */

const HlDbBackend hl_db_backend_sqlite = {
    .name            = "sqlite",
    .open            = sqlite_open,
    .close           = sqlite_close,
    .query           = sqlite_query,
    .exec            = sqlite_exec,
    .begin           = sqlite_begin,
    .commit          = sqlite_commit,
    .rollback        = sqlite_rollback,
    .last_id         = sqlite_last_id,
    .errmsg          = sqlite_errmsg,
    .guard_stale_txn = sqlite_guard_stale_txn,
};

/* ── Accessors ────────────────────────────────────────────────────── */

sqlite3 *hl_db_sqlite_raw(HlDbHandle *h)
{
    if (!h || h->backend != &hl_db_backend_sqlite) return NULL;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return s ? s->db : NULL;
}

HlStmtCache *hl_db_sqlite_cache(HlDbHandle *h)
{
    if (!h || h->backend != &hl_db_backend_sqlite) return NULL;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return s ? &s->cache : NULL;
}

/* ── Wrap an externally-owned sqlite3* ────────────────────────────── */

int hl_db_sqlite_wrap(HlDbHandle *out, sqlite3 *db)
{
    if (!out || !db) return -1;
    HlDbSqliteCtx *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->db    = db;       /* borrowed — caller owns lifetime */
    s->alloc = NULL;
    hl_stmt_cache_init(&s->cache, db, NULL);
    out->backend = &hl_db_backend_sqlite;
    out->ctx     = s;
    return 0;
}

void hl_db_sqlite_unwrap(HlDbHandle *h)
{
    if (!h || h->backend != &hl_db_backend_sqlite || !h->ctx) return;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    hl_stmt_cache_destroy(&s->cache);
    /* NOTE: s->db is borrowed — do NOT sqlite3_close it here. */
    free(s);
    h->ctx = NULL;
}
