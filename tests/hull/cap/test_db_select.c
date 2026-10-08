/*
 * test_db_select.c - DSN-scheme backend routing (hl_db_backend_select)
 *
 * Flag-aware: asserts the routing for whatever backends this build compiled,
 * plus the reserved-but-uncompiled and unknown-scheme error paths (which are
 * backend-independent). See cap/db_select.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/db_backend.h"
#include <string.h>

/* Scheme-less DSNs (bare path, ":memory:", "file:" URI) and "sqlite://" route
 * to SQLite when it is compiled; otherwise they error (no default backend). */
UTEST(db_select, sqlite_and_scheme_less)
{
    const char *err = NULL;
    const HlDbBackend *b;
#ifdef HL_ENABLE_SQLITE
    b = hl_db_backend_select(":memory:", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite"); ASSERT_FALSE(err);
    b = hl_db_backend_select("./data.db", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
    b = hl_db_backend_select("sqlite://:memory:", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
    /* "file:" URI has no "://" -> scheme-less -> SQLite default. */
    b = hl_db_backend_select("file:data.db?mode=ro", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
    /* Scheme match is case-insensitive. */
    b = hl_db_backend_select("SQLITE://x", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
#else
    b = hl_db_backend_select(":memory:", &err);
    ASSERT_FALSE(b); ASSERT_TRUE(err);
    err = NULL;
    b = hl_db_backend_select("sqlite://x", &err);
    ASSERT_FALSE(b); ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "HL_ENABLE_SQLITE") != NULL);
#endif
}

/* "postgres://" / "postgresql://" route to Postgres when compiled, else point at
 * the composable Postgres feature (not a generic "unknown scheme"). */
UTEST(db_select, postgres_scheme)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("postgres://u@h/db", &err);
#ifdef HL_ENABLE_POSTGRES
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "postgres"); ASSERT_FALSE(err);
    b = hl_db_backend_select("postgresql://u@h/db", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "postgres");
#else
    ASSERT_FALSE(b); ASSERT_TRUE(err);
    ASSERT_TRUE(strstr(err, "feature install postgres") != NULL);
#  if defined(__COSMOPOLITAN__)
    /* A cosmo build cannot load a native feature archive, so `hull feature
     * install` refuses for every feature. The hint must not read as though
     * running it would fix this, and must name a route that works. */
    ASSERT_TRUE(strstr(err, "not available on this build") != NULL);
    ASSERT_TRUE(strstr(err, "native hull") != NULL);
    ASSERT_TRUE(strstr(err, "SQLite") != NULL);
#  endif
#endif
}

/* "mysql://" / "mariadb://" route to the MySQL backend when compiled, else point
 * at the composable MySQL feature. Both schemes share the one backend. */
UTEST(db_select, mysql_scheme)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("mysql://u@h/db", &err);
#ifdef HL_ENABLE_MYSQL
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "mysql"); ASSERT_FALSE(err);
    ASSERT_EQ(b->dialect.identifier_quote, '`');   /* backtick dialect */
    b = hl_db_backend_select("mariadb://u@h/db", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "mysql");
#else
    ASSERT_FALSE(b); ASSERT_TRUE(err);
    ASSERT_TRUE(strstr(err, "feature install mysql") != NULL);
#  if defined(__COSMOPOLITAN__)
    ASSERT_TRUE(strstr(err, "not available on this build") != NULL);
    ASSERT_TRUE(strstr(err, "native hull") != NULL);
#  endif
    b = hl_db_backend_select("mariadb://u@h/db", &err);
    ASSERT_FALSE(b); ASSERT_TRUE(strstr(err, "feature install mysql") != NULL);
#endif
}

/* "duckdb://" routes to the DuckDB backend when compiled, else a build-flag
 * hint (reserved-scheme path, covered below). */
UTEST(db_select, duckdb_scheme)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("duckdb://:memory:", &err);
#ifdef HL_ENABLE_DUCKDB
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "duckdb"); ASSERT_FALSE(err);
    ASSERT_EQ(b->dialect.identifier_quote, '"');
    ASSERT_EQ((int)b->native_tag, (int)HL_DB_NATIVE_DUCKDB);
#else
    ASSERT_FALSE(b); ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "DuckDB") != NULL);
#endif
}

/* Reserved schemes with no backend in this build: a specific hint, never a
 * backend. mysql/mariadb are reserved only when the MySQL backend isn't
 * compiled; duckdb only when the DuckDB backend isn't. */
UTEST(db_select, reserved_schemes)
{
    const char *err = NULL;
#ifndef HL_ENABLE_DUCKDB
    ASSERT_FALSE(hl_db_backend_select("duckdb://x.duckdb", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "DuckDB") != NULL);
    /* The hint points at the composable-feature path, not just a build flag. */
    ASSERT_TRUE(strstr(err, "feature install") != NULL);
    ASSERT_TRUE(strstr(err, "--with=duckdb") != NULL);
    err = NULL;
#endif
#ifndef HL_ENABLE_MYSQL
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("mysql://u@h/db", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "MySQL") != NULL);
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("mariadb://u@h/db", &err));
    ASSERT_TRUE(err);
#endif
}

/* An unrecognized scheme gets a generic hint. */
UTEST(db_select, unknown_scheme)
{
    const char *err = NULL;
    ASSERT_FALSE(hl_db_backend_select("bogus://x", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "unknown") != NULL);
}

/* Feature-composition hook: a STRONG override of the base's weak
 * hl_db_feature_backends (this is exactly how a `hull build --with=<feature>`
 * build injects a composed backend). The synthetic "featuretest" scheme is
 * claimed by no base backend and is not reserved, so it exercises only the
 * feature path. This strong definition displaces db_select.c's weak default for
 * the whole test binary; it is inert for every other scheme, so the tests above
 * are unaffected. */
static const char *const feature_schemes[] = { "featuretest", NULL };
static const HlDbBackend feature_backend = {
    .name    = "featuretest",
    .schemes = feature_schemes,
};
static const HlDbBackend *const FEATURE_TABLE[] = { &feature_backend };
const HlDbBackend *const *hl_db_feature_backends(size_t *count)
{
    if (count) *count = 1;
    return FEATURE_TABLE;
}

UTEST(db_select, feature_backend_composed)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("featuretest://x", &err);
    ASSERT_TRUE(b);
    ASSERT_STREQ(b->name, "featuretest");
    ASSERT_FALSE(err);
    /* Base backends still win + a truly unknown scheme still errors. */
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("bogus://x", &err));
    ASSERT_TRUE(err);
}

/* hl_db_dsn_redact keeps scheme + host only: no user, password (even one
 * containing '@'), database or query parameter reaches a log line (audit 9
 * L4). A scheme-less file path is printed as is. */
UTEST(db_select, dsn_redact)
{
    char b[128];
    hl_db_dsn_redact("postgres://hull:s3cret@db.local:5432/app?sslmode=require",
                     b, sizeof b);
    ASSERT_STREQ("postgres://db.local:5432", b);
    hl_db_dsn_redact("mysql://u:p@ss@10.0.0.1/db", b, sizeof b);
    ASSERT_STREQ("mysql://10.0.0.1", b);
    hl_db_dsn_redact("postgres://db.local/app?password=s3cret", b, sizeof b);
    ASSERT_STREQ("postgres://db.local", b);
    hl_db_dsn_redact("data/app.db", b, sizeof b);
    ASSERT_STREQ("data/app.db", b);
    hl_db_dsn_redact(NULL, b, sizeof b);
    ASSERT_STREQ("", b);
}

/* Audit 10: a password holding an unencoded '#', '/' or '?' ended the
 * "authority" inside it, and its first part was printed as the host. When an
 * '@' follows the authority the DSN is ambiguous, so only the scheme is
 * printed - never any part of the password. */
UTEST(db_select, dsn_redact_ambiguous_userinfo)
{
    char b[128];
    static const char *const dsns[] = {
        "postgres://hull:s3c#ret@db.local/app",
        "postgres://hull:s3c/ret@db.local/app",
        "mysql://u:pa?ss@10.0.0.1/db",
        "postgres://db.local/app?password=x@y",
    };
    for (size_t i = 0; i < sizeof dsns / sizeof dsns[0]; i++) {
        hl_db_dsn_redact(dsns[i], b, sizeof b);
        EXPECT_TRUE(strcmp(b, "postgres://(redacted)") == 0 ||
                    strcmp(b, "mysql://(redacted)") == 0);
        EXPECT_TRUE(strstr(b, "://(redacted)") != NULL);
    }
    hl_db_dsn_redact("postgres://h:p@db.local/app#frag", b, sizeof b);
    ASSERT_STREQ("postgres://db.local", b);
}

UTEST_MAIN()
