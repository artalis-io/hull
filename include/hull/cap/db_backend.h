/*
 * cap/db_backend.h — Database backend vtable
 *
 * Decouples the query engine from SQLite via a pluggable vtable.
 * Enables pure compute apps (no DB), future alternative backends
 * (PostgreSQL, DuckDB), while hull internals always use embedded SQLite.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_BACKEND_H
#define HL_CAP_DB_BACKEND_H

#include "hull/cap/types.h"
#include <stdint.h>

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef int (*HlRowCallback)(void *ctx, HlColumn *cols, int ncols);

/* ── Backend vtable ───────────────────────────────────────────────── */

typedef struct HlDbBackend {
    const char *name;   /* "sqlite", "none" */
    int    (*open)(void **ctx, const char *dsn, HlAllocator *alloc);
    void   (*close)(void *ctx);
    int    (*query)(void *ctx, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *cb_ctx, HlAllocator *alloc);
    int    (*exec)(void *ctx, const char *sql,
                   const HlValue *params, int nparams);
    int    (*begin)(void *ctx);
    int    (*commit)(void *ctx);
    int    (*rollback)(void *ctx);
    int64_t (*last_id)(void *ctx);
    const char *(*errmsg)(void *ctx);
    void   (*guard_stale_txn)(void *ctx);  /* NULL = no-op */
} HlDbBackend;

typedef struct HlDbHandle {
    const HlDbBackend *backend;
    void              *ctx;
} HlDbHandle;

/* ── Inline wrappers ──────────────────────────────────────────────── */

static inline int hl_db_query(HlDbHandle *h, const char *sql,
                              const HlValue *params, int nparams,
                              HlRowCallback cb, void *cb_ctx,
                              HlAllocator *alloc)
{
    if (!h || !h->backend) return -1;
    return h->backend->query(h->ctx, sql, params, nparams,
                             cb, cb_ctx, alloc);
}

static inline int hl_db_exec(HlDbHandle *h, const char *sql,
                             const HlValue *params, int nparams)
{
    if (!h || !h->backend) return -1;
    return h->backend->exec(h->ctx, sql, params, nparams);
}

static inline int hl_db_begin(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->begin(h->ctx);
}

static inline int hl_db_commit(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->commit(h->ctx);
}

static inline int hl_db_rollback(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->rollback(h->ctx);
}

static inline int64_t hl_db_last_id(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->last_id(h->ctx);
}

static inline const char *hl_db_errmsg(HlDbHandle *h)
{
    if (!h || !h->backend) return "no database";
    return h->backend->errmsg(h->ctx);
}

static inline void hl_db_guard_stale_txn(HlDbHandle *h)
{
    if (!h || !h->backend || !h->backend->guard_stale_txn) return;
    h->backend->guard_stale_txn(h->ctx);
}

/* ── SQLite backend ───────────────────────────────────────────────── */

extern const HlDbBackend hl_db_backend_sqlite;

/* Get raw sqlite3* from an HlDbHandle (NULL if not SQLite backend) */
struct sqlite3 *hl_db_sqlite_raw(HlDbHandle *h);

/* Get statement cache from a SQLite-backed HlDbHandle (NULL if not SQLite) */
struct HlStmtCache *hl_db_sqlite_cache(HlDbHandle *h);

#endif /* HL_CAP_DB_BACKEND_H */
