/*
 * cap/db_duckdb.h - DuckDB backend registration
 *
 * Exposes the DuckDB backend const for the DSN-scheme registry (db_select.c).
 * The abstract interface (db_backend.h) stays free of concrete-backend symbols.
 * DuckDB is a side-loaded, statically-linked embedded OLAP backend; the archive
 * set is fetched via `make fetch-duckdb` and the flavor built with
 * HL_ENABLE_DUCKDB=1. See docs/duckdb_backend_design.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_DUCKDB_H
#define HL_CAP_DB_DUCKDB_H

#include "hull/cap/db_backend.h"

#ifdef HL_ENABLE_DUCKDB
extern const HlDbBackend hl_db_backend_duckdb;

/* Install the bounded file-access policy (design §3.2 mode B): @p dirs is an
 * array of @p ndirs absolute directory paths (from the manifest's fs.read /
 * fs.write, resolved by serve.c into the sealed policy arena) that DuckDB SQL
 * (read_csv / read_parquet / COPY) may touch. Call once at boot, before any
 * connection opens; the array + strings must outlive the process (sealed). An
 * empty policy (ndirs <= 0) leaves DuckDB in full lockdown (mode A). */
void hl_db_duckdb_set_fs_policy(const char *const *dirs, int ndirs);

/* hl_db_duckdb_set_fs_policy, plus @p paths (@p npaths absolute FILE paths,
 * DuckDB's allowed_paths) and @p reserved_dir, the canonical
 * `<app root>/.hull` that app SQL may not reach (audit convergence §7.2;
 * NULL: unknown, only the name check applies). Any granted directory or
 * file puts DuckDB in mode B. Same lifetime rules: sealed, process life. */
void hl_db_duckdb_set_fs_policy_ex(const char *const *dirs, int ndirs,
                                   const char *const *paths, int npaths,
                                   const char *reserved_dir);

/* A grant of the whole app root would hand DuckDB the reserved `.hull`
 * directory (Hull's internal database). DuckDB's allowed_directories has no
 * exclusion, so the root is granted as its top-level entries instead:
 * hl_db_duckdb_split_root calls @p fn once per entry of @p root (absolute)
 * with its absolute path and whether it is a directory, leaving out `.hull`
 * in any letter case (and by inode), symbolic links, and anything neither a
 * directory nor a regular file. Returns the count, or -1 (unreadable root,
 * more than HL_DUCK_ROOT_ENTRIES_MAX entries, or @p fn returned non-zero).
 * DuckDB's lists cannot change once the configuration is locked, so an
 * entry made at the root later is not reachable from SQL. */
#define HL_DUCK_ROOT_ENTRIES_MAX 1024
typedef int (*HlDuckRootEntryFn)(void *ud, const char *abs_path, int is_dir);
int hl_db_duckdb_split_root(const char *root, HlDuckRootEntryFn fn, void *ud);
#endif

#endif /* HL_CAP_DB_DUCKDB_H */
