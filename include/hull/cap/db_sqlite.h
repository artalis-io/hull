/*
 * cap/db_sqlite.h - SQLite backend registration + construction helpers
 *
 * The abstract interface (db_backend.h) stays free of concrete-backend
 * symbols; this header exposes the SQLite backend const + the wrap/unwrap
 * helpers used by code paths that open SQLite directly (agent introspection,
 * migrations, tests). The native sqlite3* is reached generically via
 * hl_db_backend_native_handle (tag == HL_DB_NATIVE_SQLITE), not from here.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_SQLITE_H
#define HL_CAP_DB_SQLITE_H

#include "hull/cap/db_backend.h"

struct sqlite3;
struct HlStmtCache;

/* The native sqlite3* if @p h is the SQLite backend, else NULL. A typed
 * convenience over hl_db_backend_native_handle (tag == HL_DB_NATIVE_SQLITE) for
 * the SQLite-only paths (udf registration, agent introspection). Lives here,
 * not in the abstract db_backend.h, so the interface carries no sqlite symbol.
 * Compiles in every flavor (returns NULL when SQLite isn't built). */
static inline struct sqlite3 *hl_db_sqlite_raw(HlDbHandle *h)
{
    HlDbNativeTag tag;
    void *n = hl_db_backend_native_handle(h, &tag);
    return (tag == HL_DB_NATIVE_SQLITE) ? (struct sqlite3 *)n : NULL;
}

#ifdef HL_ENABLE_SQLITE
extern const HlDbBackend hl_db_backend_sqlite;

/* Statement cache of a SQLite-backed handle (NULL if not SQLite). The cache is
 * a sqlite prepared-statement adapter, hence SQLite-specific. */
struct HlStmtCache *hl_db_sqlite_cache(HlDbHandle *h);

/*
 * Wrap an externally-managed sqlite3* in an HlDbHandle for code paths that open
 * SQLite directly (agent_lib, migrations, tests). The handle does NOT own the
 * sqlite3*; the caller stays responsible for sqlite3_close. Allocates a small
 * adapter ctx (with its own statement cache over the borrowed sqlite3); call
 * hl_db_sqlite_unwrap(&handle) when done. Returns 0 / -1 (alloc failure).
 */
int  hl_db_sqlite_wrap(HlDbHandle *out, struct sqlite3 *db);
void hl_db_sqlite_unwrap(HlDbHandle *h);
#else
/* No-SQLite build: cache is NULL, wrap fails, unwrap is a no-op, so callers
 * that legitimately degrade for a non-SQLite handle stay compilable without
 * per-site #ifdef. */
static inline struct HlStmtCache *hl_db_sqlite_cache(HlDbHandle *h)
{ (void)h; return (struct HlStmtCache *)0; }
static inline int hl_db_sqlite_wrap(HlDbHandle *out, struct sqlite3 *db)
{ (void)out; (void)db; return -1; }
static inline void hl_db_sqlite_unwrap(HlDbHandle *h) { (void)h; }
#endif

#endif /* HL_CAP_DB_SQLITE_H */
