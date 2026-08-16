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
#include <fcntl.h>
#include <ftw.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
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

/* Demand paging: mapping a large (2 GiB) SPARSE file as a 512 MiB window and
 * touching only a handful of pages must NOT make the file/window resident — the
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
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    uint64_t pagemask = (uint64_t)pg - 1;

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

UTEST_MAIN();
