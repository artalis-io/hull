/*
 * test_hull_cap_fs.c — Tests for shared filesystem capability
 *
 * Tests path validation, read, write, exists, delete operations.
 * Uses a temporary directory as the base_dir.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* FTW_DEPTH / FTW_PHYS are XSI extensions to nftw; on glibc they're
 * only declared when _XOPEN_SOURCE >= 500. macOS exposes them
 * unconditionally AND uses _XOPEN_SOURCE to gate Darwin extensions
 * the other way (defining it hides clock_gettime_nsec_np /
 * CLOCK_UPTIME_RAW that utest.h needs), so this define has to stay
 * Linux-only. */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "hull/cap/fs.h"
#include <errno.h>
#include <ftw.h>
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

static int teardown_rm_entry(const char *path, const struct stat *sb,
                             int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

static void teardown_fs(void)
{
    /* In-process recursive delete via nftw(FTW_DEPTH). Cosmopolitan's
     * toybox rm rejects `-r` ("rm: illegal option -- r"), and
     * `system("rm -rf ...")` leaves the next test hitting EEXIST on
     * setup_fs's mkdir. nftw is POSIX and works uniformly on Linux,
     * macOS, and cosmo. ENOENT is fine — that's the goal. */
    if (nftw(test_dir, teardown_rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0
        && errno != ENOENT) {
        /* Best-effort cleanup; test failures shouldn't mask the
         * actual assertion that failed. */
    }
}

/* ── Path validation tests ──────────────────────────────────────────── */

UTEST(hl_cap_fs, validate_normal_path)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "file.txt", NULL), 0);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "subdir/file.txt", NULL), 0);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_rejects_dotdot)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "../etc/passwd", NULL), -1);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "subdir/../../etc/passwd", NULL), -1);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "..", NULL), -1);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_rejects_absolute)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "/etc/passwd", NULL), -1);
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "/tmp/evil", NULL), -1);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_rejects_empty)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "", NULL), -1);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_null)
{
    ASSERT_EQ(hl_cap_fs_validate(NULL, "file.txt", NULL), -1);
    setup_fs();
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, NULL, NULL), -1);
    teardown_fs();
}

/* ── Write and read tests ───────────────────────────────────────────── */

UTEST(hl_cap_fs, write_and_read)
{
    setup_fs();

    const char *data = "Hello, Hull!";
    int rc = hl_cap_fs_write(&test_cfg, "test.txt", data, strlen(data), NULL);
    ASSERT_EQ(rc, 0);

    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "test.txt", buf, sizeof(buf), NULL);
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
                               data, strlen(data), NULL);
    ASSERT_EQ(rc, 0);

    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "a/b/c/file.txt",
                                     buf, sizeof(buf), NULL);
    ASSERT_EQ(nread, (int64_t)strlen(data));

    teardown_fs();
}

UTEST(hl_cap_fs, read_file_size)
{
    setup_fs();

    const char *data = "12345";
    hl_cap_fs_write(&test_cfg, "size.txt", data, 5, NULL);

    /* NULL buf → returns file size */
    int64_t size = hl_cap_fs_read(&test_cfg, "size.txt", NULL, 0, NULL);
    ASSERT_EQ(size, 5);

    teardown_fs();
}

UTEST(hl_cap_fs, read_nonexistent)
{
    setup_fs();
    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "nope.txt", buf, sizeof(buf), NULL);
    ASSERT_EQ(nread, -1);
    teardown_fs();
}

/* ── Exists and delete tests ────────────────────────────────────────── */

UTEST(hl_cap_fs, exists)
{
    setup_fs();

    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "gone.txt", NULL), 0);

    hl_cap_fs_write(&test_cfg, "here.txt", "x", 1, NULL);
    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "here.txt", NULL), 1);

    teardown_fs();
}

UTEST(hl_cap_fs, delete)
{
    setup_fs();

    hl_cap_fs_write(&test_cfg, "del.txt", "x", 1, NULL);
    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "del.txt", NULL), 1);

    int rc = hl_cap_fs_delete(&test_cfg, "del.txt", NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(hl_cap_fs_exists(&test_cfg, "del.txt", NULL), 0);

    teardown_fs();
}

UTEST(hl_cap_fs, delete_nonexistent)
{
    setup_fs();
    int rc = hl_cap_fs_delete(&test_cfg, "nope.txt", NULL);
    ASSERT_EQ(rc, -1);
    teardown_fs();
}

/* ── Path traversal rejection in operations ─────────────────────────── */

UTEST(hl_cap_fs, write_rejects_traversal)
{
    setup_fs();
    int rc = hl_cap_fs_write(&test_cfg, "../evil.txt", "x", 1, NULL);
    ASSERT_EQ(rc, -1);
    teardown_fs();
}

UTEST(hl_cap_fs, read_rejects_traversal)
{
    setup_fs();
    char buf[256];
    int64_t nread = hl_cap_fs_read(&test_cfg, "../etc/passwd",
                                     buf, sizeof(buf), NULL);
    ASSERT_EQ(nread, -1);
    teardown_fs();
}

UTEST(hl_cap_fs, validate_rejects_symlink_escape)
{
    setup_fs();
    /* Create a symlink inside test_dir pointing to /tmp */
    char link_path[512];
    snprintf(link_path, sizeof(link_path), "%s/escape", test_dir);
    symlink("/tmp", link_path);

    /* Accessing via symlink should be rejected */
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "escape/some_file", NULL), -1);

    unlink(link_path);
    teardown_fs();
}

/* ── mmap tests ─────────────────────────────────────────────────────── */

UTEST(hl_cap_fs, mmap_basic)
{
    setup_fs();

    /* Write a test file */
    const char *data = "hello mmap world";
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "mmap_test.txt", data, strlen(data), NULL), 0);

    /* mmap it */
    HlMappedBuffer *buf = hl_cap_fs_mmap(&test_cfg, "mmap_test.txt", NULL, NULL);
    ASSERT_NE(buf, NULL);
    ASSERT_EQ(buf->len, strlen(data));
    ASSERT_EQ(buf->closed, 0);
    ASSERT_EQ(memcmp(buf->addr, data, strlen(data)), 0);

    hl_cap_fs_munmap(buf);
    teardown_fs();
}

/* Regression (audit F1): a zero-copy borrower (e.g. image.from_buffer)
 * must keep the mapping alive even if the source is closed first. */
UTEST(hl_cap_fs, mmap_borrow_defers_close)
{
    setup_fs();
    const char *data = "borrowed mmap bytes";
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "mmap_borrow.txt", data, strlen(data), NULL), 0);

    HlMappedBuffer *buf = hl_cap_fs_mmap(&test_cfg, "mmap_borrow.txt", NULL, NULL);
    ASSERT_NE(buf, NULL);

    hl_cap_fs_mmap_borrow(buf);
    ASSERT_EQ(buf->borrow_count, 1);

    /* Close while borrowed: teardown is deferred, mapping stays valid. */
    hl_cap_fs_munmap(buf);
    ASSERT_EQ(buf->pending_free, 1);
    ASSERT_EQ(buf->closed, 0);
    ASSERT_EQ(memcmp(buf->addr, data, strlen(data)), 0);

    /* Last release completes the deferred munmap + free (ASan/leak-san
     * confirms it runs exactly once and nothing dangles). */
    hl_cap_fs_mmap_release(buf);
    teardown_fs();
}

/* A borrow that is released BEFORE any close must not tear the buffer down;
 * the normal munmap still owns teardown. */
UTEST(hl_cap_fs, mmap_release_without_close_keeps_buffer)
{
    setup_fs();
    const char *data = "still open";
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "mmap_rel.txt", data, strlen(data), NULL), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap(&test_cfg, "mmap_rel.txt", NULL, NULL);
    ASSERT_NE(buf, NULL);

    hl_cap_fs_mmap_borrow(buf);
    hl_cap_fs_mmap_release(buf);
    ASSERT_EQ(buf->borrow_count, 0);
    ASSERT_EQ(buf->pending_free, 0);
    ASSERT_EQ(buf->closed, 0);
    ASSERT_EQ(memcmp(buf->addr, data, strlen(data)), 0);

    hl_cap_fs_munmap(buf); /* normal teardown, no borrowers */
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_path_traversal)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_mmap(&test_cfg, "../etc/passwd", NULL, NULL), NULL);
    ASSERT_EQ(hl_cap_fs_mmap(&test_cfg, "/etc/passwd", NULL, NULL), NULL);
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_nonexistent)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_mmap(&test_cfg, "no_such_file.txt", NULL, NULL), NULL);
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_null_safe)
{
    ASSERT_EQ(hl_cap_fs_mmap(NULL, "test.txt", NULL, NULL), NULL);
    ASSERT_EQ(hl_cap_fs_mmap(&test_cfg, NULL, NULL, NULL), NULL);
    hl_cap_fs_munmap(NULL); /* should not crash */
}

UTEST_MAIN();
