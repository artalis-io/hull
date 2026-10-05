/*
 * test_db_dynamic.c: db.open validation + caller-owned open (hl_db_dynamic_*)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/db_dynamic.h"
#include "hull/cap/db_backend.h"
#include "hull/manifest.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/utils/alloc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static HlManifestDbDynamic policy_sqlite(void)
{
    HlManifestDbDynamic p = {0};
    p.declared = 1;
    p.schemes[0] = "sqlite";
    p.scheme_count = 1;
    return p;
}

/* No policy (or an undeclared one) fails closed. */
UTEST(db_dynamic, no_policy_denied)
{
    const char *err = NULL;
    ASSERT_TRUE(hl_db_dynamic_open(":memory:", NULL, NULL, &err) == NULL);
    ASSERT_TRUE(err != NULL);

    HlManifestDbDynamic p = {0};   /* declared == 0 */
    err = NULL;
    ASSERT_TRUE(hl_db_dynamic_open(":memory:", &p, NULL, &err) == NULL);
    ASSERT_TRUE(err != NULL);
}

/* An allowlisted sqlite :memory: DSN opens; the count tracks open/close. */
UTEST(db_dynamic, sqlite_memory_open_close)
{
    HlManifestDbDynamic p = policy_sqlite();
    const char *err = NULL;
    int before = hl_db_dynamic_open_count();

    HlDbHandle *h = hl_db_dynamic_open(":memory:", &p, NULL, &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(err == NULL);
    ASSERT_STREQ(h->backend->name, "sqlite");
    ASSERT_EQ(hl_db_dynamic_open_count(), before + 1);
    ASSERT_TRUE(hl_db_exec(h, "CREATE TABLE t(x INTEGER)", NULL, 0) >= 0);

    hl_db_dynamic_close(h);
    ASSERT_EQ(hl_db_dynamic_open_count(), before);
}

/* A db.open handle is shared like a registry connection, so the stale guard
 * and the wait refusal cover it (audit 6 L7); its id stays live until close,
 * which is how a db.async worker learns to drop the connection it keeps. */
UTEST(db_dynamic, guard_and_ids_cover_open_handles)
{
    HlManifestDbDynamic p = policy_sqlite();
    const char *err = NULL;
    HlDbHandle *h = hl_db_dynamic_open(":memory:", &p, NULL, &err);
    ASSERT_TRUE(h != NULL);
    uint64_t id = hl_db_dynamic_id(h);
    EXPECT_NE(0u, id);
    EXPECT_EQ(1, hl_db_dynamic_id_live(id));
    EXPECT_EQ(0u, hl_db_dynamic_id(NULL));

    EXPECT_EQ(0, hl_db_dynamic_in_txn());
    ASSERT_EQ(0, hl_db_begin(h));
    EXPECT_EQ(1, hl_db_dynamic_in_txn());
    hl_db_dynamic_guard_stale_txns();
    EXPECT_EQ(0, hl_db_in_txn(h));
    EXPECT_EQ(0, hl_db_dynamic_in_txn());

    hl_db_dynamic_close(h);
    EXPECT_EQ(0, hl_db_dynamic_id_live(id));

    /* Ids are not reused: a new handle in the same slot gets a new one. */
    HlDbHandle *h2 = hl_db_dynamic_open(":memory:", &p, NULL, &err);
    ASSERT_TRUE(h2 != NULL);
    EXPECT_NE(id, hl_db_dynamic_id(h2));
    EXPECT_EQ(0, hl_db_dynamic_id_live(id));
    hl_db_dynamic_close(h2);
}

/* A DSN whose scheme isn't in databases.dynamic.schemes is rejected. */
UTEST(db_dynamic, scheme_not_allowed)
{
    HlManifestDbDynamic p = {0};
    p.declared = 1;
    p.schemes[0] = "postgres";
    p.scheme_count = 1;

    const char *err = NULL;   /* ":memory:" implies "sqlite", not in {postgres} */
    ASSERT_TRUE(hl_db_dynamic_open(":memory:", &p, NULL, &err) == NULL);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(err, "scheme") != NULL);
}

/* A file DSN outside the fs sandbox base_dir is rejected. */
UTEST(db_dynamic, file_fs_gate)
{
    HlManifestDbDynamic p = policy_sqlite();
    HlFsConfig fs = { .base_dir = "/tmp", .base_len = 4 };
    const char *err = NULL;

    HlDbHandle *h = hl_db_dynamic_open("/etc/hosts", &p, &fs, &err);
    ASSERT_TRUE(h == NULL);
    ASSERT_TRUE(err != NULL);
}

/* A file DSN needs an fs.write grant for its path, not just containment, and
 * opens under the app directory (not the process's working directory): the
 * DSN reported back - which conn.async reopens - is the absolute one. */
UTEST(db_dynamic, file_dsn_needs_a_write_grant_and_opens_under_the_app_dir)
{
    char cwd[1024];
    ASSERT_TRUE(getcwd(cwd, sizeof cwd) != NULL);
    const char *wr[] = { "dyn_grant_probe.db" };
    HlAllocator alloc = {0};
    HlFsPolicy pol = HL_FS_POLICY_INIT;
    const char *perr = NULL;
    ASSERT_EQ(0, hl_fs_policy_compile_manifest(cwd, &alloc, NULL, 0, wr, 1, &pol, &perr));
    HlFsConfig fs = { .base_dir = cwd, .base_len = strlen(cwd), .policy = &pol };
    HlManifestDbDynamic p = policy_sqlite();
    const char *err = NULL;
    char opened[HL_DB_DYNAMIC_DSN_MAX];

    remove("dyn_nogrant_probe.db");
    EXPECT_TRUE(hl_db_dynamic_open_ex("dyn_nogrant_probe.db", &p, &fs,
                                      opened, sizeof opened, &err) == NULL);
    EXPECT_TRUE(err != NULL && strstr(err, "fs.write") != NULL);
    FILE *f = fopen("dyn_nogrant_probe.db", "rb");
    EXPECT_TRUE(f == NULL);
    if (f) fclose(f);

    HlDbHandle *h = hl_db_dynamic_open_ex("dyn_grant_probe.db", &p, &fs,
                                          opened, sizeof opened, &err);
    ASSERT_TRUE(h != NULL);
    char want[1100];
    snprintf(want, sizeof want, "%s/dyn_grant_probe.db", cwd);
    EXPECT_STREQ(want, opened);
    hl_db_dynamic_close(h);
    remove("dyn_grant_probe.db");
    remove("dyn_grant_probe.db-wal");
    remove("dyn_grant_probe.db-shm");
    hl_fs_policy_free(&pol);
}

/* The process-wide concurrent-open cap trips (and does not overflow). */
UTEST(db_dynamic, concurrent_cap)
{
    HlManifestDbDynamic p = policy_sqlite();
    HlDbHandle *hs[32];
    int n = 0;
    const char *err = NULL;

    while (n < 32) {
        err = NULL;
        HlDbHandle *h = hl_db_dynamic_open(":memory:", &p, NULL, &err);
        if (!h) {
            ASSERT_TRUE(err != NULL);
            ASSERT_TRUE(strstr(err, "too many") != NULL);
            break;
        }
        hs[n++] = h;
    }
    ASSERT_TRUE(n <= 16);   /* HL_DB_DYNAMIC_MAX */
    for (int i = 0; i < n; i++)
        hl_db_dynamic_close(hs[i]);
}

#ifdef HL_ENABLE_POSTGRES
/* Network backend: the host is gated by databases.dynamic.hosts (CIDR here, so
 * no DNS). A denied host is rejected before any connect; an allowed host passes
 * validation and only then fails to connect (port 1). */
UTEST(db_dynamic, network_host_gate)
{
    HlManifestDbDynamic p = {0};
    p.declared = 1;
    p.schemes[0] = "postgres"; p.scheme_count = 1;
    p.hosts[0] = "127.0.0.0/8"; p.host_count = 1;

    const char *err = NULL;
    ASSERT_TRUE(hl_db_dynamic_open("postgres://u@10.9.9.9:1/db", &p, NULL, &err) == NULL);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(err, "host") != NULL);

    err = NULL;   /* 127.0.0.1 is in the CIDR -> past the host gate, connect refused */
    ASSERT_TRUE(hl_db_dynamic_open("postgres://u@127.0.0.1:1/db?sslmode=disable",
                                   &p, NULL, &err) == NULL);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(err, "host") == NULL);
}
#endif

UTEST_MAIN()
