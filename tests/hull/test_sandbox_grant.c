/*
 * test_sandbox_grant.c - what the kernel grant for a manifest fs entry
 * covers (hl_sandbox_resolve_grant, src/hull/sandbox.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/sandbox.h"
#include "test_tmpdir.h"

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

UTEST(sandbox_grant, refuses_escapes)
{
    char out[PATH_MAX];
    EXPECT_EQ(hl_sandbox_resolve_grant("/srv/app", "/etc/passwd", out, sizeof out, 1), -1);
    EXPECT_EQ(hl_sandbox_resolve_grant("/srv/app", "../x", out, sizeof out, 1), -1);
    EXPECT_EQ(hl_sandbox_resolve_grant("/srv/app", "", out, sizeof out, 0), -1);
}

UTEST_MAIN();
