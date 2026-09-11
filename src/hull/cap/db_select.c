/*
 * cap/db_select.c: Database backend selection by DSN scheme
 *
 * Chooses the HlDbBackend for a DSN so the three call sites that open a
 * database (app_context, tool_orchestration, migrate) stay backend-neutral.
 * Deliberately free of any backend header (no sqlite3.h) so it compiles in
 * every flag combination the umbrella permits: SQLite-only, PostgreSQL-only,
 * or both.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"     /* hl_db_backend_sqlite (BACKENDS[]) */
#include "hull/cap/db_postgres.h"   /* hl_db_backend_postgres (BACKENDS[]) */
#include "hull/cap/db_mysql.h"      /* hl_db_backend_mysql (BACKENDS[]) */
#include "hull/cap/db_duckdb.h"     /* hl_db_backend_duckdb (BACKENDS[]) */
#include "hull/cap/db.h"   /* HlDbError codes + check_namespace decl */
#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Extract the DSN scheme (the text before "://") lowercased into @p buf.
 * Returns the length, or 0 when the DSN has no "://" scheme: a bare path, a
 * ":memory:", a single-colon "file:" URI, or a Windows "C:\path" all correctly
 * report no scheme (none contain "://"). An empty or over-long scheme also
 * reports 0 (treated as scheme-less). */
static size_t dsn_scheme(const char *dsn, char *buf, size_t bufsz)
{
    if (!dsn) return 0;
    const char *sep = strstr(dsn, "://");
    if (!sep) return 0;
    size_t n = (size_t)(sep - dsn);
    if (n == 0 || n >= bufsz) return 0;
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)dsn[i]);
    buf[n] = '\0';
    return n;
}

/* Compiled-in backends, in match order. Adding a backend = one line here plus
 * its `.schemes` declaration in the backend TU. This is the sole registration
 * point; the umbrella guarantees at least one entry (HL_ENABLE_DB iff SQLite or
 * Postgres). */
static const HlDbBackend *const BACKENDS[] = {
#ifdef HL_ENABLE_SQLITE
    &hl_db_backend_sqlite,
#endif
#ifdef HL_ENABLE_POSTGRES
    &hl_db_backend_postgres,
#endif
#ifdef HL_ENABLE_MYSQL
    &hl_db_backend_mysql,
#endif
#ifdef HL_ENABLE_DUCKDB
    &hl_db_backend_duckdb,
#endif
#if !defined(HL_ENABLE_SQLITE) && !defined(HL_ENABLE_POSTGRES) && \
    !defined(HL_ENABLE_MYSQL) && !defined(HL_ENABLE_DUCKDB)
    /* No backend compiled into the base (HL_SQLITE_FEATURE: SQLite composed as a
     * feature, docs/sqlite_feature.md). A lone NULL keeps the array non-empty
     * for ISO C (no zero-length array under -Wpedantic); backend_for_scheme
     * skips NULL entries and the real backend arrives via hl_db_feature_backends. */
    NULL,
#endif
};

/* Schemes Hull recognizes but may not have compiled: a helpful, specific error
 * instead of a bare "unknown scheme". Reserving duckdb/mysql/mariadb keeps the
 * routing forward-compatible; each resolves to a real backend the moment one is
 * compiled in (it would then match a BACKENDS[] entry before reaching here).
 * DuckDB additionally resolves when composed as a build feature: the feature
 * loop above (hl_db_feature_backends) matches duckdb:// before this table, so
 * this hint fires only for a base app with neither the compile flag nor the
 * feature, and points at the feature path. See docs/features_and_flavors.md.
 *
 * The feature route does not exist on a cosmo build. Features ship as native
 * static archives and a fat APE cannot force-load one, so FEATURES[] carries no
 * cosmo column and `hull feature install` refuses with "not published for
 * cosmo". Pointing a cosmo user at that command sends them to something that
 * cannot succeed, so these hints name the routes that do work there: a native
 * hull, a source build with the compile flag, or SQLite. */
#if defined(__COSMOPOLITAN__)
static const char HINT_PG[] =
    "postgres:// needs the Postgres feature, which is not available on this "
    "build. Features are native static "
    "archives and a fat APE cannot load one, so they are not published for "
    "cosmo. Use a native hull (linux-x86_64, linux-aarch64, darwin-arm64) "
    "with 'hull feature install postgres' then 'hull build --with=postgres', "
    "or build from source with HL_ENABLE_POSTGRES=1. A SQLite DSN works on "
    "this build.";
static const char HINT_MY[] =
    "mysql:// needs the MySQL feature, which is not available on this build. "
    "Features are native static "
    "archives and a fat APE cannot load one, so they are not published for "
    "cosmo. Use a native hull (linux-x86_64, linux-aarch64, darwin-arm64) "
    "with 'hull feature install mysql' then 'hull build --with=mysql', or "
    "build from source with HL_ENABLE_MYSQL=1. A SQLite DSN works on this "
    "build.";
static const char HINT_DD[] =
    "duckdb:// needs the DuckDB feature, which is not available on this "
    "build. Features are native static "
    "archives and a fat APE cannot load one, so they are not published for "
    "cosmo. Use a native hull (linux-x86_64, linux-aarch64, darwin-arm64) "
    "with 'hull feature install duckdb' then 'hull build --with=duckdb'. "
    "A SQLite DSN works on this build.";
#else
static const char HINT_PG[] =
    "postgres:// needs the Postgres feature: run 'hull feature install "
    "postgres', then build the app with 'hull build --with=postgres'";
static const char HINT_MY[] =
    "mysql:// needs the MySQL feature: run 'hull feature install mysql', "
    "then build the app with 'hull build --with=mysql'";
static const char HINT_DD[] =
    "duckdb:// needs the DuckDB feature: run 'hull feature install duckdb', "
    "then build the app with 'hull build --with=duckdb'";
#endif

static const struct { const char *scheme; const char *msg; } RESERVED[] = {
    { "postgres",   HINT_PG },
    { "postgresql", HINT_PG },
    { "sqlite",     "sqlite:// requires a build with HL_ENABLE_SQLITE" },
    { "file",       "file: URIs require a build with HL_ENABLE_SQLITE" },
    { "duckdb",     HINT_DD },
    { "mysql",      HINT_MY },
    { "mariadb",    HINT_MY },
};

static int scheme_in(const char *const *list, const char *scheme)
{
    if (!list) return 0;
    for (; *list; list++)
        if (strcmp(*list, scheme) == 0) return 1;
    return 0;
}

/* The scheme a scheme-less DSN (a bare path, ":memory:", a "file:" URI) routes
 * to. This is the DEFAULT backend. Today the base SQLite backend claims it; once
 * SQLite becomes a composable feature (docs/sqlite_feature.md) it fills the same
 * slot via hl_db_feature_backends, so resolving the default by scheme keeps this
 * correct without the selector naming hl_db_backend_sqlite directly. */
#define HL_DB_DEFAULT_SCHEME "sqlite"

/* Resolve the backend claiming @p scheme among the base backends compiled into
 * this binary, then any feature backends composed in at build time. NULL if
 * none. Shared by the explicit-scheme path and the scheme-less default. */
static const HlDbBackend *backend_for_scheme(const char *scheme)
{
    for (size_t i = 0; i < sizeof BACKENDS / sizeof BACKENDS[0]; i++)
        if (BACKENDS[i] && scheme_in(BACKENDS[i]->schemes, scheme))
            return BACKENDS[i];
    size_t fcount = 0;
    const HlDbBackend *const *feats = hl_db_feature_backends(&fcount);
    for (size_t i = 0; i < fcount; i++)
        if (feats && feats[i] && scheme_in(feats[i]->schemes, scheme))
            return feats[i];
    return NULL;
}

/* Weak default: a base build has no composed feature backends. A feature build
 * (`hull build --with=<feature>`) links a generated STRONG override - a const
 * table referencing each composed feature's backend - which the linker prefers
 * over this. See db_backend.h + docs/features_and_flavors.md §3.2. Kept in this
 * TU (which is always linked) so the symbol always resolves; the override, being
 * a direct object rather than an archive member, displaces it. */
__attribute__((weak))
const HlDbBackend *const *hl_db_feature_backends(size_t *count)
{
    if (count) *count = 0;
    return NULL;
}

const HlDbBackend *hl_db_backend_select(const char *dsn, const char **err)
{
    if (err) *err = NULL;

    char scheme[24];
    size_t slen = dsn_scheme(dsn, scheme, sizeof scheme);

    if (slen > 0) {
        /* Explicit "<scheme>://": route to the backend that claims it (base
         * backends first, then feature backends composed at build time). */
        const HlDbBackend *be = backend_for_scheme(scheme);
        if (be) return be;
        /* A scheme Hull knows but this build lacks: specific hint. */
        for (size_t i = 0; i < sizeof RESERVED / sizeof RESERVED[0]; i++)
            if (strcmp(RESERVED[i].scheme, scheme) == 0) {
                if (err) *err = RESERVED[i].msg;
                return NULL;
            }
        if (err)
            *err = "unknown database DSN scheme (expected postgres://, "
                   "sqlite://, duckdb://, mysql://, or a file path)";
        return NULL;
    }

    /* No scheme: a bare path, ":memory:", or an sqlite "file:" URI -> the
     * default backend, resolved by scheme rather than by a hardcoded symbol so
     * this stays correct when SQLite moves behind hl_db_feature_backends. Today
     * the base SQLite backend claims HL_DB_DEFAULT_SCHEME. */
    const HlDbBackend *def = backend_for_scheme(HL_DB_DEFAULT_SCHEME);
    if (def) return def;
    if (err)
        *err = "this hull has no default (SQLite) backend for a scheme-less "
               "DSN; use an explicit scheme (e.g. postgres:// or duckdb://)";
    return NULL;
}

/* hl_cap_db_check_namespace (the backend-agnostic _hull_* guard) moved to
 * cap/db_common.c (§2.7); it was never part of DSN selection. */

#endif /* HL_ENABLE_DB */
