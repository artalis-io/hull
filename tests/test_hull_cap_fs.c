/*
 * test_hull_cap_fs.c — Tests for shared filesystem capability
 *
 * Tests path validation, read, write, exists, delete operations.
 * Uses a temporary directory as the base_dir.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/hull_cap.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char test_dir[256];
static HlFsConfig test_cfg;

static void setup_fs(void)
{
    snprintf(test_dir, sizeof(test_dir), "/tmp/hull_test_%d", getpid());
    mkdir(test_dir, 0755);
    test_cfg.base_dir = test_dir;
    test_cfg.base_len = strlen(test_dir);
}

static void teardown_fs(void)
{
    /* Clean up test files */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    system(cmd);
}

/* ── Path validation tests ──────────────────────────────────────────── */

UTEST(hl_cap_fs, validate_normal_path)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "file.txt"), 0);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "subdir/file.txt"), 0);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_rejects_dotdot)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "../etc/passwd"), -1);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "subdir/../../etc/passwd"), -1);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, ".."), -1);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_rejects_absolute)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "/etc/passwd"), -1);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "/tmp/evil"), -1);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_rejects_empty)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, ""), -1);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_null)
{
    ASSERT_EQ(hl_cap_fs_validate(NULL, "file.txt"), -1);
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, NULL), -1);
    teardown_fs();
}

/* ── Write and read tests ───────────────────────────────────────────── */

UTEST(hl_cap_fs, write_and_read)
{
    setup_fs();

    const char *data = "Hello, Hull!";
    int rc = hl_cap_fs_write(&test_cfg, "test.txt", data, strlen(data));
    ASSERT_EQ(rc, 0);

    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "test.txt", buf, sizeof(buf));
    ASSERT_EQ(nread, (int64_t)strlen(data));
    buf[nread] = '\0';
    ASSERT_STREQ(buf, data);

    teardown_fs();
}

UTEST(hl_cap_fs, write_creates_subdirs)
{
    setup_fs();

    const char *data = "nested file";
    int rc = hl_cap_fs_write(&test_cfg, "a/b/c/file.txt",
                               data, strlen(data));
    ASSERT_EQ(rc, 0);

    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "a/b/c/file.txt",
                                     buf, sizeof(buf));
    ASSERT_EQ(nread, (int64_t)strlen(data));

    teardown_fs();
}

UTEST(hl_cap_fs, read_file_size)
{
    setup_fs();

    const char *data = "12345";
    hl_cap_fs_write(&test_cfg, "size.txt", data, 5);

    /* NULL buf → returns file size */
    int64_t size = hl_cap_fs_read(&test_cfg, "size.txt", NULL, 0);
    ASSERT_EQ(size, 5);

    teardown_fs();
}

UTEST(hl_cap_fs, read_nonexistent)
{
    setup_fs();
    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "nope.txt", buf, sizeof(buf));
    ASSERT_EQ(nread, -1);
    teardown_fs();
}

/* ── Exists and delete tests ────────────────────────────────────────── */

UTEST(hl_cap_fs, exists)
{
    setup_fs();

    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "gone.txt"), 0);

    hl_cap_fs_write(&test_cfg, "here.txt", "x", 1);
    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "here.txt"), 1);

    teardown_fs();
}

UTEST(hl_cap_fs, delete)
{
    setup_fs();

    hl_cap_fs_write(&test_cfg, "del.txt", "x", 1);
    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "del.txt"), 1);

    int rc = hl_cap_fs_delete(&test_cfg, "del.txt");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "del.txt"), 0);

    teardown_fs();
}

UTEST(hl_cap_fs, delete_nonexistent)
{
    setup_fs();
    int rc = hl_cap_fs_delete(&test_cfg, "nope.txt");
    ASSERT_EQ(rc, -1);
    teardown_fs();
}

/* ── Path traversal rejection in operations ─────────────────────────── */

UTEST(hl_cap_fs, write_rejects_traversal)
{
    setup_fs();
    int rc = hl_cap_fs_write(&test_cfg, "../evil.txt", "x", 1);
    ASSERT_EQ(rc, -1);
    teardown_fs();
}

UTEST(hl_cap_fs, read_rejects_traversal)
{
    setup_fs();
    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "../etc/passwd",
                                     buf, sizeof(buf));
    ASSERT_EQ(nread, -1);
    teardown_fs();
}

UTEST_MAIN();
