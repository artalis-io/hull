/*
 * test_hull_cap_fs.c - Tests for shared filesystem capability
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
#include "hull/shared/host.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/cap/fs_resolve.h"
#include "hull/cap/db_live_files.h"
#include "hull/utils/alloc.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <ftw.h>
#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../test_tmpdir.h"
#ifdef HL_ENABLE_SQLITE
#include <pthread.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <time.h>
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#endif

static char test_dir[256];
static HlFsConfig  test_cfg;
static HlFsPolicy  test_policy = HL_FS_POLICY_INIT;
static HlAllocator test_alloc;

/* Checkpoint 3: read/write/mmap are policy-gated. These tests exercise the
 * read/write/mmap MECHANICS, so setup grants exactly the paths they touch (both
 * read and write sets); the traversal/absolute cases stay denied by the lexical
 * pre-check. Policy authorization itself is covered by test_fs_policy.c. */
static const char *const test_grants[] = {
    "a/b/c/file.txt", "atomic.txt", "del.txt", "dirt", "gone.txt", "here.txt",
    "huge.bin", "link.txt", "mode.txt", "sub/",
    "mmap_borrow.txt", "mmap_rel.txt", "mmap_test.txt", "no_such_file.txt",
    "nope.txt", "size.txt", "test.txt", "whole.txt",
    "win_borrow.bin", "win_cross.bin", "win_eof.bin", "win_rej.bin", "win_zero.bin",
    "shrink.bin", "many.bin", "suid.bin", "trunc.bin",
};

static void setup_fs(void)
{
    hl_test_path(test_dir, sizeof(test_dir), "hull_test_%d", getpid());
    mkdir(test_dir, 0755);
    {
        char sub[512];
        snprintf(sub, sizeof(sub), "%s/sub", test_dir);  /* a SUBTREE anchor */
        mkdir(sub, 0755);
    }
    test_cfg.base_dir = test_dir;
    test_cfg.base_len = strlen(test_dir);

    hl_alloc_init(&test_alloc, 0);
    test_policy = HL_FS_POLICY_INIT;
    size_t n = sizeof(test_grants) / sizeof(test_grants[0]);
    const char *perr = NULL;
    if (hl_fs_policy_compile_manifest(test_dir, &test_alloc,
                                      test_grants, n, test_grants, n,
                                      &test_policy, &perr) != 0) {
        fprintf(stderr, "test_fs setup: policy compile failed: %s\n",
                perr ? perr : "?");
    }
    test_cfg.policy = &test_policy;
}

static int teardown_rm_entry(const char *path, const struct stat *sb,
                             int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

static void teardown_fs(void)
{
    hl_fs_policy_free(&test_policy);   /* close held anchor fds; idempotent */
    test_cfg.policy = NULL;
    /* In-process recursive delete via nftw(FTW_DEPTH). Cosmopolitan's
     * toybox rm rejects `-r` ("rm: illegal option -- r"), and
     * `system("rm -rf ...")` leaves the next test hitting EEXIST on
     * setup_fs's mkdir. nftw is POSIX and works uniformly on Linux,
     * macOS, and cosmo. ENOENT is fine - that's the goal. */
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

/* ── Atomic write ───────────────────────────────────────────────────── */

/* Whether any ".hull-tmp-" file is left in `dir` (relative to test_dir). */
static int temp_left(const char *dir)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", test_dir, dir);
    DIR *d = opendir(p);
    if (!d) return 0;
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
        if (strncmp(de->d_name, ".hull-tmp-", 10) == 0) found = 1;
    closedir(d);
    return found;
}

static int host_path_stat(const char *rel, struct stat *st)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", test_dir, rel);
    return lstat(p, st);
}

UTEST(hl_cap_fs, write_replaces_by_rename)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "atomic.txt", "old contents", 12, NULL), 0);
    struct stat before, after;
    ASSERT_EQ(host_path_stat("atomic.txt", &before), 0);
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "atomic.txt", "new", 3, NULL), 0);
    ASSERT_EQ(host_path_stat("atomic.txt", &after), 0);

    char buf[32];
    int64_t n = hl_cap_fs_read(&test_cfg, "atomic.txt", buf, sizeof(buf), NULL);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(memcmp(buf, "new", 3), 0);
    /* A new inode: the old file was replaced whole, never truncated in place. */
    if (!hl_host_is_windows())
        EXPECT_NE(before.st_ino, after.st_ino);
    EXPECT_FALSE(temp_left("."));
    EXPECT_FALSE(temp_left("a/b/c"));
    teardown_fs();
}

UTEST(hl_cap_fs, write_keeps_the_mode_of_a_replaced_file)
{
    if (hl_host_is_windows()) return;          /* no POSIX permission bits */
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "mode.txt", "secret", 6, NULL), 0);
    char p[512];
    snprintf(p, sizeof(p), "%s/mode.txt", test_dir);
    ASSERT_EQ(chmod(p, 0600), 0);
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "mode.txt", "rotated", 7, NULL), 0);
    struct stat st;
    ASSERT_EQ(host_path_stat("mode.txt", &st), 0);
    EXPECT_EQ((int)(st.st_mode & 07777), 0600);
    teardown_fs();
}

/* Audit 22: the atomic replace copied setuid / setgid onto new content (an
 * in-place write had the kernel clear them). */
UTEST(hl_cap_fs, write_drops_setuid_and_setgid_of_a_replaced_file)
{
    if (hl_host_is_windows()) return;          /* no POSIX permission bits */
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "suid.bin", "old", 3, NULL), 0);
    char p[512];
    snprintf(p, sizeof(p), "%s/suid.bin", test_dir);
    ASSERT_EQ(chmod(p, 06755), 0);
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "suid.bin", "new", 3, NULL), 0);
    struct stat st;
    ASSERT_EQ(host_path_stat("suid.bin", &st), 0);
    EXPECT_EQ((int)(st.st_mode & 07777), 0755);
    teardown_fs();
}

UTEST(hl_cap_fs, write_to_a_directory_fails_and_leaves_no_temp)
{
    setup_fs();
    char p[512];
    snprintf(p, sizeof(p), "%s/dirt", test_dir);
    ASSERT_EQ(mkdir(p, 0755), 0);
    const char *err = NULL;
    EXPECT_EQ(hl_cap_fs_write(&test_cfg, "dirt", "x", 1, &err), -1);
    EXPECT_STREQ(err, "not_a_regular_file");
    EXPECT_FALSE(temp_left("."));
    teardown_fs();
}

UTEST(hl_cap_fs, write_refuses_a_symlink_under_an_exact_grant)
{
    if (hl_host_is_windows()) return;          /* symlinks need privileges */
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "here.txt", "keep", 4, NULL), 0);
    char p[512];
    snprintf(p, sizeof(p), "%s/link.txt", test_dir);
    ASSERT_EQ(symlink("here.txt", p), 0);
    const char *err = NULL;
    EXPECT_EQ(hl_cap_fs_write(&test_cfg, "link.txt", "x", 1, &err), -1);
    EXPECT_STREQ(err, "symlink_denied");
    struct stat st;
    ASSERT_EQ(host_path_stat("link.txt", &st), 0);
    EXPECT_TRUE(S_ISLNK(st.st_mode));          /* the link is untouched */
    EXPECT_FALSE(temp_left("."));
    teardown_fs();
}

UTEST(hl_cap_fs, write_goes_through_an_in_root_symlink_under_a_subtree)
{
    if (hl_host_is_windows()) return;
    setup_fs();
    char p[512];
    snprintf(p, sizeof(p), "%s/sub/real.txt", test_dir);
    FILE *f = fopen(p, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("before", f);
    fclose(f);
    snprintf(p, sizeof(p), "%s/sub/alias.txt", test_dir);
    ASSERT_EQ(symlink("real.txt", p), 0);

    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "sub/alias.txt", "after", 5, NULL), 0);
    struct stat st;
    ASSERT_EQ(host_path_stat("sub/alias.txt", &st), 0);
    EXPECT_TRUE(S_ISLNK(st.st_mode));          /* still a link... */
    char buf[16];
    int64_t n = hl_cap_fs_read(&test_cfg, "sub/real.txt", buf, sizeof(buf), NULL);
    ASSERT_EQ(n, 5);                           /* ...whose target was written */
    EXPECT_EQ(memcmp(buf, "after", 5), 0);
    EXPECT_FALSE(temp_left("sub"));
    teardown_fs();
}

/* Audit 23: the in-place write (through an in-root link under a subtree
 * grant) kept setuid / setgid when Hull ran as root - the kernel clears
 * them on a write only for a writer without CAP_FSETID. */
UTEST(hl_cap_fs, an_in_place_write_drops_setuid_and_setgid)
{
    if (hl_host_is_windows()) return;
    setup_fs();
    char p[512];
    snprintf(p, sizeof(p), "%s/sub/real.bin", test_dir);
    FILE *f = fopen(p, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("old", f);
    fclose(f);
    ASSERT_EQ(chmod(p, 06755), 0);
    snprintf(p, sizeof(p), "%s/sub/alias.bin", test_dir);
    ASSERT_EQ(symlink("real.bin", p), 0);
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "sub/alias.bin", "new", 3, NULL), 0);
    struct stat st;
    ASSERT_EQ(host_path_stat("sub/real.bin", &st), 0);
    EXPECT_EQ((int)(st.st_mode & 07777), 0755);
    teardown_fs();
}

/* Audit 25: compute.stream writes through hl_cap_fs_open_write_fd, which
 * left setuid / setgid on for a root writer. The open itself now clears
 * them - observable before any write, whoever the writer is. */
UTEST(hl_cap_fs, the_write_open_drops_setuid_and_setgid)
{
    if (hl_host_is_windows()) return;
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "suid.bin", "old", 3, NULL), 0);
    char p[512];
    snprintf(p, sizeof(p), "%s/suid.bin", test_dir);
    ASSERT_EQ(chmod(p, 06755), 0);
    const char *err = NULL;
    int fd = hl_cap_fs_open_write_fd(&test_cfg, "suid.bin", &err);
    ASSERT_GE(fd, 0);
    struct stat st;
    ASSERT_EQ(fstat(fd, &st), 0);
    EXPECT_EQ((int)(st.st_mode & 07777), 0755);
    EXPECT_EQ(hl_fs_strip_setid(fd), 0);       /* nothing left to clear */
    close(fd);
    teardown_fs();
}

/* Audit 27: the write open no longer truncates before the setid check (a
 * refused write had already emptied the file); it truncates after it, so a
 * write through it still replaces the whole content. */
UTEST(hl_cap_fs, the_write_open_still_truncates)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "trunc.bin", "abcdef", 6, NULL), 0);
    const char *err = NULL;
    int fd = hl_cap_fs_open_write_fd(&test_cfg, "trunc.bin", &err);
    ASSERT_GE(fd, 0);
    struct stat st;
    ASSERT_EQ(fstat(fd, &st), 0);
    EXPECT_EQ((long long)st.st_size, 0LL);
    ASSERT_EQ((ssize_t)2, write(fd, "xy", 2));
    close(fd);
    char buf[16];
    int64_t n = hl_cap_fs_read(&test_cfg, "trunc.bin", buf, sizeof(buf), NULL);
    ASSERT_EQ(n, (int64_t)2);
    EXPECT_EQ(0, memcmp(buf, "xy", 2));
    teardown_fs();
}

/* Audit 26: a non-root writer of a file it does not own got EPERM from the
 * fchmod and the write was refused - a regression; root still fails closed.
 * (Making a file owned by another user needs root, so the policy is tested
 * directly.) */
UTEST(hl_cap_fs, setid_strip_eperm_is_ok_only_for_non_root)
{
    EXPECT_EQ(1, hl_fs_setid_fchmod_failure_ok(EPERM, 0));
    EXPECT_EQ(0, hl_fs_setid_fchmod_failure_ok(EPERM, 1));
    EXPECT_EQ(0, hl_fs_setid_fchmod_failure_ok(EIO, 0));
    EXPECT_EQ(0, hl_fs_setid_fchmod_failure_ok(EROFS, 0));
}

UTEST(hl_cap_fs, write_ex_without_sync_is_still_a_whole_replace)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write_ex(&test_cfg, "atomic.txt", "one", 3,
                                 HL_FS_WRITE_NO_SYNC, NULL), 0);
    ASSERT_EQ(hl_cap_fs_write_ex(&test_cfg, "atomic.txt", "two!", 4,
                                 HL_FS_WRITE_NO_SYNC, NULL), 0);
    char buf[16];
    ASSERT_EQ(hl_cap_fs_read(&test_cfg, "atomic.txt", buf, sizeof(buf), NULL), 4);
    EXPECT_EQ(memcmp(buf, "two!", 4), 0);
    EXPECT_FALSE(temp_left("."));
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

/* exists / delete went around the fs policy (a base_dir path join): an
 * ungranted file could be probed and removed. */
UTEST(hl_cap_fs, exists_and_delete_honour_the_grants)
{
    setup_fs();
    char p[600];
    snprintf(p, sizeof p, "%s/secret.txt", test_dir);
    FILE *f = fopen(p, "w");
    ASSERT_TRUE(f != NULL);
    fputs("s", f);
    fclose(f);

    const char *err = NULL;
    EXPECT_EQ(hl_cap_fs_exists(&test_cfg, "secret.txt", &err), -1);
    err = NULL;
    EXPECT_EQ(hl_cap_fs_delete(&test_cfg, "secret.txt", &err), -1);
    EXPECT_TRUE(err != NULL);
    struct stat st;
    EXPECT_EQ(stat(p, &st), 0);                    /* still there */

    /* a granted path whose parent is missing is absent, not an error */
    EXPECT_EQ(hl_cap_fs_exists(&test_cfg, "a/b/c/file.txt", NULL), 0);
    /* deleting a missing granted file names the reason */
    err = NULL;
    EXPECT_EQ(hl_cap_fs_delete(&test_cfg, "nope.txt", &err), -1);
    EXPECT_STREQ(err ? err : "(null)", "not_found");
    teardown_fs();
}

/* Windows splits a path on a backslash too and reads "C:" as a drive: a
 * caller path holding either is refused there (ordinary bytes on POSIX). */
UTEST(hl_cap_fs, windows_separators_are_refused_on_windows)
{
    if (!hl_host_is_windows()) UTEST_SKIP("Windows-only path syntax");
    setup_fs();
    char buf[16];
    const char *err = NULL;
    EXPECT_EQ(hl_cap_fs_read(&test_cfg, "sub\\..\\..\\x", buf, sizeof buf, &err), -1);
    EXPECT_STREQ(err ? err : "(null)", "invalid_path");
    err = NULL;
    EXPECT_EQ(hl_cap_fs_write(&test_cfg, "sub/C:x", "x", 1, &err), -1);
    EXPECT_STREQ(err ? err : "(null)", "invalid_path");
    teardown_fs();
}

/* Audit 22: Windows drops a trailing dot or space and opens an 8.3 short
 * name as its long name, so a pattern grant matched one spelling and the OS
 * opened another file. Such a component is refused there. */
UTEST(hl_cap_fs, windows_name_aliases_are_refused_on_windows)
{
    if (!hl_host_is_windows()) UTEST_SKIP("Windows-only name aliasing");
    setup_fs();
    static const char *const bad[] = { "sub/a.txt.", "sub/a.txt ", "sub/x./y",
                                       "sub/secret~1.htm" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *err = NULL;
        EXPECT_EQ_MSG(hl_cap_fs_write(&test_cfg, bad[i], "x", 1, &err), -1, bad[i]);
        EXPECT_STREQ(err ? err : "(null)", "invalid_path");
    }
    EXPECT_EQ(hl_cap_fs_write(&test_cfg, "sub/a~b.txt", "x", 1, NULL), 0);
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

    /* Creating a symlink needs SeCreateSymbolicLinkPrivilege on Windows,
     * which an ordinary account does not hold. The return value was
     * ignored here, so on such a host no symlink existed and the assertion
     * below ran against a plain missing path - it passed or failed for
     * reasons having nothing to do with symlink escape, i.e. the test was
     * silently covering nothing. Skip honestly instead. */
    if (symlink("/tmp", link_path) != 0) {
        teardown_fs();
        UTEST_SKIP("symlink() unavailable (needs privilege on this host)");
    }

    /* Accessing via symlink should be rejected */
    ASSERT_EQ(hl_cap_fs_validate(&test_cfg, "escape/some_file", NULL), -1);

    unlink(link_path);
    teardown_fs();
}

/* ── mmap tests ─────────────────────────────────────────────────────── */

/* A file that shrinks under its mapping no longer kills the process: the
 * read of a page past the new end raised SIGBUS. In a child, since a failure
 * is the process dying. Skipped where the host refuses to shrink a mapped file
 * (Windows), which is the same protection. */
#include <signal.h>
#include <sys/wait.h>
UTEST(hl_cap_fs, mmap_truncated_file_reads_zeros)
{
    setup_fs();
    long ps = sysconf(_SC_PAGESIZE);
    size_t n = (size_t)(ps > 0 ? ps : 4096) * 3;
    char *data = malloc(n);
    ASSERT_TRUE(data != NULL);
    memset(data, 'x', n);
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "shrink.bin", data, n, NULL), 0);
    free(data);

    char path[512];
    snprintf(path, sizeof(path), "%s/shrink.bin", test_dir);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        HlMappedBuffer *buf = hl_cap_fs_mmap(&test_cfg, "shrink.bin", NULL, NULL);
        if (!buf) _exit(10);
        if (truncate(path, 0) != 0) _exit(77);          /* host refuses: skip */
        volatile const char *p = (const char *)buf->addr;
        char c = p[n - 1];                              /* page past the new end */
        hl_cap_fs_munmap(buf);
        /* Surviving the read is the point. Linux faults the page (SIGBUS,
         * absorbed: zeros); macOS keeps serving the cached page. */
#ifdef __linux__
        _exit(c == 0 ? 0 : 11);
#else
        (void)c;
        _exit(0);
#endif
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    teardown_fs();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
        UTEST_SKIP("the host refuses to shrink a mapped file");
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

/* Audit 20: past MMAP_GUARD_SLOTS (256) live mappings a new one was made
 * unguarded, so a file that shrank under it crashed the server with the
 * SIGBUS the guard exists to absorb. It is refused now, and a slot a
 * close frees is used again. */
UTEST(hl_cap_fs, mmap_refused_past_the_guard_table)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "many.bin", "abc", 3, NULL), 0);
    enum { TRY = 300 };
    HlMappedBuffer *bufs[TRY];
    const char *err = NULL;
    int n = 0;
    for (; n < TRY; n++) {
        err = NULL;
        bufs[n] = hl_cap_fs_mmap(&test_cfg, "many.bin", NULL, &err);
        if (!bufs[n]) break;
    }
    EXPECT_LT(n, TRY);                 /* refused before 300 */
    EXPECT_LE(n, 256);
    ASSERT_TRUE(err != NULL);
    EXPECT_STREQ(err, "too_many_mappings");
    ASSERT_GT(n, 0);
    hl_cap_fs_munmap(bufs[--n]);       /* frees a slot */
    bufs[n] = hl_cap_fs_mmap(&test_cfg, "many.bin", NULL, &err);
    EXPECT_TRUE(bufs[n] != NULL);
    if (bufs[n]) n++;
    while (n > 0) hl_cap_fs_munmap(bufs[--n]);
    teardown_fs();
}

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

/* Demand paging: mapping a large (2 GiB) SPARSE file as a 512 MiB window and
 * touching only a handful of pages must NOT make the file/window resident - the
 * whole point of zero-copy mapped spans over huge files ("opening a 50 GB file
 * must not allocate 50 GB of RAM"). Coarse per the spec (no exact RSS numbers):
 * we assert peak RSS stays FAR below the window size, with GiB-scale margin that
 * dwarfs any MB-scale ru_maxrss pollution from sibling tests in this binary.
 * A few real pages are written into the sparse file so the touched reads fault
 * genuine data (and read back correctly), while the rest stay holes. */
UTEST(hl_cap_fs, mmap_window_demand_paging)
{
    setup_fs();

    const uint64_t LOGICAL = (uint64_t)2 << 30;   /* 2 GiB sparse file */
    const uint64_t WINDOW  = (uint64_t)512 << 20; /* 512 MiB mapped window (< 1 GiB cap) */
    const long     PAGE    = sysconf(_SC_PAGESIZE);

    char path[512];
    snprintf(path, sizeof(path), "%s/huge.bin", test_dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_NE(fd, -1);
    if (ftruncate(fd, (off_t)LOGICAL) != 0) {      /* CI FS may refuse a 2 GiB logical file */
        close(fd); unlink(path); teardown_fs();
        UTEST_SKIP("filesystem cannot create a 2 GiB sparse file");
    }
    /* Write real pages at scattered offsets INSIDE the window so touching them
     * faults genuine (non-hole) data. Marker byte = (offset / PAGE) & 0xff. */
    uint64_t real_offs[8];
    for (int i = 0; i < 8; i++) {
        uint64_t off = (uint64_t)i * (WINDOW / 8) + (uint64_t)PAGE * 3;
        real_offs[i] = off;
        unsigned char b = (unsigned char)((off / (uint64_t)PAGE) & 0xff);
        ASSERT_EQ(pwrite(fd, &b, 1, (off_t)off), (ssize_t)1);
    }
    close(fd);

    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&test_cfg, "huge.bin", 0, WINDOW, NULL, NULL);
    ASSERT_NE(buf, NULL);
    ASSERT_EQ((uint64_t)buf->len, WINDOW);

    const unsigned char *w = (const unsigned char *)buf->addr;
    /* Touch the 8 real pages (must read back the marker) + 8 hole pages (must be
     * 0). ~16 pages resident at most, not 512 MiB. */
    volatile uint64_t sink = 0;
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(w[real_offs[i]], (unsigned char)((real_offs[i] / (uint64_t)PAGE) & 0xff));
        uint64_t hole = (uint64_t)i * (WINDOW / 8) + (uint64_t)PAGE * 100;  /* an untouched hole */
        ASSERT_EQ(w[hole], 0);
        sink += w[real_offs[i]] + w[hole];
    }
    (void)sink;

    struct rusage ru;
    ASSERT_EQ(getrusage(RUSAGE_SELF, &ru), 0);
    long rss_kb = ru.ru_maxrss;
#ifdef __APPLE__
    rss_kb /= 1024;               /* macOS/BSD report ru_maxrss in bytes, Linux in KiB */
#endif
    /* Touched ~16 pages of a 512 MiB window over a 2 GiB file. Peak RSS must be
     * nowhere near either. 256 MiB is half the window and 1/8 the file: a coarse
     * ceiling with enormous headroom over real demand-paged residency. */
    ASSERT_LT(rss_kb, (long)(256 * 1024));

    hl_cap_fs_munmap(buf);
    unlink(path);
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

/* A compute.async span set borrows and releases on a worker thread while the
 * event loop borrows (images, sync spans) and closes. The counts were plain
 * ints: concurrent updates got lost, and close() and the last release could
 * each decide to free. Four threads churning borrow/release must leave the
 * count exactly where it started, and the close that follows frees once. */
#include <pthread.h>

static void *borrow_churn(void *arg)
{
    HlMappedBuffer *buf = (HlMappedBuffer *)arg;
    for (int i = 0; i < 10000; i++) {
        hl_cap_fs_mmap_borrow(buf);
        hl_cap_fs_mmap_release(buf);
    }
    return NULL;
}

UTEST(hl_cap_fs, mmap_borrow_is_thread_safe)
{
    setup_fs();
    const char *data = "shared across threads";
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "mmap_borrow.txt", data, strlen(data), NULL), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap(&test_cfg, "mmap_borrow.txt", NULL, NULL);
    ASSERT_NE(buf, NULL);

    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(pthread_create(&th[i], NULL, borrow_churn, buf), 0);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);

    EXPECT_EQ(atomic_load(&buf->borrow_count), 0);
    EXPECT_EQ(atomic_load(&buf->refs), 1);       /* only the owner's */
    EXPECT_EQ(atomic_load(&buf->pending_free), 0);
    EXPECT_EQ(memcmp(buf->addr, data, strlen(data)), 0);

    hl_cap_fs_munmap(buf);   /* the owner's reference was the last: freed here */
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

/* ── Windowed mmap: pure geometry (mapped-spans cut 1) ──────────────────
 * hl_cap_fs_mmap_window_geometry is filesystem-free, so overflow / boundary
 * cases near UINT64_MAX are tested directly without giant files. */

UTEST(hl_cap_fs, mmap_geometry_offset_zero)
{
    uint64_t off, len, slop, eff; const char *err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(0, 100, 1000, 4096,
                                             &off, &len, &slop, &eff, &err), 0);
    ASSERT_EQ((int)off, 0);
    ASSERT_EQ((int)slop, 0);
    ASSERT_EQ((int)len, 4096);   /* round_up(0 + 100) */
    ASSERT_EQ((int)eff, 100);
}

UTEST(hl_cap_fs, mmap_geometry_cross_page_slop)
{
    uint64_t off, len, slop, eff; const char *err = NULL;
    /* offset 5000 with a 4096 page: base page 4096, slop 904, map spans two
     * pages to cover [5000,5100). */
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(5000, 100, 10000, 4096,
                                             &off, &len, &slop, &eff, &err), 0);
    ASSERT_EQ((int)off, 4096);
    ASSERT_EQ((int)slop, 904);
    ASSERT_EQ((int)eff, 100);
    ASSERT_EQ((int)len, 4096);   /* round_up(904 + 100 = 1004) */
}

UTEST(hl_cap_fs, mmap_geometry_page_multiple_offset)
{
    uint64_t off, len, slop, eff; const char *err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(8192, 5000, 20000, 4096,
                                             &off, &len, &slop, &eff, &err), 0);
    ASSERT_EQ((int)off, 8192);   /* already page-aligned */
    ASSERT_EQ((int)slop, 0);
    ASSERT_EQ((int)eff, 5000);
    ASSERT_EQ((int)len, 8192);   /* round_up(5000) = 2 pages */
}

UTEST(hl_cap_fs, mmap_geometry_eof_clamp)
{
    uint64_t off, len, slop, eff; const char *err = NULL;
    /* window runs past EOF: eff_len clamps to file_size - offset. */
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(900, 500, 1000, 4096,
                                             &off, &len, &slop, &eff, &err), 0);
    ASSERT_EQ((int)eff, 100);    /* 1000 - 900 */
    ASSERT_EQ((int)off, 0);
    ASSERT_EQ((int)slop, 900);
    ASSERT_EQ((int)len, 4096);   /* round_up(900 + 100 = 1000) */
}

UTEST(hl_cap_fs, mmap_geometry_rejections)
{
    uint64_t off, len, slop, eff; const char *err;
    /* empty window */
    err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(0, 0, 1000, 4096,
                                             &off, &len, &slop, &eff, &err), -1);
    ASSERT_STREQ(err, "empty_window");
    /* over the per-window cap */
    err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(0, HL_FS_MMAP_MAX_WINDOW_BYTES + 1,
                                             (uint64_t)1 << 40, 4096,
                                             &off, &len, &slop, &eff, &err), -1);
    ASSERT_STREQ(err, "window_too_large");
    /* offset at/after EOF maps nothing */
    err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(1000, 10, 1000, 4096,
                                             &off, &len, &slop, &eff, &err), -1);
    ASSERT_STREQ(err, "offset_past_eof");
    /* zero page size */
    err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(0, 10, 1000, 0,
                                             &off, &len, &slop, &eff, &err), -1);
    ASSERT_STREQ(err, "bad_page_size");
}

/* The geometry makes no power-of-two assumption: an exotic page size computes
 * correctly via modulo/division rather than being rejected. */
UTEST(hl_cap_fs, mmap_geometry_non_power_of_two_page)
{
    uint64_t off, len, slop, eff; const char *err = NULL;
    /* page 1000: offset 2500 -> base 2000, slop 500, need 500+100=600 -> 1 page. */
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(2500, 100, 10000, 1000,
                                             &off, &len, &slop, &eff, &err), 0);
    ASSERT_EQ((int)off, 2000);
    ASSERT_EQ((int)slop, 500);
    ASSERT_EQ((int)eff, 100);
    ASSERT_EQ((int)len, 1000);
    /* a window that spans two 1000-byte pages rounds up to 2000. */
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(2500, 700, 10000, 1000,
                                             &off, &len, &slop, &eff, &err), 0);
    ASSERT_EQ((int)len, 2000);   /* round_up(500 + 700 = 1200) */
}

/* The EOF clamp bottoms out at 1 byte, never 0: a request at the last byte with
 * a huge length still maps a non-empty (one-page) region. */
UTEST(hl_cap_fs, mmap_geometry_eof_min_one_byte)
{
    uint64_t off, len, slop, eff; const char *err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(4999, 1000000, 5000, 4096,
                                             &off, &len, &slop, &eff, &err), 0);
    ASSERT_EQ((int)eff, 1);       /* 5000 - 4999, clamped */
    ASSERT_TRUE(len >= 4096);     /* non-zero mapping */
    ASSERT_TRUE(eff >= 1);
}

UTEST(hl_cap_fs, mmap_geometry_overflow_near_uint64_max)
{
    uint64_t off, len, slop, eff; const char *err = NULL;
    /* offset just below UINT64_MAX with a maximal file_size: eff_len clamps to
     * the few remaining bytes, but the page-rounded mapping's base+len wraps the
     * 64-bit address space -> must fail closed, not silently overflow. */
    uint64_t huge = UINT64_MAX;
    ASSERT_EQ(hl_cap_fs_mmap_window_geometry(huge - 10, 1000, huge, 4096,
                                             &off, &len, &slop, &eff, &err), -1);
    ASSERT_STREQ(err, "align_overflow");
}

/* ── Windowed mmap: real files (mapped-spans cut 1) ─────────────────────── */

/* Write `n` bytes where byte i == (i & 0xff), so a window at `off` is verifiable
 * against the deterministic pattern. */
static int write_pattern(const char *name, size_t n)
{
    unsigned char *p = (unsigned char *)malloc(n);
    if (!p) return -1;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(i & 0xff);
    int rc = hl_cap_fs_write(&test_cfg, name, (const char *)p, n, NULL);
    free(p);
    return rc;
}

static void assert_window_invariants(int *utest_result, HlMappedBuffer *buf,
                                     uint64_t offset, size_t eff_len)
{
    /* The mapping is aligned to the mmap GRANULARITY, which is not the page
     * size everywhere: Windows maps views at 64 KiB boundaries while
     * sysconf(_SC_PAGESIZE) still reports 4096. Ask the same source the
     * implementation does, or these invariants encode a Linux assumption. */
    uint64_t gran = hl_cap_fs_mmap_granularity();
    uint64_t pagemask = gran - 1;

    ASSERT_NE(buf, NULL);
    ASSERT_EQ(buf->foffset, offset);
    ASSERT_EQ(buf->len, eff_len);
    ASSERT_EQ(buf->closed, 0);
    /* the mmap base is page-aligned; the window sits `slop` bytes into it */
    ASSERT_EQ((uint64_t)(uintptr_t)buf->map_base & pagemask, (uint64_t)0);
    uint64_t slop = offset - (offset & ~pagemask);
    ASSERT_EQ((size_t)((char *)buf->addr - (char *)buf->map_base), (size_t)slop);
    /* the window lies wholly inside the page-aligned mapping */
    ASSERT_TRUE((char *)buf->addr >= (char *)buf->map_base);
    ASSERT_TRUE((char *)buf->addr + buf->len
                <= (char *)buf->map_base + buf->map_len);
    /* the bytes are exactly the file's window (pattern: byte k == k & 0xff) */
    const unsigned char *w = (const unsigned char *)buf->addr;
    for (size_t j = 0; j < eff_len; j++)
        ASSERT_EQ((int)w[j], (int)((offset + j) & 0xff));
}

UTEST(hl_cap_fs, mmap_window_cross_page)
{
    setup_fs();
    ASSERT_EQ(write_pattern("win_cross.bin", 40000), 0);
    /* offset 20000 forces a non-zero page-aligned base on both 4K and 16K
     * pages (20000 & ~4095 == 20000 & ~16383 == 16384). */
    HlMappedBuffer *buf =
        hl_cap_fs_mmap_window(&test_cfg, "win_cross.bin", 20000, 100, NULL, NULL);
    assert_window_invariants(utest_result, buf, 20000, 100);
    hl_cap_fs_munmap(buf);
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_window_offset_zero)
{
    setup_fs();
    ASSERT_EQ(write_pattern("win_zero.bin", 8192), 0);
    HlMappedBuffer *buf =
        hl_cap_fs_mmap_window(&test_cfg, "win_zero.bin", 0, 256, NULL, NULL);
    assert_window_invariants(utest_result, buf, 0, 256);
    hl_cap_fs_munmap(buf);
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_window_eof_clamp)
{
    setup_fs();
    ASSERT_EQ(write_pattern("win_eof.bin", 5000), 0);
    /* ask for 4000 bytes starting at 4000; only 1000 remain -> clamp to 1000,
     * and reading the whole (clamped) window must not SIGBUS. */
    HlMappedBuffer *buf =
        hl_cap_fs_mmap_window(&test_cfg, "win_eof.bin", 4000, 4000, NULL, NULL);
    assert_window_invariants(utest_result, buf, 4000, 1000);
    hl_cap_fs_munmap(buf);
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_window_borrow_defers_close)
{
    setup_fs();
    ASSERT_EQ(write_pattern("win_borrow.bin", 40000), 0);
    HlMappedBuffer *buf =
        hl_cap_fs_mmap_window(&test_cfg, "win_borrow.bin", 20000, 100, NULL, NULL);
    ASSERT_NE(buf, NULL);
    void *map_base = buf->map_base; /* teardown must unmap THIS, not addr */

    hl_cap_fs_mmap_borrow(buf);
    ASSERT_EQ(buf->borrow_count, 1);
    hl_cap_fs_munmap(buf);            /* deferred while borrowed */
    ASSERT_EQ(buf->pending_free, 1);
    ASSERT_EQ(buf->closed, 0);
    ASSERT_EQ((int)((const unsigned char *)buf->addr)[0], (int)(20000 & 0xff));
    ASSERT_EQ(buf->map_base, map_base);

    hl_cap_fs_mmap_release(buf);      /* last release -> real munmap(map_base) + free */
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_window_rejections)
{
    setup_fs();
    ASSERT_EQ(write_pattern("win_rej.bin", 4096), 0);
    const char *err;
    /* offset past EOF */
    err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window(&test_cfg, "win_rej.bin", 4096, 10, NULL, &err),
              NULL);
    ASSERT_STREQ(err, "offset_past_eof");
    /* empty window */
    err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window(&test_cfg, "win_rej.bin", 0, 0, NULL, &err),
              NULL);
    ASSERT_STREQ(err, "empty_window");
    /* over the per-window cap */
    err = NULL;
    ASSERT_EQ(hl_cap_fs_mmap_window(&test_cfg, "win_rej.bin", 0,
                                    HL_FS_MMAP_MAX_WINDOW_BYTES + 1, NULL, &err),
              NULL);
    ASSERT_STREQ(err, "window_too_large");
    teardown_fs();
}

UTEST(hl_cap_fs, mmap_window_path_traversal)
{
    setup_fs();
    ASSERT_EQ(hl_cap_fs_mmap_window(&test_cfg, "../etc/passwd", 0, 10, NULL, NULL),
              NULL);
    ASSERT_EQ(hl_cap_fs_mmap_window(&test_cfg, "/etc/passwd", 0, 10, NULL, NULL),
              NULL);
    teardown_fs();
}

/* Whole-file mmap must keep the window fields self-consistent so the shared
 * munmap/borrow path is uniform: map_base==addr, map_len==len, foffset==0. */
UTEST(hl_cap_fs, mmap_whole_file_window_fields)
{
    setup_fs();
    const char *data = "whole file window fields";
    ASSERT_EQ(hl_cap_fs_write(&test_cfg, "whole.txt", data, strlen(data), NULL), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap(&test_cfg, "whole.txt", NULL, NULL);
    ASSERT_NE(buf, NULL);
    ASSERT_EQ(buf->map_base, buf->addr);
    ASSERT_EQ(buf->map_len, buf->len);
    ASSERT_EQ(buf->foffset, (uint64_t)0);
    hl_cap_fs_munmap(buf);
    teardown_fs();
}

/* ── Audit 33: database files SQLite holds open ─────────────────────── */

/* Make sub/live.db + its -wal / -shm with known content, register the
 * database as a live SQLite file, and alias the -shm through an in-root
 * symlink (the route the in-place write takes). */
static void live_db_setup(char *db_abs, size_t sz)
{
    setup_fs();
    static const char *const names[] = { "live.db", "live.db-wal", "live.db-shm" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/sub/%s", test_dir, names[i]);
        FILE *f = fopen(p, "wb");
        if (f) { fputs("keep", f); fclose(f); }
    }
    char p[512];
    snprintf(p, sizeof(p), "%s/sub/shmalias", test_dir);
    (void)symlink("live.db-shm", p);
    snprintf(db_abs, sz, "%s/sub/live.db", test_dir);
    hl_db_live_file_add(db_abs);
}

static long long sub_size(const char *leaf)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/sub/%s", test_dir, leaf);
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

/* compute.stream's file output opens through hl_cap_fs_open_write_fd and
 * truncated the -shm SQLite maps MAP_SHARED: the next query died of SIGBUS. */
UTEST(hl_cap_fs, the_write_open_refuses_a_live_sqlite_sidecar)
{
    if (hl_host_is_windows()) return;
    char db[512];
    live_db_setup(db, sizeof db);
    const char *err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_open_write_fd(&test_cfg, "sub/live.db-shm", &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_open_write_fd(&test_cfg, "sub/live.db", &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    EXPECT_EQ(4LL, sub_size("live.db-shm"));   /* not truncated */
    EXPECT_EQ(4LL, sub_size("live.db"));
    hl_db_live_file_remove(db);
    teardown_fs();
}

/* fs.write through an in-root symlink writes in place: refused by inode. */
UTEST(hl_cap_fs, an_in_place_write_refuses_a_live_sqlite_sidecar)
{
    if (hl_host_is_windows()) return;
    char db[512];
    live_db_setup(db, sizeof db);
    const char *err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_write(&test_cfg, "sub/shmalias", "y", 1, &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    EXPECT_EQ(4LL, sub_size("live.db-shm"));
    hl_db_live_file_remove(db);
    teardown_fs();
}

/* Replacing (rename) or deleting a live database or sidecar is refused too. */
UTEST(hl_cap_fs, replace_and_delete_refuse_a_live_sqlite_file)
{
    if (hl_host_is_windows()) return;
    char db[512];
    live_db_setup(db, sizeof db);
    struct stat before, after;
    ASSERT_EQ(host_path_stat("sub/live.db-wal", &before), 0);
    const char *err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_write(&test_cfg, "sub/live.db-wal", "x", 1, &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_write(&test_cfg, "sub/live.db", "x", 1, &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    ASSERT_EQ(host_path_stat("sub/live.db-wal", &after), 0);
    EXPECT_EQ((long long)before.st_ino, (long long)after.st_ino);
    EXPECT_FALSE(temp_left("sub"));
    err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_delete(&test_cfg, "sub/live.db-shm", &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    EXPECT_EQ(4LL, sub_size("live.db-shm"));
    /* The link is only a name: removing it is fine. */
    EXPECT_EQ(0, hl_cap_fs_delete(&test_cfg, "sub/shmalias", NULL));

    /* Once the connection is gone the files are ordinary again. */
    hl_db_live_file_remove(db);
    EXPECT_EQ(0, hl_cap_fs_write(&test_cfg, "sub/live.db", "x", 1, NULL));
    EXPECT_EQ(0, hl_cap_fs_delete(&test_cfg, "sub/live.db-wal", NULL));
    teardown_fs();
}

/* A sidecar NAME is refused for an in-place write even with no live
 * connection; an atomic write of such a name is an ordinary new file. */
UTEST(hl_cap_fs, sidecar_names_are_refused_in_place_only)
{
    setup_fs();
    const char *err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_open_write_fd(&test_cfg, "sub/other.db-WAL", &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    EXPECT_EQ(0, hl_cap_fs_write(&test_cfg, "sub/travel-journal", "j", 1, NULL));
    EXPECT_EQ(1, hl_db_sidecar_name("a/b.db-journal"));
    EXPECT_EQ(1, hl_db_sidecar_name("x-shm"));
    EXPECT_EQ(0, hl_db_sidecar_name("shm"));
    EXPECT_EQ(0, hl_db_sidecar_name("a-wal/b.txt"));
    teardown_fs();
}

/* The registry is refcounted by path: one connection closing leaves the
 * file live for the others. */
UTEST(hl_cap_fs, live_db_registry_is_refcounted)
{
    if (hl_host_is_windows()) return;
    char db[512];
    live_db_setup(db, sizeof db);
    hl_db_live_file_add(db);                    /* a second connection */
    hl_db_live_file_remove(db);
    struct stat st;
    ASSERT_EQ(host_path_stat("sub/live.db", &st), 0);
    EXPECT_EQ(1, hl_db_live_file_match(&st));
    hl_db_live_file_remove(db);
    EXPECT_EQ(0, hl_db_live_file_match(&st));
    teardown_fs();
}

/* ── Audit 34: registered before mapped; the check atomic with the write ── */
#ifdef HL_ENABLE_SQLITE

/* A VFS shim over the default one that, when SQLite opens a database's WAL
 * or rollback journal (the step that leads to the -shm mapping), records
 * whether the database was already registered. Registered after
 * hl_cap_db_init, it was not: `PRAGMA journal_mode=WAL` inside the init
 * opened the WAL and mapped the -shm while the fs capability still saw an
 * ordinary file (audit 34). */
static sqlite3_vfs  shim_vfs;
static sqlite3_vfs *shim_orig;
static int          shim_side_opens;
static int          shim_unregistered;

static int shim_open(sqlite3_vfs *vfs, sqlite3_filename name,
                     sqlite3_file *file, int flags, int *out_flags)
{
    (void)vfs;
    if (name && (flags & (SQLITE_OPEN_WAL | SQLITE_OPEN_MAIN_JOURNAL))) {
        const char *sfx = (flags & SQLITE_OPEN_WAL) ? "-wal" : "-journal";
        size_t n = strlen(name), k = strlen(sfx);
        char db[1024];
        if (n > k && n - k < sizeof db && strcmp(name + n - k, sfx) == 0) {
            memcpy(db, name, n - k);
            db[n - k] = '\0';
            struct stat st;
            shim_side_opens++;
            if (stat(db, &st) != 0 || !hl_db_live_file_match(&st))
                shim_unregistered++;
        }
    }
    return shim_orig->xOpen(shim_orig, name, file, flags, out_flags);
}

static int live_open(HlDbHandle *h, const char *path)
{
    void *ctx = NULL;
    memset(h, 0, sizeof *h);
    if (hl_db_backend_sqlite.open(&ctx, path, NULL) != 0) return -1;
    h->backend = &hl_db_backend_sqlite;
    h->ctx = ctx;
    return 0;
}

UTEST(hl_cap_fs, a_sqlite_file_is_registered_before_its_wal_opens)
{
    if (hl_host_is_windows()) return;
    setup_fs();
    hl_cap_db_sqlite_setup();          /* before sqlite3_vfs_register initializes */
    shim_orig = sqlite3_vfs_find(NULL);
    ASSERT_TRUE(shim_orig != NULL);
    shim_vfs = *shim_orig;
    shim_vfs.zName = "hull-test-live-shim";
    shim_vfs.pNext = NULL;
    shim_vfs.xOpen = shim_open;
    ASSERT_EQ(SQLITE_OK, sqlite3_vfs_register(&shim_vfs, 1));
    shim_side_opens = shim_unregistered = 0;

    char db[512];
    snprintf(db, sizeof db, "%s/sub/order.db", test_dir);
    HlDbHandle h;
    int opened = live_open(&h, db);
    if (opened == 0) {
        EXPECT_TRUE(hl_db_backend_sqlite.exec(&h, "CREATE TABLE t(x)", NULL, 0) >= 0);
        EXPECT_TRUE(hl_db_backend_sqlite.exec(&h, "INSERT INTO t VALUES (1)", NULL, 0) >= 0);
        hl_db_backend_sqlite.close(&h);
    }
    sqlite3_vfs_unregister(&shim_vfs);
    sqlite3_vfs_register(shim_orig, 1);

    ASSERT_EQ(0, opened);
    EXPECT_TRUE(shim_side_opens > 0);
    EXPECT_EQ(0, shim_unregistered);
    /* And unregistered once closed. */
    struct stat st;
    ASSERT_EQ(0, stat(db, &st));
    EXPECT_EQ(0, hl_db_live_file_match(&st));
    teardown_fs();
}

/* The race itself: a thread opens, uses and closes a WAL database over and
 * over (db.async on a db.open DSN, an evicted worker connection) while this
 * thread writes in place through an in-root symlink to its -shm. Before
 * audit 34 the write truncated the index a just-opened connection had mapped
 * and the process died of SIGBUS within a second. Run in a child so a crash
 * is a failed assertion, not a dead test binary. */
typedef struct {
    char path[512];
    volatile int stop;
    int cycles;
} LiveRace;

static void *live_race_thread(void *arg)
{
    LiveRace *r = arg;
    while (!r->stop) {
        HlDbHandle h;
        if (live_open(&h, r->path) != 0) continue;
        (void)hl_db_backend_sqlite.exec(&h, "INSERT INTO t VALUES (1)", NULL, 0);
        hl_db_backend_sqlite.close(&h);
        r->cycles++;
    }
    return NULL;
}

UTEST(hl_cap_fs, a_write_racing_sqlite_opens_never_truncates_a_mapped_index)
{
    if (hl_host_is_windows()) return;
    setup_fs();
    char db[512];
    snprintf(db, sizeof db, "%s/sub/race.db", test_dir);
    {
        HlDbHandle h;
        ASSERT_EQ(0, live_open(&h, db));
        ASSERT_TRUE(hl_db_backend_sqlite.exec(&h, "CREATE TABLE t(x)", NULL, 0) >= 0);
        hl_db_backend_sqlite.close(&h);
    }
    char link[512];
    snprintf(link, sizeof link, "%s/sub/racealias", test_dir);
    ASSERT_EQ(0, symlink("race.db-shm", link));

    fflush(NULL);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        static LiveRace r;
        snprintf(r.path, sizeof r.path, "%s", db);
        pthread_t th;
        if (pthread_create(&th, NULL, live_race_thread, &r) != 0) _exit(10);
        struct timespec t0, now;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long refused = 0, written = 0;
        do {
            const char *err = NULL;
            if (hl_cap_fs_write(&test_cfg, "sub/racealias", "y", 1, &err) == 0)
                written++;
            else if (err && strcmp(err, HL_DB_LIVE_FILE_ERR) == 0)
                refused++;
            int fd = hl_cap_fs_open_write_fd(&test_cfg, "sub/racealias", &err);
            if (fd >= 0) { written++; close(fd); }
            else if (err && strcmp(err, HL_DB_LIVE_FILE_ERR) == 0) refused++;
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while ((now.tv_sec - t0.tv_sec) * 1000L
                 + (now.tv_nsec - t0.tv_nsec) / 1000000L < 1500);
        r.stop = 1;
        pthread_join(th, NULL);
        /* Surviving is the point. Every write here is refused (the alias's
         * real leaf is named like a sidecar); the registry alone would let
         * one through while no connection is open, which is harmless. */
        (void)written;
        if (refused == 0 || r.cycles == 0) _exit(3);
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status));
    if (WIFSIGNALED(status))
        fprintf(stderr, "race child died of signal %d\n", WTERMSIG(status));
    EXPECT_EQ(0, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    teardown_fs();
}

/* The real leaf an alias reaches is checked by name too: a symlink to some
 * other process's (or a not-yet-open) `x.db-shm` is refused in place. */
UTEST(hl_cap_fs, an_in_place_write_refuses_an_alias_to_a_sidecar_name)
{
    if (hl_host_is_windows()) return;
    setup_fs();
    char p[512];
    snprintf(p, sizeof p, "%s/sub/other.db-shm", test_dir);
    FILE *f = fopen(p, "wb");
    if (f) { fputs("keep", f); fclose(f); }
    snprintf(p, sizeof p, "%s/sub/otheralias", test_dir);
    ASSERT_EQ(0, symlink("other.db-shm", p));
    const char *err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_open_write_fd(&test_cfg, "sub/otheralias", &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    err = NULL;
    EXPECT_EQ(-1, hl_cap_fs_write(&test_cfg, "sub/otheralias", "y", 1, &err));
    EXPECT_STREQ(HL_DB_LIVE_FILE_ERR, err ? err : "");
    EXPECT_EQ(4LL, sub_size("other.db-shm"));
    /* An alias to an ordinary file still writes through. */
    snprintf(p, sizeof p, "%s/sub/plain.txt", test_dir);
    f = fopen(p, "wb");
    if (f) { fputs("keep", f); fclose(f); }
    snprintf(p, sizeof p, "%s/sub/plainalias", test_dir);
    ASSERT_EQ(0, symlink("plain.txt", p));
    EXPECT_EQ(0, hl_cap_fs_write(&test_cfg, "sub/plainalias", "y", 1, NULL));
    EXPECT_EQ(1LL, sub_size("plain.txt"));
    teardown_fs();
}

#endif /* HL_ENABLE_SQLITE */

/* ── Audit 42 ─────────────────────────────────────────────────────────── */

/* `.hull` is reserved once the app root is registered by path, with no fs
 * policy compiled: a planted in-root symlink to it (templates -> .hull)
 * cannot be followed by the dev-mode disk loaders (hl_fs_fopen_read). */
UTEST(fs_audit42, hull_reserved_without_a_policy)
{
    char dir[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(dir, sizeof dir, "a42res") != NULL);
    char p[HL_TEST_PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/.hull", dir);
    ASSERT_EQ(0, mkdir(p, 0700));
    snprintf(p, sizeof p, "%s/.hull/secret.html", dir);
    FILE *w = fopen(p, "w");
    ASSERT_TRUE(w != NULL);
    fputs("SECRET", w);
    fclose(w);
    snprintf(p, sizeof p, "%s/templates", dir);
    ASSERT_EQ(0, symlink(".hull", p));
    snprintf(p, sizeof p, "%s/ok.txt", dir);
    w = fopen(p, "w");
    ASSERT_TRUE(w != NULL);
    fputs("ok", w);
    fclose(w);

    ASSERT_EQ(0, hl_fs_reserve_root_path(dir));

    FILE *f = hl_fs_fopen_read(dir, "templates/secret.html");
    EXPECT_TRUE(f == NULL);
    if (f) fclose(f);
    f = hl_fs_fopen_read(dir, ".hull/secret.html");
    EXPECT_TRUE(f == NULL);
    if (f) fclose(f);
    f = hl_fs_fopen_read(dir, ".HULL/secret.html");
    EXPECT_TRUE(f == NULL);
    if (f) fclose(f);
    /* The rest of the tree is unaffected. */
    f = hl_fs_fopen_read(dir, "ok.txt");
    EXPECT_TRUE(f != NULL);
    if (f) fclose(f);

    EXPECT_EQ(-1, hl_fs_reserve_root_path(NULL));
    EXPECT_EQ(-1, hl_fs_reserve_root_path(p));   /* a file, not a directory */
    (void)nftw(dir, teardown_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* fs_write_atomic's temp opens O_CREAT|O_EXCL|O_NOFOLLOW: an existing file
 * or a symlink at the name is never opened, truncated or written through. */
UTEST(fs_audit42, write_excl_temp_never_opens_an_existing_name)
{
    char dir[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(dir, sizeof dir, "a42excl") != NULL);
    char p[HL_TEST_PATH_MAX + 64], victim[HL_TEST_PATH_MAX + 64];
    snprintf(victim, sizeof victim, "%s/victim", dir);
    FILE *w = fopen(victim, "w");
    ASSERT_TRUE(w != NULL);
    fputs("KEEP", w);
    fclose(w);
    snprintf(p, sizeof p, "%s/.hull-tmp-planted", dir);
    ASSERT_EQ(0, symlink("victim", p));

    const char *err = NULL;
    int root = hl_fs_open_base(dir, &err);
    ASSERT_GE(root, 0);
    /* Symlink at the name: refused, the target untouched. */
    int fd = hl_fs_open_at_ex(root, ".hull-tmp-planted", HL_FS_OPEN_WRITE_EXCL,
                              HL_FS_SYMLINK_FOLLOW, 0644, &err);
    EXPECT_LT(fd, 0);
    if (fd >= 0) close(fd);
    /* An existing regular file: refused, not truncated. */
    err = NULL;
    fd = hl_fs_open_at_ex(root, "victim", HL_FS_OPEN_WRITE_EXCL,
                          HL_FS_SYMLINK_FOLLOW, 0644, &err);
    EXPECT_LT(fd, 0);
    if (fd >= 0) close(fd);
    struct stat st;
    ASSERT_EQ(0, stat(victim, &st));
    EXPECT_EQ((long long)st.st_size, 4LL);
    /* A new name (parents made) opens. */
    err = NULL;
    fd = hl_fs_open_at_ex(root, "sub/new", HL_FS_OPEN_WRITE_EXCL,
                          HL_FS_SYMLINK_FOLLOW, 0644, &err);
    EXPECT_GE(fd, 0);
    if (fd >= 0) close(fd);
    close(root);
    (void)nftw(dir, teardown_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST_MAIN();
