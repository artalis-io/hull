/*
 * cap/db_internal.h: the Hull-owned internal database (audit convergence
 * §7.2)
 *
 * The stdlib's own tables (sessions, auth-flows, TOTP, audit log, RBAC,
 * idempotency keys) live on the internal connection
 * (hull.db._internal / hull:db:_internal). Unless the manifest declares
 * `databases.internal`, that connection is a SQLite file Hull owns, never the
 * app's database file:
 *
 *   - the default database is a SQLite file inside the app directory (the
 *     implicit <app>/data.db, or a -d path below the app):
 *         <app>/.hull/internal.db
 *     (`.hull` is reserved from every app capability: fs, db.open, a
 *     databases.named file DSN);
 *   - a -d SQLite file outside the app directory (a deploy volume):
 *         <that file>.hull-internal
 *     beside it, so it persists where the operator persists the database
 *     (no app capability reaches outside the app directory);
 *   - an in-memory default (`hull test`, `hull agent`, an app.main program
 *     with no -d): a separate in-memory database;
 *   - a network or DuckDB default, or none: no default - the internal
 *     tables stay on the default connection (see docs/design/
 *     internal_db_separation.md for why).
 *
 * The outbox, inbox, jobs, attachment, search and kv SQL tables are NOT moved:
 * they must commit atomically with the app's own writes (or read its tables),
 * so they stay on the app connection, as does `_hull_migrations`. (Attachment
 * and auth-flows' standard_users table use hull.db._internal.app_connection():
 * a declared databases.internal, else the default connection.)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_INTERNAL_H
#define HL_CAP_DB_INTERNAL_H

#include <stddef.h>

typedef struct HlDbRegistry HlDbRegistry;
typedef struct HlDbHandle HlDbHandle;

/* The file name of the default internal database below <app>/.hull. */
#define HL_DB_INTERNAL_FILE "internal.db"
/* The suffix of one beside a -d database outside the app directory. */
#define HL_DB_INTERNAL_BESIDE_SUFFIX ".hull-internal"

/* Where the default internal database lives for an app whose default DSN is
 * @p default_dsn and whose app directory canonicalises to @p app_dir_real.
 * Writes the DSN (an absolute path, or ":memory:") into @p out and returns:
 *    1  a file DSN; *base_len is the length of the prefix the registry's
 *       symlink check starts below (the app directory for <app>/.hull, the
 *       database's directory for a beside file), *in_app_hull is 1 for
 *       <app>/.hull/internal.db;
 *    2  ":memory:";
 *    0  no default (network / DuckDB / unknown scheme / none, or the default
 *       database IS that file);
 *   -1  the path does not fit.
 * Pure (no I/O beyond realpath of existing ancestors). */
int hl_db_internal_default_dsn(const char *app_dir_real,
                               const char *default_dsn,
                               char *out, size_t outsz,
                               size_t *base_len, int *in_app_hull);

/* The `_hull_*` tables the stdlib keeps on the internal connection, in an
 * order where a referenced table comes before its referrers (NULL ends it).
 * The tables NOT listed (outbox, inbox, jobs, search, kv, migrations) stay
 * on the app connection. */
const char *const *hl_db_internal_tables(void);

/* Move every internal table @p src holds into @p dst (both SQLite): per
 * table, in one transaction on @p dst, create it from @p src's own DDL when
 * @p dst lacks it and copy every row (INSERT OR IGNORE, so a re-run after a
 * crash, or rows a stdlib init() already wrote, never lose a row); once every
 * table is copied, drop them from @p src (referrers first). Crash-safe: a
 * table is dropped only after its copy committed, and a re-run finishes the
 * move. Runs unbudgeted and with the `_hull_*` authorizer trusted. @p what
 * names the direction for the log. Returns the number of tables moved
 * (0 = nothing to do), or -1 with *err set (nothing dropped from @p src for
 * the failed table). */
int hl_db_internal_move(HlDbHandle *src, HlDbHandle *dst, const char *what,
                        char *err, size_t errsz);

/* The startup step both runners call once the manifest is wired and before
 * the kernel sandbox applies:
 *   - normal: when the internal connection is the default internal database
 *     (no databases.internal), open it now (before the sandbox narrows file
 *     access) and move the app database's internal tables into it;
 *   - @p keep (--keep-internal-in-app-db): move them back from the default
 *     internal file, when one exists, into the app database.
 * Only a SQLite default connection is migrated (a network / DuckDB one has
 * no default internal database: section 5 of the design note). The file is
 * not created for an app that needs none: when @p needed is 0 (no resolved
 * module reaches the internal connection, hl_module_set_uses_internal_db),
 * the app database holds no internal table and no internal file exists yet,
 * nothing is done. Returns 0, or -1 after logging (the app must not start: it
 * would run without its sessions / roles). */
int hl_db_internal_startup(HlDbRegistry *reg, int keep, int needed);

/* The file the kernel sandbox must let Hull read and write
 * (HlSandboxPolicy.internal_db_path): the active default internal database
 * when it is a file, else NULL. */
const char *hl_db_internal_sandbox_path(const HlDbRegistry *reg);

#endif /* HL_CAP_DB_INTERNAL_H */
