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
};

/* Schemes Hull recognizes but may not have compiled: a helpful, specific error
 * instead of a bare "unknown scheme". Reserving duckdb/mysql/mariadb keeps the
 * routing forward-compatible; each resolves to a real backend the moment one is
 * compiled in (it would then match a BACKENDS[] entry before reaching here). */
static const struct { const char *scheme; const char *msg; } RESERVED[] = {
    { "postgres",   "postgres:// requires a build with HL_ENABLE_POSTGRES" },
    { "postgresql", "postgresql:// requires a build with HL_ENABLE_POSTGRES" },
    { "sqlite",     "sqlite:// requires a build with HL_ENABLE_SQLITE" },
    { "file",       "file: URIs require a build with HL_ENABLE_SQLITE" },
    { "duckdb",     "the DuckDB backend (duckdb://) is not available in this build" },
    { "mysql",      "the MySQL/MariaDB backend (mysql://) is not available in this build" },
    { "mariadb",    "the MySQL/MariaDB backend (mariadb://) is not available in this build" },
};

static int scheme_in(const char *const *list, const char *scheme)
{
    if (!list) return 0;
    for (; *list; list++)
        if (strcmp(*list, scheme) == 0) return 1;
    return 0;
}

/* Weak default: a base build has no composed feature backends. A feature build
 * (`hull build --with=<feature>`) links a generated STRONG override — a const
 * table referencing each composed feature's backend — which the linker prefers
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
        /* Explicit "<scheme>://": route to the backend that claims it. First the
         * base backends compiled into this binary. */
        for (size_t i = 0; i < sizeof BACKENDS / sizeof BACKENDS[0]; i++)
            if (scheme_in(BACKENDS[i]->schemes, scheme))
                return BACKENDS[i];
        /* Then feature backends composed in at build time (empty in a base
         * build; a generated registry fills this in a `--with=<feature>` build). */
        size_t fcount = 0;
        const HlDbBackend *const *feats = hl_db_feature_backends(&fcount);
        for (size_t i = 0; i < fcount; i++)
            if (feats && feats[i] && scheme_in(feats[i]->schemes, scheme))
                return feats[i];
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

    /* No scheme: a bare path, ":memory:", or an sqlite "file:" URI -> SQLite. */
#ifdef HL_ENABLE_SQLITE
    return &hl_db_backend_sqlite;
#else
    if (err)
        *err = "this hull has no SQLite backend for a scheme-less DSN; use an "
               "explicit scheme (e.g. postgres:// or duckdb://)";
    return NULL;
#endif
}

/* hl_cap_db_check_namespace (the backend-agnostic _hull_* guard) moved to
 * cap/db_common.c (§2.7); it was never part of DSN selection. */

#endif /* HL_ENABLE_DB */
