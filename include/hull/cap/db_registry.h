/*
 * cap/db_registry.h: Named database connection registry
 *
 * Backs the multi-backend API: an app declares named
 * connections via `manifest.databases`, and db.connect(name) / db.default()
 * resolve them through this registry. Connections open lazily on first use
 * and are cached by name, so an app can mix, e.g., a Postgres primary and a
 * local SQLite cache in one process.
 *
 * The connection named "default" is the stdlib / db.default() target; when an
 * app declares no databases map it falls back to the -d flag DSN, seeded via
 * hl_db_registry_seed so the already-opened default connection is reused
 * rather than opened twice.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_REGISTRY_H
#define HL_CAP_DB_REGISTRY_H

#include "hull/cap/db_backend.h"

typedef struct HlManifest  HlManifest;
typedef struct HlManifestDbDynamic HlManifestDbDynamic;
typedef struct HlAllocator HlAllocator;
typedef struct HlDbRegistry HlDbRegistry;

/*
 * Create a registry. @p manifest supplies the declared databases map (may be
 * NULL); it is borrowed and must outlive the registry (the sealed manifest
 * does). Nothing is opened here. @p alloc is passed to each backend open
 * (may be NULL for raw malloc). Returns NULL on allocation failure.
 */
HlDbRegistry *hl_db_registry_create(const HlManifest *manifest,
                                    const char *default_dsn,
                                    HlAllocator *alloc);

/*
 * Fast accessor for the already-open "default" connection (the -d flag DSN,
 * opened at startup so it is cached). Returns NULL if absent (compute-only /
 * --no-db). No open, no error path: this is the per-request hot path used by
 * the stale-transaction guards.
 *
 * The per-request guard sites call this unconditionally, so a pure-compute
 * build (no registry compiled in) gets an inline no-op instead of an
 * undefined symbol.
 */
#ifdef HL_ENABLE_DB
HlDbHandle *hl_db_registry_default(HlDbRegistry *reg);
#else
static inline HlDbHandle *hl_db_registry_default(HlDbRegistry *reg)
{ (void)reg; return (HlDbHandle *)0; }
#endif

/*
 * The stale-transaction guard over EVERY open connection - the default, named
 * ones, the internal one and every open db.open handle - not only "default":
 * a handler that raised between BEGIN and COMMIT on a named connection left it
 * in the transaction for every later request. Opens nothing.
 *
 * The runtimes run it when an entry (a request, middleware, SSE event, timer,
 * ws-server or ws-client callback) starts and when it returns, raises or
 * parks, and before a parked continuation resumes (audit 6 M1). No
 * transaction may span a wait (below), so at each of those points any open
 * transaction belongs to no running entry: run only at the start, a
 * transaction a failed entry left open was joined by whatever resumed next.
 */
#ifdef HL_ENABLE_DB
void hl_db_registry_guard_stale_txns(HlDbRegistry *reg);
#else
static inline void hl_db_registry_guard_stale_txns(HlDbRegistry *reg)
{ (void)reg; }
#endif

/*
 * The name of an open registry connection that is inside a transaction
 * ("internal" for the stdlib's internal one, "db.open" for a dynamic handle),
 * or NULL when none is. No I/O.
 *
 * The runtimes refuse to WAIT (http.fetch, db.async, hull.sleep, ...) while
 * this is non-NULL. Every registry connection is shared by all requests, SSE
 * events and timers, so while a handler is parked another one runs on the
 * same connection: it used to roll the parked handler's transaction back
 * (the stale-transaction guard above, which cannot tell a parked owner from
 * a dead one), after which the parked handler's remaining statements
 * autocommitted and its COMMIT "succeeded" with half its writes gone. With
 * no transaction ever held across a wait, every transaction the guard finds
 * open at the start of an entry belongs to an entry that has finished, so
 * rolling it back is always right (audit 5 M1).
 *
 * Session state that is as good as a transaction counts too
 * (hl_db_holds_txn: MySQL autocommit off, LOCK TABLES, XA; audit 40): the
 * guard resets a session holding it, and run before a parked handler
 * resumes it reset the handler's own `SET autocommit = 0` under it.
 */
#ifdef HL_ENABLE_DB
const char *hl_db_registry_open_txn(HlDbRegistry *reg);
#else
static inline const char *hl_db_registry_open_txn(HlDbRegistry *reg)
{ (void)reg; return (const char *)0; }
#endif

/*
 * Hold the stale-transaction guard off (hold: +1, release: -1; nests) on the
 * event-loop thread. Only for the in-process test request (test.get & co):
 * it runs the app's dispatch, which guards at its start and end, INSIDE a
 * test case - and a case may have a transaction open (`db.batch(function()
 * ... test.get(...) ... end)`), which is live, not stale: the guard rolled
 * it back under the case (audit 10). The test binding holds the guard only
 * while the case has a transaction open, so a transaction the request
 * itself leaves open is still rolled back otherwise.
 */
#ifdef HL_ENABLE_DB
void hl_db_registry_guard_hold(int delta);
#else
static inline void hl_db_registry_guard_hold(int delta) { (void)delta; }
#endif

/* 1 while hl_db_registry_guard_hold holds the guard off (the guard then
 * rolls nothing back, so a caller that would report a rollback skips it). */
#ifdef HL_ENABLE_DB
int hl_db_registry_guard_held(void);
#else
static inline int hl_db_registry_guard_held(void) { return 0; }
#endif

/*
 * Point the registry at the app's databases map. The manifest is only known
 * after the app runs app.manifest(), which is after the registry is created
 * at db-open time, so the serve path injects the sealed manifest here once it
 * is available. Borrowed; must outlive the registry (the sealed manifest
 * does). Until set, only seeded connections (e.g. "default") resolve.
 */
void hl_db_registry_set_manifest(HlDbRegistry *reg, const HlManifest *manifest);

/* The app directory a manifest file DSN (databases.named / .internal) must
 * stay inside (audit 34): a relative one is opened under it, and one outside
 * it, through a symbolic link, or naming a non-regular file is refused. Set
 * right after create, before any manifest connection opens; until set every
 * manifest file DSN is refused. -1 once sealed or when @p app_dir cannot be
 * resolved. The -d DSN (the operator's) is not checked. */
int hl_db_registry_set_app_dir(HlDbRegistry *reg, const char *app_dir);

/* Make the registry's DSN sources (the manifest pointer and the -d DSN)
 * read-only for the rest of the process - the last step of wiring, after
 * set_manifest. 0 on success; -1 is fatal for the caller. Destroy undoes it. */
int hl_db_registry_seal(HlDbRegistry *reg);

/* The connection the stdlib keeps its own _hull_* tables on: the manifest's
 * `databases.internal` DSN when one is declared (opened lazily, cached),
 * otherwise the default connection. Reachable only through the stdlib-only
 * hull.db._internal module - db.connect cannot name it. NULL + *err on
 * failure. */
HlDbHandle *hl_db_registry_internal(HlDbRegistry *reg, const char **err);

/* Whether the internal connection is separate from the default one: declared
 * (databases.internal) or the Hull-owned default internal database. */
int hl_db_registry_has_internal(const HlDbRegistry *reg);

/* The Hull-owned default internal database (cap/db_internal.h, audit
 * convergence §7.2): the DSN hl_db_internal_default_dsn worked out, used as
 * the internal connection when the manifest declares no databases.internal.
 * @p base_len is where the symlink check starts below; @p mkdir_parent makes
 * the leaf's directory (<app>/.hull, 0700) when it is missing; @p active 0
 * records the path without using it (--keep-internal-in-app-db, so the
 * startup step can move tables back). In the sealed span, like the -d DSN;
 * -1 once sealed or for a DSN too long. */
int hl_db_registry_set_default_internal(HlDbRegistry *reg, const char *dsn,
                                        size_t base_len, int mkdir_parent,
                                        int active);

/* The default internal DSN recorded above (active or not), or NULL. */
const char *hl_db_registry_default_internal_path(const HlDbRegistry *reg);

/* 1 when the internal connection resolves to the ACTIVE default internal
 * database (no databases.internal declared or learned). */
int hl_db_registry_internal_is_default(const HlDbRegistry *reg);

/* The connection auth-flows' standard_users adapter keeps the app's users
 * table on: a DECLARED databases.internal (the users table then lives there,
 * under the internal role), else the default connection - never the
 * Hull-owned default internal database, which holds only _hull_* tables (the
 * users table is the app's, made by its migrations). NULL + *err. */
HlDbHandle *hl_db_registry_internal_or_app(HlDbRegistry *reg, const char **err);

/* Open the recorded default internal database into @p out, outside the
 * registry (the caller closes it with out->backend->close) - the keep-flag
 * move back. 0, or -1 + *err. */
int hl_db_registry_open_default_internal(HlDbRegistry *reg, HlDbHandle *out,
                                         const char **err);

/* Before the manifest is wired, a stdlib init() at app top level may already
 * need the internal connection: the runtime reads `databases.internal` from
 * the app's manifest table and hands it over here. The wired manifest's value
 * takes over afterwards. -1 once sealed, or for a DSN that is too long. */
int hl_db_registry_set_internal_dsn(HlDbRegistry *reg, const char *dsn);

/* Whether the (sealed) manifest has been wired - after this the internal
 * connection is final and may be cached. */
int hl_db_registry_manifest_wired(const HlDbRegistry *reg);

/* Whether the default connection is a database where Hull's SQL-text check
 * cannot keep the app away from the stdlib's _hull_* tables, so
 * databases.internal is the real separation: a network database (Postgres /
 * MySQL: dynamic SQL builds a name at run time - a database role separates),
 * or DuckDB (audit 25: its dynamic-SQL table functions did the same; Hull
 * refuses the ones it knows, and a separate database file separates). */
int hl_db_registry_default_is_network(const HlDbRegistry *reg);

/*
 * Seed a pre-opened, externally-owned connection under @p name (typically
 * "default", the -d flag connection that app_context already opened). The
 * handle is value-copied and marked not-owned, so hl_db_registry_destroy
 * will NOT close it; the caller keeps ownership. Returns 0 / -1.
 */
int hl_db_registry_seed(HlDbRegistry *reg, const char *name,
                        const HlDbHandle *h);

/*
 * Resolve @p name to an open connection, opening + caching it on first use.
 * Resolution order: a cached connection, then a manifest databases entry
 * (with { dsn_env } resolved from the environment), then a seeded connection.
 * Returns NULL with *err (a static message) set on unknown name / unset
 * dsn_env / backend-select failure / open failure. The returned handle is
 * owned by the registry (unless seeded) and valid until destroy.
 */
HlDbHandle *hl_db_registry_get(HlDbRegistry *reg, const char *name,
                               const char **err);

/*
 * Return the DSN a connection was opened from, matched by handle identity
 * (the pointer returned by _get / _default). Used by db.async on a connection
 * object to tell the worker pool WHICH database to open its per-thread
 * connection against. Returns NULL for a seeded/borrowed connection whose DSN
 * the registry never saw (the async path then falls back to the worker's
 * default DSN). The returned string is registry-owned and valid until destroy.
 */
#ifdef HL_ENABLE_DB
const char *hl_db_registry_dsn_for(HlDbRegistry *reg, const HlDbHandle *h);

/* The manifest's databases.dynamic policy (for db.open), or NULL if no manifest
 * is set. Borrowed; valid until the sealed manifest is torn down. */
const HlManifestDbDynamic *hl_db_registry_dynamic_policy(HlDbRegistry *reg);
#else
static inline const char *hl_db_registry_dsn_for(HlDbRegistry *reg,
                                                 const HlDbHandle *h)
{ (void)reg; (void)h; return (const char *)0; }
static inline const HlManifestDbDynamic *
hl_db_registry_dynamic_policy(HlDbRegistry *reg)
{ (void)reg; return (const HlManifestDbDynamic *)0; }
#endif

/*
 * Close every registry-owned connection and mark the registry closed, but
 * keep its memory: runtime connection objects hold pointers to the handles
 * in it, and a Lua __gc finalizer run by the runtime's destroy may still use
 * one (audit 17). Afterwards every handle has no backend (its calls fail),
 * _default / _get / _internal / _dynamic_policy return NULL, and nothing
 * opens again. The app context runs this BEFORE the runtime's destroy (a
 * udf's xDestroy needs the runtime alive) and destroy AFTER it. Seeded
 * (externally-owned) connections are not closed, only dropped. Idempotent.
 */
void hl_db_registry_close_all(HlDbRegistry *reg);

/* 1 once hl_db_registry_close_all has run (or for NULL). */
int hl_db_registry_is_closed(const HlDbRegistry *reg);

/* Close every registry-owned connection (hl_db_registry_close_all) and free
 * the registry. Seeded (externally-owned) connections are left untouched. */
void hl_db_registry_destroy(HlDbRegistry *reg);

#endif /* HL_CAP_DB_REGISTRY_H */
