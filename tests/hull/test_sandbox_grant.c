/*
 * test_sandbox_grant.c - what the kernel grant for a manifest fs entry
 * covers (hl_sandbox_resolve_grant, src/hull/sandbox.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/sandbox.h"
#include "test_tmpdir.h"
#include "log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

static void cleanup(const char *app)
{
    char p[HL_TEST_PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/out.txt", app);   unlink(p);
    snprintf(p, sizeof p, "%s/sub/res.json", app); unlink(p);
    snprintf(p, sizeof p, "%s/sub", app);       rmdir(p);
    snprintf(p, sizeof p, "%s/data", app);      rmdir(p);
    rmdir(app);
}

/* A file write grant directly in the app directory must not open the whole
 * app directory (app.lua, migrations/, package.sig) once the file exists:
 * the grant is the file alone, written in place by the cap layer. */
UTEST(sandbox_grant, top_level_write_is_the_file_once_it_exists)
{
    char app[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(app, sizeof app, "hull_grant") != NULL);
    char real_app[PATH_MAX];
    ASSERT_TRUE(realpath(app, real_app) != NULL);

    char out[PATH_MAX], want[PATH_MAX + 16];

    /* Absent: nothing narrower than the app directory exists to grant. */
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "out.txt", out, sizeof out, 1), 0);
    EXPECT_STREQ(out, real_app);

    /* Present: the file alone. */
    snprintf(want, sizeof want, "%s/out.txt", app);
    touch(want);
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "out.txt", out, sizeof out, 1), 0);
    snprintf(want, sizeof want, "%s/out.txt", real_app);
    EXPECT_STREQ(out, want);

    /* A read grant of an existing file is the file too. */
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "out.txt", out, sizeof out, 0), 0);
    EXPECT_STREQ(out, want);

    cleanup(app);
}

/* In a subdirectory the grant stays the parent (round-5 M5): the temp beside
 * the target must be creatable, and only that subdirectory opens. */
UTEST(sandbox_grant, nested_write_is_its_parent)
{
    char app[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(app, sizeof app, "hull_grant") != NULL);
    char real_app[PATH_MAX];
    ASSERT_TRUE(realpath(app, real_app) != NULL);

    char p[PATH_MAX + 16], out[PATH_MAX], want[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/sub", app);
    ASSERT_EQ(mkdir(p, 0755), 0);
    snprintf(p, sizeof p, "%s/sub/res.json", app);
    touch(p);

    ASSERT_EQ(hl_sandbox_resolve_grant(app, "sub/res.json", out, sizeof out, 1), 0);
    snprintf(want, sizeof want, "%s/sub", real_app);
    EXPECT_STREQ(out, want);

    cleanup(app);
}

/* Round 7 (L2): the grant is classified on the spelling the cap layer
 * compiles, so "./out.txt" is the top-level file "out.txt". It joined to
 * "app/./out.txt", was not seen as top-level, and its parent "app/." - the
 * app directory itself after realpath - was granted, with no warning. */
UTEST(sandbox_grant, dot_spellings_are_the_same_grant)
{
    char app[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(app, sizeof app, "hull_grant") != NULL);
    char real_app[PATH_MAX];
    ASSERT_TRUE(realpath(app, real_app) != NULL);

    char p[PATH_MAX + 16], out[PATH_MAX], want[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/out.txt", app);
    touch(p);
    snprintf(want, sizeof want, "%s/out.txt", real_app);
    static const char *const spell[] = {
        "./out.txt", ".//out.txt", "././out.txt", "out.txt", NULL
    };
    for (int i = 0; spell[i]; i++) {
        ASSERT_EQ(hl_sandbox_resolve_grant(app, spell[i], out, sizeof out, 1), 0);
        EXPECT_STREQ(out, want);
    }

    snprintf(p, sizeof p, "%s/sub", app);
    ASSERT_EQ(mkdir(p, 0755), 0);
    snprintf(p, sizeof p, "%s/sub/res.json", app);
    touch(p);
    snprintf(want, sizeof want, "%s/sub", real_app);
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "./sub/./res.json", out, sizeof out, 1), 0);
    EXPECT_STREQ(out, want);

    /* "." / "./" is the cap layer's base-root grant: the app directory, as
     * asked. */
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "./", out, sizeof out, 1), 0);
    EXPECT_STREQ(out, real_app);

    cleanup(app);
}

/* An existing directory named without the trailing slash is that directory
 * (the cap layer compiles it to its subtree), not its parent: top-level
 * "data" used to open the whole app directory, under a warning that called
 * it a file. */
UTEST(sandbox_grant, existing_directory_without_slash_is_itself)
{
    char app[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(app, sizeof app, "hull_grant") != NULL);
    char real_app[PATH_MAX];
    ASSERT_TRUE(realpath(app, real_app) != NULL);

    char p[PATH_MAX + 16], out[PATH_MAX], want[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/data", app);
    ASSERT_EQ(mkdir(p, 0755), 0);
    snprintf(want, sizeof want, "%s/data", real_app);
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "data", out, sizeof out, 1), 0);
    EXPECT_STREQ(out, want);
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "./data", out, sizeof out, 1), 0);
    EXPECT_STREQ(out, want);
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "data", out, sizeof out, 0), 0);
    EXPECT_STREQ(out, want);

    cleanup(app);
}

/* A top-level write glob has only the app directory as its literal part, so
 * the kernel grant is all of it: said, as for a top-level file that does not
 * exist yet (audit 9). A nested glob, or a read glob, is not. */
static int g_glob_warns;
static void count_glob_warn(log_Event *ev)
{
    if (ev->level == LOG_WARN && ev->fmt && strstr(ev->fmt, "is a pattern"))
        g_glob_warns++;
}

UTEST(sandbox_grant, top_level_write_glob_is_warned)
{
    static int registered;
    if (!registered) {
        ASSERT_EQ(log_add_callback(count_glob_warn, NULL, LOG_WARN), 0);
        registered = 1;
    }
    char app[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(app, sizeof app, "hull_grant") != NULL);
    char real_app[PATH_MAX];
    ASSERT_TRUE(realpath(app, real_app) != NULL);
    char out[PATH_MAX];

    g_glob_warns = 0;
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "*.log", out, sizeof out, 1), 0);
    EXPECT_STREQ(out, real_app);
    EXPECT_EQ(g_glob_warns, 1);
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "./*.log", out, sizeof out, 1), 0);
    EXPECT_EQ(g_glob_warns, 2);

    ASSERT_EQ(hl_sandbox_resolve_grant(app, "*.log", out, sizeof out, 0), 0);
    EXPECT_EQ(g_glob_warns, 2);                /* a read glob */
    char p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/data", app);
    ASSERT_EQ(mkdir(p, 0755), 0);
    ASSERT_EQ(hl_sandbox_resolve_grant(app, "data/*.log", out, sizeof out, 1), 0);
    EXPECT_EQ(g_glob_warns, 2);                /* nested: its directory */

    cleanup(app);
}

UTEST(sandbox_grant, refuses_escapes)
{
    char out[PATH_MAX];
    EXPECT_EQ(hl_sandbox_resolve_grant("/srv/app", "/etc/passwd", out, sizeof out, 1), -1);
    EXPECT_EQ(hl_sandbox_resolve_grant("/srv/app", "../x", out, sizeof out, 1), -1);
    EXPECT_EQ(hl_sandbox_resolve_grant("/srv/app", "", out, sizeof out, 0), -1);
}

/* Audit 10: the app.main runner handed the raw -d DSN to the sandbox, so a
 * network DSN - password included - was unveiled and logged as a path. Both
 * entry points map it to the file it names, or NULL. */
UTEST(sandbox_grant, db_path_is_the_file_never_a_network_dsn)
{
    EXPECT_TRUE(hl_sandbox_db_path("postgres://u:s3cret@db/app") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("MySQL://u:s3cret@db/app") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("mariadb://u:p@db/app") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("postgresql://db/app") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("sqlite://:memory:") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path(NULL) == NULL);
    EXPECT_STREQ("/var/app.db", hl_sandbox_db_path("sqlite:///var/app.db"));
    EXPECT_STREQ("data.db", hl_sandbox_db_path("data.db"));
}

/* Audit 11: only the file schemes yield a path. An unknown or mistyped network
 * scheme returned the text after "://" - user, password and all. */
UTEST(sandbox_grant, db_path_only_for_file_schemes)
{
    EXPECT_TRUE(hl_sandbox_db_path("pg://u:s3cret@h/app") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("postgress://u:s3cret@h/app") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("redis://:s3cret@h:6379/0") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("valkeys://u:s3cret@h") == NULL);
    EXPECT_TRUE(hl_sandbox_db_path("duckdb://:memory:") == NULL);
    EXPECT_STREQ("/srv/a.duckdb", hl_sandbox_db_path("duckdb:///srv/a.duckdb"));
    EXPECT_STREQ("/srv/a.db", hl_sandbox_db_path("file:///srv/a.db"));
    EXPECT_STREQ("/srv/a.db", hl_sandbox_db_path("SQLite:///srv/a.db"));
    /* A "://" that does not follow a scheme is part of a plain path. */
    EXPECT_STREQ("./x://y.db", hl_sandbox_db_path("./x://y.db"));
}

/* Audit 25: the app.main runner did not grant outbound network for a network
 * -d DSN, as serve.c did; both now go through one helper. */
UTEST(sandbox_grant, default_dsn_grants_outbound_only_when_it_dials)
{
    HlSandboxPolicy p;
    hl_sandbox_policy_from_manifest(&p, NULL);
    EXPECT_EQ(p.network_outbound, 0);

    hl_sandbox_policy_add_default_dsn(&p, NULL);
    EXPECT_EQ(p.network_outbound, 0);
    hl_sandbox_policy_add_default_dsn(&p, "data.db");
    EXPECT_EQ(p.network_outbound, 0);
    hl_sandbox_policy_add_default_dsn(&p, ":memory:");
    EXPECT_EQ(p.network_outbound, 0);
    hl_sandbox_policy_add_default_dsn(&p, "sqlite:///srv/a.db");
    EXPECT_EQ(p.network_outbound, 0);

    hl_sandbox_policy_add_default_dsn(&p, "postgres://u:p@db/app");
    EXPECT_EQ(p.network_outbound, 1);

    hl_sandbox_policy_from_manifest(&p, NULL);
    hl_sandbox_policy_add_default_dsn(&p, "$DATABASE_URL");
    EXPECT_EQ(p.network_outbound, 1);

    hl_sandbox_policy_from_manifest(&p, NULL);
    hl_sandbox_policy_add_default_dsn(&p, "mysql://db/app");
    EXPECT_EQ(p.network_outbound, 1);
}

/* Audit 43: a DuckDB -d database is marked, so the sandbox grants DuckDB's
 * own sidecars (its .wal, .wal.checkpoint, .wal.recovery, .tmp/), not only
 * SQLite's names. An in-memory DuckDB has no file. */
UTEST(sandbox_audit43, duckdb_dsn_marks_the_policy)
{
    HlSandboxPolicy p;
    hl_sandbox_policy_from_manifest(&p, NULL);
    EXPECT_EQ(p.db_duckdb, 0);
    hl_sandbox_policy_add_default_dsn(&p, "data/app.db");
    EXPECT_EQ(p.db_duckdb, 0);
    hl_sandbox_policy_add_default_dsn(&p, "duckdb://:memory:");
    EXPECT_EQ(p.db_duckdb, 0);
    hl_sandbox_policy_add_default_dsn(&p, "DuckDB://data/app.duckdb");
    EXPECT_EQ(p.db_duckdb, 1);
    EXPECT_EQ(p.network_outbound, 0);
}

/* Audit 43: on Linux a DuckDB database's directory is granted rwc (Landlock
 * cannot grant a name that does not exist yet, and DuckDB recreates its .wal
 * after every checkpoint) - but never the app directory or an ancestor of it,
 * the app's .hull, or a world-writable directory. */
UTEST(sandbox_audit43, duckdb_dir_grant_keeps_the_app_tree_read_only)
{
    char base[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(base, sizeof base, "hull_duck") != NULL);
    char real_base[PATH_MAX];
    ASSERT_TRUE(realpath(base, real_base) != NULL);
    char app[PATH_MAX], data[PATH_MAX + 8], hull[PATH_MAX + 8], pub[PATH_MAX];
    char db[PATH_MAX + 32], out[PATH_MAX], want[PATH_MAX + 8];
    snprintf(app, sizeof app, "%s/app", real_base);
    snprintf(data, sizeof data, "%s/data", app);
    snprintf(hull, sizeof hull, "%s/.hull", app);
    snprintf(pub, sizeof pub, "%s/pub", real_base);
    ASSERT_EQ(mkdir(app, 0755), 0);
    ASSERT_EQ(mkdir(data, 0755), 0);
    ASSERT_EQ(mkdir(hull, 0755), 0);
    ASSERT_EQ(mkdir(pub, 0755), 0);
    ASSERT_EQ(chmod(pub, 0777), 0);

    /* A directory of its own inside the app: granted. */
    snprintf(db, sizeof db, "%s/x.duckdb", data);
    ASSERT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), 0);
    EXPECT_STREQ(out, data);
    /* The app directory itself, and an ancestor of it: refused. */
    snprintf(db, sizeof db, "%s/x.duckdb", app);
    EXPECT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), 1);
    snprintf(db, sizeof db, "%s/x.duckdb", real_base);
    EXPECT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), 1);
    snprintf(db, sizeof db, "%s/../x.duckdb", data);
    EXPECT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), 1);
    /* The reserved .hull: refused. */
    snprintf(db, sizeof db, "%s/x.duckdb", hull);
    EXPECT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), 1);
    /* World-writable: refused. */
    snprintf(db, sizeof db, "%s/x.duckdb", pub);
    EXPECT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), 1);
    /* /: refused. A directory that does not exist: an error. */
    EXPECT_EQ(hl_sandbox_duckdb_dir_grant(app, "/x.duckdb", out, sizeof out), 1);
    snprintf(db, sizeof db, "%s/nope/x.duckdb", real_base);
    EXPECT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), -1);
    /* Outside the app tree, its own directory: granted. */
    snprintf(want, sizeof want, "%s/var", real_base);
    ASSERT_EQ(mkdir(want, 0755), 0);
    snprintf(db, sizeof db, "%s/x.duckdb", want);
    ASSERT_EQ(hl_sandbox_duckdb_dir_grant(app, db, out, sizeof out), 0);
    EXPECT_STREQ(out, want);

    rmdir(want); rmdir(pub); rmdir(hull); rmdir(data); rmdir(app);
    rmdir(base);
}

#if defined(__APPLE__) && !defined(__COSMOPOLITAN__)
/* Audit 43: Seatbelt matches paths, so a DuckDB database's sidecars are
 * granted by name - its .wal, .wal.checkpoint, .wal.recovery and the .tmp/
 * subtree - and its directory stays unwritable. A SQLite database gets none. */
UTEST(sandbox_audit43, duckdb_seatbelt_names_its_sidecars)
{
    static char buf[16384];
    const char *params[256];
    HlSandboxPolicy p;
    hl_sandbox_policy_from_manifest(&p, NULL);
    hl_sandbox_policy_add_default_dsn(&p, "duckdb:///srv/app/data/x.duckdb");
    ASSERT_EQ(hl_sandbox_seatbelt_profile_db_for_test(
                  &p, NULL, "/srv/app/data/x.duckdb", buf, sizeof buf,
                  params, 256), 0);
    EXPECT_TRUE(strstr(buf, "(literal (param \"DUCK_F0\")) (literal (param \"DUCK_F1\"))\n"
                            "    (literal (param \"DUCK_F2\")) (subpath (param \"DUCK_TMP\")))")
                != NULL);
    EXPECT_TRUE(strstr(buf, "(subpath (param \"DB_DIR\"))") == NULL);
    static const char *const want[][2] = {
        { "DUCK_F0", "/srv/app/data/x.duckdb.wal" },
        { "DUCK_F1", "/srv/app/data/x.duckdb.wal.checkpoint" },
        { "DUCK_F2", "/srv/app/data/x.duckdb.wal.recovery" },
        { "DUCK_TMP", "/srv/app/data/x.duckdb.tmp" },
    };
    for (size_t w = 0; w < 4; w++) {
        int found = 0;
        for (int i = 0; params[i]; i += 2)
            if (strcmp(params[i], want[w][0]) == 0) {
                EXPECT_STREQ(params[i + 1], want[w][1]);
                found = 1;
            }
        EXPECT_EQ(found, 1);
    }

    hl_sandbox_policy_from_manifest(&p, NULL);
    hl_sandbox_policy_add_default_dsn(&p, "/srv/app/data/x.db");
    ASSERT_EQ(hl_sandbox_seatbelt_profile_db_for_test(
                  &p, NULL, "/srv/app/data/x.db", buf, sizeof buf,
                  params, 256), 0);
    EXPECT_TRUE(strstr(buf, "DUCK_") == NULL);
}
#endif

/* Audit 25: a TUI's terminal grant names only real tty device paths. */
UTEST(sandbox_tty, path_validation)
{
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/ttys003"), 1);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/ttyp0"), 1);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/tty"), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok(NULL), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok(""), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/pts/3"), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/ttys003/../disk0"), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/ttys\"))(allow default"), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/ttys 1"), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/ttyaaaaaaaaaaaaaaaaaaaa"), 0);
    EXPECT_EQ(hl_sandbox_tty_path_ok("/dev/console"), 0);
}

UTEST(sandbox_tty, own_ttys_are_valid_and_unique)
{
    char out[HL_SANDBOX_TTY_MAX][HL_SANDBOX_TTY_PATH_MAX];
    int n = hl_sandbox_own_ttys(out);
    ASSERT_TRUE(n >= 0 && n <= HL_SANDBOX_TTY_MAX);
    for (int i = 0; i < n; i++) {
        EXPECT_EQ(hl_sandbox_tty_path_ok(out[i]), 1);
        for (int j = 0; j < i; j++)
            EXPECT_STRNE(out[i], out[j]);
    }
}

#if defined(__APPLE__) && !defined(__COSMOPOLITAN__)
/* Audit 25: the macOS TUI profile granted every /dev/ttys[0-9]+ and an
 * unrestricted file-ioctl. It now names /dev/tty and the process's own
 * terminals, each by literal param, and ioctl only on those - and, since
 * audit 26, only the ioctl commands the TUI issues (no TIOCSTI). */
UTEST(sandbox_tty, seatbelt_profile_names_only_own_ttys)
{
    static char buf[8192];
    const char *params[256];
    HlSandboxPolicy p;
    hl_sandbox_policy_from_manifest(&p, NULL);
    p.tui = 1;

    const char *ttys[] = { "/dev/ttys004", "/dev/ttys\")(allow default" };
    ASSERT_EQ(hl_sandbox_seatbelt_profile_for_test(&p, ttys, 2, buf, sizeof buf,
                                                   params, 256), 0);
    EXPECT_TRUE(strstr(buf, "regex") == NULL);
    EXPECT_TRUE(strstr(buf, "ttys") == NULL);   /* never interpolated */
    EXPECT_TRUE(strstr(buf, "(allow file-ioctl)") == NULL);
    EXPECT_TRUE(strstr(buf, "(allow file-ioctl (require-all\n"
                            "    (require-any (literal \"/dev/tty\") "
                            "(literal (param \"TTY_0\")))\n") != NULL);
    /* Audit 26: only the commands the TUI issues - no TIOCSTI. */
    EXPECT_TRUE(strstr(buf, "    (ioctl-command TIOCGWINSZ TIOCGETA TIOCSETA"
                            " TIOCSETAW TIOCSETAF)))") != NULL);
    EXPECT_TRUE(strstr(buf, "TIOCSTI") == NULL);
    EXPECT_TRUE(strstr(buf, "(allow file-read* file-write* (literal \"/dev/tty\") "
                            "(literal (param \"TTY_0\")))") != NULL);
    EXPECT_TRUE(strstr(buf, "TTY_1") == NULL);   /* the bad path is dropped */

    int found = 0;
    for (int i = 0; params[i]; i += 2) {
        if (strcmp(params[i], "TTY_0") == 0) {
            EXPECT_STREQ(params[i + 1], "/dev/ttys004");
            found = 1;
        }
        EXPECT_STRNE(params[i], "TTY_1");
    }
    EXPECT_EQ(found, 1);

    /* No terminal: /dev/tty alone. */
    ASSERT_EQ(hl_sandbox_seatbelt_profile_for_test(&p, NULL, 0, buf, sizeof buf,
                                                   params, 256), 0);
    EXPECT_TRUE(strstr(buf, "(allow file-ioctl (require-all\n"
                            "    (require-any (literal \"/dev/tty\"))\n") != NULL);

    /* Not a TUI: no terminal grant at all. */
    p.tui = 0;
    ASSERT_EQ(hl_sandbox_seatbelt_profile_for_test(&p, ttys, 1, buf, sizeof buf,
                                                   params, 256), 0);
    EXPECT_TRUE(strstr(buf, "/dev/tty\"") == NULL);
    EXPECT_TRUE(strstr(buf, "TTY_0") == NULL);
}
#endif

UTEST_MAIN()
