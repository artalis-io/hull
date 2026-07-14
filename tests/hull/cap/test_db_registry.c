/*
 * test_db_registry.c: Named connection registry (§1 Phase 5b)
 *
 * Covers lazy open + caching, manifest DSN resolution (literal and
 * { dsn_env }), the seeded "default" connection, and error paths.
 * Uses SQLite :memory: DSNs so no external server is needed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/db_backend.h"
#include "hull/manifest.h"

#include <stdlib.h>
#include <string.h>

/* Seed a pre-opened SQLite connection as "default", then resolve it. */
UTEST(db_registry, seed_and_get_default)
{
    HlDbHandle def = {0};
    def.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(0, hl_db_backend_sqlite.open(&def.ctx, ":memory:", NULL));

    HlDbRegistry *reg = hl_db_registry_create(NULL, NULL, NULL);
    ASSERT_TRUE(reg != NULL);
    ASSERT_EQ(0, hl_db_registry_seed(reg, "default", &def));

    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "default", &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(err == NULL);
    /* Same underlying connection as the seed (value-copied ctx). */
    ASSERT_EQ(h->ctx, def.ctx);
    ASSERT_TRUE(hl_db_exec(h, "CREATE TABLE t (x INTEGER)", NULL, 0) >= 0);

    hl_db_registry_destroy(reg);           /* must NOT close the seeded conn */
    def.backend->close(&def);              /* caller still owns it */
}

/* A manifest literal-DSN entry opens lazily and caches. */
UTEST(db_registry, manifest_lazy_open_and_cache)
{
    HlManifest m = {0};
    m.databases[0].name = "cache";
    m.databases[0].dsn = ":memory:";
    m.databases[0].dsn_is_env = 0;
    m.databases_count = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    ASSERT_TRUE(reg != NULL);

    const char *err = NULL;
    HlDbHandle *h1 = hl_db_registry_get(reg, "cache", &err);
    ASSERT_TRUE(h1 != NULL);
    ASSERT_TRUE(err == NULL);
    ASSERT_STREQ(h1->backend->name, "sqlite");
    ASSERT_TRUE(hl_db_exec(h1, "CREATE TABLE c (x INTEGER)", NULL, 0) >= 0);

    /* Second get returns the SAME cached handle. */
    HlDbHandle *h2 = hl_db_registry_get(reg, "cache", &err);
    ASSERT_EQ(h1, h2);

    hl_db_registry_destroy(reg);           /* closes the owned "cache" conn */
}

/* A { dsn_env } entry reads the DSN from the environment at open time. */
UTEST(db_registry, dsn_env_resolves)
{
    setenv("HULL_TEST_DB_DSN", ":memory:", 1);

    HlManifest m = {0};
    m.databases[0].name = "primary";
    m.databases[0].dsn = "HULL_TEST_DB_DSN";   /* env var NAME */
    m.databases[0].dsn_is_env = 1;
    m.databases_count = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "primary", &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(err == NULL);

    hl_db_registry_destroy(reg);
    unsetenv("HULL_TEST_DB_DSN");
}

/* An unset dsn_env variable is a clear error, not a crash. */
UTEST(db_registry, dsn_env_unset_errors)
{
    unsetenv("HULL_TEST_MISSING_DSN");

    HlManifest m = {0};
    m.databases[0].name = "primary";
    m.databases[0].dsn = "HULL_TEST_MISSING_DSN";
    m.databases[0].dsn_is_env = 1;
    m.databases_count = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "primary", &err);
    ASSERT_TRUE(h == NULL);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(err, "unset") != NULL);

    hl_db_registry_destroy(reg);
}

/* An undeclared name fails closed. */
UTEST(db_registry, unknown_name_errors)
{
    HlManifest m = {0};   /* no databases */
    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "nope", &err);
    ASSERT_TRUE(h == NULL);
    ASSERT_TRUE(err != NULL);
    hl_db_registry_destroy(reg);
}

/* "default" falls back to the create-time default_dsn (the -d flag), and the
 * fast accessor returns the same open handle. */
UTEST(db_registry, default_dsn_and_accessor)
{
    HlDbRegistry *reg = hl_db_registry_create(NULL, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    /* Before first get, the accessor sees nothing open. */
    ASSERT_TRUE(hl_db_registry_default(reg) == NULL);

    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "default", &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(err == NULL);
    ASSERT_STREQ(h->backend->name, "sqlite");
    /* The accessor now returns the cached default (same pointer). */
    ASSERT_EQ(hl_db_registry_default(reg), h);

    hl_db_registry_destroy(reg);   /* owns + closes the default */
}

UTEST_MAIN()
