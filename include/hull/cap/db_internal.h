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
/* The same with the app's state directory (shared/state_dir.h; canonical,
 * or NULL): a default database under it gets <state>/.hull/internal.db -
 * the state root reserves .hull as the app root does - with base_len that
 * directory's and *in_app_hull 1 (its .hull is made the same way). */
int hl_db_internal_default_dsn_ex(const char *app_dir_real,
                                  const char *state_dir_real,
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

/* Unit-test seam: let @p after column-name pushes succeed, then fail every
 * later one, as an allocation failure would (-1 = off). */
void hl_db_internal_fail_column_push_for_test(int after);

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

/* The step both runners call INSTEAD of hl_db_internal_startup under
 * --no-migrate, which skips the move. It refuses (-1, after logging an error
 * naming `hull migrate`) when the stdlib would read a database that does not
 * hold its tables:
 *   - normal: the app database holds internal tables and the default
 *     internal database does not record the move as done (a pre-§7.2
 *     deployment started with --no-migrate: sessions, roles and TOTP
 *     enrolments would read as empty - and an empty _hull_totp reads as "not
 *     enrolled", so a password alone signed in);
 *   - @p keep: the default internal file still holds internal tables an
 *     earlier start moved into it.
 * A fresh app (no internal table anywhere), a declared databases.internal, a
 * network / DuckDB / in-memory default, and a move already recorded (strays
 * in the app database are then ignored with a WARN) return 0. Creates no
 * file. */
int hl_db_internal_check_unmoved(HlDbRegistry *reg, int keep);

/* Record the default internal database for app directory @p app_dir and
 * default DSN @p default_dsn (hl_db_internal_default_dsn), active unless
 * @p keep: the one derivation every entry point (hl_app_context_init,
 * `hull migrate`) uses. 0, or -1 when the path does not fit. A default with
 * no internal database of its own (network / DuckDB / "$VAR") CLEARS an
 * earlier recorded one and closes an internal connection opened from it
 * (audit 44), so a re-pointed default never keeps the implicit one's. */
int hl_db_internal_configure(HlDbRegistry *reg, const char *app_dir,
                             const char *default_dsn, int keep);
/* The same, given the app's state directory too (NULL = none). */
int hl_db_internal_configure_ex(HlDbRegistry *reg, const char *app_dir,
                                const char *state_dir,
                                const char *default_dsn, int keep);

/* The file the kernel sandbox must let Hull read and write
 * (HlSandboxPolicy.internal_db_path): the active default internal database
 * when it is a file, else NULL. */
const char *hl_db_internal_sandbox_path(const HlDbRegistry *reg);

#endif /* HL_CAP_DB_INTERNAL_H */
