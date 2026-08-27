/* FTW_DEPTH / FTW_PHYS are XSI extensions to nftw; on glibc they're only
 * declared when _XOPEN_SOURCE >= 500. macOS exposes them unconditionally AND
 * uses _XOPEN_SOURCE to gate Darwin extensions the other way (defining it hides
 * clock_gettime_nsec_np / CLOCK_UPTIME_RAW that utest.h needs), so this define
 * has to stay Linux-only. */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

/*
 * test_tar.c - Unit tests for the shared ustar core (include/hull/cap/tar.h):
 *   - hl_tar_parse()   pure iteration; name normalization; traversal rejection.
 *   - hl_tar_extract() trusted extraction to a directory (files, nested dirs,
 *                      exec-bit preservation, real system-tar interop).
 *   - hl_tar_create()  round-trips through hl_tar_parse; rejects unsafe names.
 *
 * The same core backs `hull tools install` (bundle extraction) and the
 * `hull.tar` stdlib module (parse/create). Extraction tests run in a per-test
 * tempdir so the real filesystem is never touched.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/tar.h"

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── ustar test-vector builders ─────────────────────────────────────── */

/* Minimal ustar header (name, size, regular-file typeflag). The reader ignores
 * the checksum field, so we leave it blank. */
static void tar_put_header(unsigned char *b, const char *name, size_t size) {
    memset(b, 0, 512);
    strncpy((char *)b, name, 99);
    snprintf((char *)(b + 100), 8, "%07o", 0755);              /* mode 0755 */
    snprintf((char *)(b + 124), 12, "%011o", (unsigned)size);  /* size, octal */
    memset(b + 148, ' ', 8);                                   /* chksum: blank */
    b[156] = '0';                                              /* regular file */
}

/* A ustar directory entry (typeflag '5', no data). */
static void tar_add_dir(unsigned char *buf, size_t *off, const char *name) {
    tar_put_header(buf + *off, name, 0);
    buf[*off + 156] = '5';   /* directory */
    *off += 512;
}

static void tar_add_file(unsigned char *buf, size_t *off,
                         const char *name, const char *data, size_t len) {
    tar_put_header(buf + *off, name, len);
    *off += 512;
    memcpy(buf + *off, data, len);
    *off += (len + 511) & ~(size_t)511;
}

/* A ustar symlink entry (typeflag '2', target in the linkname field @ 157). */
static void tar_add_symlink(unsigned char *buf, size_t *off,
                            const char *name, const char *target) {
    tar_put_header(buf + *off, name, 0);
    buf[*off + 156] = '2';                          /* symlink typeflag */
    strncpy((char *)(buf + *off + 157), target, 99);
    *off += 512;
}

/* ── Fixture: per-test sandbox under /tmp ───────────────────────────── */

struct tar_fixture {
    char tmpdir[PATH_MAX];
    char saved_path[2048];
    int  had_path;
};

static int rm_recursive_entry(const char *path, const struct stat *sb,
                              int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

static int rm_recursive(const char *path) {
    if (nftw(path, rm_recursive_entry, 16, FTW_DEPTH | FTW_PHYS) != 0
        && errno != ENOENT) {
        return -1;
    }
    return 0;
}

UTEST_F_SETUP(tar_fixture) {
    snprintf(utest_fixture->tmpdir, sizeof(utest_fixture->tmpdir),
             "/tmp/hull-tar-test-%d", getpid());
    rm_recursive(utest_fixture->tmpdir);
    ASSERT_EQ(mkdir(utest_fixture->tmpdir, 0700), 0);

    const char *p = getenv("PATH");
    utest_fixture->had_path = p != NULL;
    if (p) snprintf(utest_fixture->saved_path, sizeof(utest_fixture->saved_path), "%s", p);
}

UTEST_F_TEARDOWN(tar_fixture) {
    (void)utest_result;
    if (utest_fixture->had_path) setenv("PATH", utest_fixture->saved_path, 1);
    else                         unsetenv("PATH");
    rm_recursive(utest_fixture->tmpdir);
}

/* ── hl_tar_parse ──────────────────────────────────────────────────── */

struct collect { char names[16][256]; unsigned modes[16]; int dirs[16]; int n; };

static int collect_cb(const HlTarEntry *e, void *ctx) {
    struct collect *c = (struct collect *)ctx;
    if (c->n >= 16) return 0;
    snprintf(c->names[c->n], sizeof(c->names[c->n]), "%s", e->name);
    c->modes[c->n] = e->mode;
    c->dirs[c->n] = e->is_dir;
    c->n++;
    return 0;
}

/* Stops iteration on the first entry (returns a sentinel non-zero value). */
static int stop_first_seen;
static int stop_first_cb(const HlTarEntry *e, void *ctx) {
    (void)e; (void)ctx;
    stop_first_seen++;
    return 42;
}

UTEST(tar_parse, iterates_files_and_dirs_skipping_root) {
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_dir(buf, &off, "./");                    /* root - skipped */
    tar_add_file(buf, &off, "./crt1.o", "OBJ1", 4);  /* leading ./ stripped */
    tar_add_dir(buf, &off, "lib/std");
    tar_add_file(buf, &off, "lib/std/foo", "SRC", 3);

    struct collect c = {0};
    ASSERT_EQ(hl_tar_parse(buf, off, collect_cb, &c), 0);
    ASSERT_EQ(c.n, 3);
    ASSERT_STREQ(c.names[0], "crt1.o");
    ASSERT_EQ(c.dirs[0], 0);
    ASSERT_STREQ(c.names[1], "lib/std");
    ASSERT_EQ(c.dirs[1], 1);
    ASSERT_STREQ(c.names[2], "lib/std/foo");
    free(buf);
}

UTEST(tar_parse, rejects_traversal) {
    unsigned char *buf = calloc(1, 4096);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "../escape", "X", 1);
    struct collect c = {0};
    ASSERT_EQ(hl_tar_parse(buf, off, collect_cb, &c), -1);
    free(buf);
}

UTEST(tar_parse, callback_can_stop_early) {
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "a", "1", 1);
    tar_add_file(buf, &off, "b", "2", 1);

    /* A callback returning non-zero halts iteration and that value is returned. */
    stop_first_seen = 0;
    ASSERT_EQ(hl_tar_parse(buf, off, stop_first_cb, NULL), 42);
    ASSERT_EQ(stop_first_seen, 1);
    free(buf);
}

/* ── hl_tar_extract ────────────────────────────────────────────────── */

UTEST_F(tar_fixture, extract_files) {
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "./crt1.o", "OBJ1", 4);        /* leading ./ stripped */
    tar_add_file(buf, &off, "libgcc.a", "ARCHIVE-BYTES", 13);
    off += 512;                                            /* zero terminator */

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/floor", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), 0);

    char p[PATH_MAX], rd[64];
    FILE *f;
    snprintf(p, sizeof(p), "%s/crt1.o", dest);
    f = fopen(p, "rb"); ASSERT_NE(f, NULL);
    size_t n = fread(rd, 1, sizeof(rd), f); fclose(f);
    ASSERT_EQ(n, (size_t)4);
    ASSERT_EQ(memcmp(rd, "OBJ1", 4), 0);

    snprintf(p, sizeof(p), "%s/libgcc.a", dest);
    f = fopen(p, "rb"); ASSERT_NE(f, NULL);
    n = fread(rd, 1, sizeof(rd), f); fclose(f);
    ASSERT_EQ(n, (size_t)13);
    ASSERT_EQ(memcmp(rd, "ARCHIVE-BYTES", 13), 0);
    free(buf);
}

/* Extract an archive produced by the SYSTEM `tar` (busybox / bsdtar / GNU),
 * the way scripts/build_musl_floor.sh does it - proves the producer/consumer
 * format contract, not just our hand-rolled headers. Skips if tar is absent. */
UTEST_F(tar_fixture, extract_real_tar) {
    char srcdir[PATH_MAX], f1[PATH_MAX], tarpath[PATH_MAX], cmd[PATH_MAX * 3];
    snprintf(srcdir, sizeof(srcdir), "%s/src", utest_fixture->tmpdir);
    ASSERT_EQ(mkdir(srcdir, 0755), 0);
    snprintf(f1, sizeof(f1), "%s/crt1.o", srcdir);
    FILE *w = fopen(f1, "wb");
    ASSERT_NE(w, NULL);
    fwrite("REALTARBYTES", 1, 12, w);
    fclose(w);

    snprintf(tarpath, sizeof(tarpath), "%s/floor.tar", utest_fixture->tmpdir);
    setenv("PATH", "/usr/bin:/bin", 1);
    snprintf(cmd, sizeof(cmd), "tar cf '%s' -C '%s' . 2>/dev/null", tarpath, srcdir);
    if (system(cmd) != 0) return;  /* no tar → skip (teardown restores PATH) */

    FILE *tf = fopen(tarpath, "rb");
    ASSERT_NE(tf, NULL);
    fseek(tf, 0, SEEK_END);
    long tlen = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    ASSERT_GT(tlen, 512);
    unsigned char *tbuf = malloc((size_t)tlen);
    ASSERT_NE(tbuf, NULL);
    ASSERT_EQ(fread(tbuf, 1, (size_t)tlen, tf), (size_t)tlen);
    fclose(tf);

    char dest[PATH_MAX], out[PATH_MAX], rd[32];
    snprintf(dest, sizeof(dest), "%s/floor", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(tbuf, (size_t)tlen, dest), 0);
    snprintf(out, sizeof(out), "%s/crt1.o", dest);
    FILE *rf = fopen(out, "rb");
    ASSERT_NE(rf, NULL);
    size_t n = fread(rd, 1, sizeof(rd), rf);
    fclose(rf);
    ASSERT_EQ(n, (size_t)12);
    ASSERT_EQ(memcmp(rd, "REALTARBYTES", 12), 0);
    free(tbuf);
}

UTEST_F(tar_fixture, extract_creates_missing_parent_dirs) {
    /* dest_dir's PARENT doesn't exist yet (mirrors `hull tools install <bundle>`
     * into a fresh $HOME, extracting to ~/.hull/tools/<name>/ before anything
     * created ~/.hull/tools). hl_tar_extract must mkdir -p, not single-mkdir. */
    unsigned char *buf = calloc(1, 4096);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "driver", "BIN", 3);
    off += 512;

    char dest[PATH_MAX];
    /* Two levels of not-yet-existing parents under the tmp root. */
    snprintf(dest, sizeof(dest), "%s/a/b/c", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), 0);

    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof(p), "%s/driver", dest);
    ASSERT_EQ(stat(p, &st), 0);
    free(buf);
}

UTEST_F(tar_fixture, extract_rejects_traversal) {
    unsigned char *buf = calloc(1, 4096);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "../escape.o", "X", 1);
    off += 512;
    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/floor2", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), -1);   /* traversal refused */
    free(buf);
}

UTEST_F(tar_fixture, extract_nested_tree) {
    /* A zig-like nested tree: root dir entry (skipped), a top-level executable,
     * and a file two directories deep with an explicit dir entry. */
    unsigned char *buf = calloc(1, 16384);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_dir(buf, &off, "./");
    tar_add_file(buf, &off, "./zig", "BINARY", 6);
    tar_add_dir(buf, &off, "lib/std");
    tar_add_file(buf, &off, "lib/std/foo.zig", "SRC", 3);
    off += 512;

    char dest[PATH_MAX], p[PATH_MAX];
    struct stat st;
    snprintf(dest, sizeof(dest), "%s/tree", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), 0);

    snprintf(p, sizeof(p), "%s/zig", dest);
    ASSERT_EQ(stat(p, &st), 0);
    ASSERT_TRUE(st.st_mode & S_IXUSR);            /* exec bit preserved */
    snprintf(p, sizeof(p), "%s/lib/std/foo.zig", dest);
    ASSERT_EQ(stat(p, &st), 0);                   /* deep nested file created */
    free(buf);
}

UTEST_F(tar_fixture, extract_rejects_nested_traversal) {
    unsigned char *buf = calloc(1, 4096);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "lib/../../escape", "X", 1);
    off += 512;
    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/tree2", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), -1);   /* nested ".." refused */
    free(buf);
}

/* Spec item B: a symlink member extracts as a link where supported, else a copy
 * of its (already-extracted) target. Either way the path resolves to the target
 * content - the cosmocc arch-cc -> cosmocc case. */
UTEST_F(tar_fixture, extract_symlink_resolves_to_target) {
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "bin/cosmocc", "DRIVER-BYTES", 12);
    tar_add_symlink(buf, &off, "bin/x86_64-cc", "cosmocc");  /* same-dir target */
    off += 512;

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/cc", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), 0);

    /* The link path resolves to the target's bytes (link or copy fallback). */
    char p[PATH_MAX], rd[64];
    snprintf(p, sizeof(p), "%s/bin/x86_64-cc", dest);
    FILE *f = fopen(p, "rb"); ASSERT_NE(f, NULL);
    size_t n = fread(rd, 1, sizeof(rd), f); fclose(f);
    ASSERT_EQ(n, (size_t)12);
    ASSERT_EQ(memcmp(rd, "DRIVER-BYTES", 12), 0);

    /* On this POSIX host it is a real symlink pointing at the same-dir target. */
    struct stat st;
    ASSERT_EQ(lstat(p, &st), 0);
    ASSERT_TRUE(S_ISLNK(st.st_mode));
    char tgt[64];
    ssize_t ln = readlink(p, tgt, sizeof(tgt) - 1);
    ASSERT_GT(ln, (ssize_t)0);
    tgt[ln] = '\0';
    ASSERT_STREQ(tgt, "cosmocc");
    free(buf);
}

/* A symlink target may use ".." AS LONG AS it stays within the extraction
 * root: `bin/foo -> ../libexec/.../foo` (cosmocc's ld.bfd / as wrappers point
 * bin -> libexec). Regression for hull tools install cosmocc, which returned
 * rc=1 ("failed to extract") when the old blanket-".."-reject aborted the
 * symlink pass on these legitimate intra-bundle links. */
UTEST_F(tar_fixture, extract_symlink_in_root_dotdot_ok) {
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "libexec/ld.bfd", "REAL-LD-BYTES", 13);
    tar_add_symlink(buf, &off, "bin/x-ld.bfd", "../libexec/ld.bfd");  /* in-root .. */
    off += 512;

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/cc2", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), 0);        /* no longer refused */

    /* The link resolves (link or copy fallback) to the real file's bytes. */
    char p[PATH_MAX], rd[64];
    snprintf(p, sizeof(p), "%s/bin/x-ld.bfd", dest);
    FILE *f = fopen(p, "rb"); ASSERT_NE(f, NULL);
    size_t n = fread(rd, 1, sizeof(rd), f); fclose(f);
    ASSERT_EQ(n, (size_t)13);
    ASSERT_EQ(memcmp(rd, "REAL-LD-BYTES", 13), 0);

    /* On this POSIX host it is a real symlink carrying the ".." target verbatim. */
    struct stat st;
    ASSERT_EQ(lstat(p, &st), 0);
    ASSERT_TRUE(S_ISLNK(st.st_mode));
    char tgt[64];
    ssize_t ln = readlink(p, tgt, sizeof(tgt) - 1);
    ASSERT_GT(ln, (ssize_t)0);
    tgt[ln] = '\0';
    ASSERT_STREQ(tgt, "../libexec/ld.bfd");
    free(buf);
}

/* Windows non-admin (no SeCreateSymbolicLinkPrivilege, Developer Mode off):
 * symlink() fails, so an archive link is materialized from its target - PREFER
 * a hardlink (no data copy, no privilege), never left as a symlink. Forced with
 * HL_TAR_NO_SYMLINK so the fallback runs on this POSIX host too. This is the
 * cosmocc arch-cc -> cosmocc case on a locked-down Windows box. */
UTEST_F(tar_fixture, extract_symlink_fallback_prefers_hardlink) {
    setenv("HL_TAR_NO_SYMLINK", "1", 1);
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "bin/cosmocc", "DRIVER-BYTES", 12);
    tar_add_symlink(buf, &off, "bin/x86_64-cc", "cosmocc");  /* same-dir target */
    off += 512;

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/cc", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), 0);

    char p[PATH_MAX], tp[PATH_MAX], rd[64];
    snprintf(p, sizeof(p), "%s/bin/x86_64-cc", dest);
    snprintf(tp, sizeof(tp), "%s/bin/cosmocc", dest);

    /* NOT a symlink - a materialized regular file resolving to target bytes. */
    struct stat st, ts;
    ASSERT_EQ(lstat(p, &st), 0);
    ASSERT_FALSE(S_ISLNK(st.st_mode));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    FILE *f = fopen(p, "rb"); ASSERT_NE(f, NULL);
    size_t n = fread(rd, 1, sizeof(rd), f); fclose(f);
    ASSERT_EQ(n, (size_t)12);
    ASSERT_EQ(memcmp(rd, "DRIVER-BYTES", 12), 0);

    /* Preferred a HARDLINK: same inode as the target (a copy would differ). */
    ASSERT_EQ(stat(tp, &ts), 0);
    ASSERT_EQ(st.st_ino, ts.st_ino);

    unsetenv("HL_TAR_NO_SYMLINK");
    free(buf);
}

/* A DANGLING archive link (target absent from the trimmed bundle) must FAIL the
 * non-admin fallback rather than silently producing an incomplete toolchain. */
UTEST_F(tar_fixture, extract_symlink_dangling_fails_in_fallback) {
    setenv("HL_TAR_NO_SYMLINK", "1", 1);
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_symlink(buf, &off, "bin/x86_64-cc", "cosmocc"); /* no bin/cosmocc */
    off += 512;

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/cc", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), -1);   /* dangling -> fail closed */

    unsetenv("HL_TAR_NO_SYMLINK");
    free(buf);
}

/* A symlink whose target is absolute or escapes via ".." is rejected (a
 * malformed archive), so it can never become a write-through primitive. */
UTEST_F(tar_fixture, extract_rejects_unsafe_symlink_target) {
    char dest[PATH_MAX];
    unsigned char *buf = calloc(1, 4096);
    ASSERT_NE(buf, NULL);

    size_t off = 0;
    tar_add_symlink(buf, &off, "bin/evil", "../../../../etc/passwd");
    off += 512;
    snprintf(dest, sizeof(dest), "%s/s1", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), -1);   /* ".." target refused */

    memset(buf, 0, 4096);
    off = 0;
    tar_add_symlink(buf, &off, "bin/evil", "/etc/passwd");
    off += 512;
    snprintf(dest, sizeof(dest), "%s/s2", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tar_extract(buf, off, dest), -1);   /* absolute target refused */

    /* hl_tar_parse still ignores symlinks entirely (contract preserved): the
     * same archive parses clean (0), it just yields no entries. */
    struct collect col = { .n = 0 };
    ASSERT_EQ(hl_tar_parse(buf, off, collect_cb, &col), 0);
    ASSERT_EQ(col.n, 0);
    free(buf);
}

/* ── hl_tar_create ─────────────────────────────────────────────────── */

UTEST(tar_create, round_trips_through_parse) {
    HlTarEntry entries[] = {
        { .name = "greet.txt", .data = (const unsigned char *)"hello", .size = 5,
          .mode = 0644, .is_dir = 0 },
        { .name = "sub", .data = NULL, .size = 0, .mode = 0755, .is_dir = 1 },
        { .name = "sub/deep.bin", .data = (const unsigned char *)"\x00\x01\x02", .size = 3,
          .mode = 0600, .is_dir = 0 },
    };
    unsigned char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(hl_tar_create(entries, 3, &out, &out_len), 0);
    ASSERT_NE(out, NULL);
    ASSERT_TRUE(out_len % 512 == 0);

    struct collect c = {0};
    ASSERT_EQ(hl_tar_parse(out, out_len, collect_cb, &c), 0);
    ASSERT_EQ(c.n, 3);
    ASSERT_STREQ(c.names[0], "greet.txt");
    ASSERT_EQ(c.dirs[0], 0);
    ASSERT_EQ(c.modes[0], 0644u);
    ASSERT_STREQ(c.names[1], "sub");
    ASSERT_EQ(c.dirs[1], 1);
    ASSERT_STREQ(c.names[2], "sub/deep.bin");
    ASSERT_EQ(c.modes[2], 0600u);
    free(out);
}

UTEST(tar_create, rejects_unsafe_name) {
    HlTarEntry bad[] = {
        { .name = "../escape", .data = (const unsigned char *)"x", .size = 1,
          .mode = 0644, .is_dir = 0 },
    };
    unsigned char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(hl_tar_create(bad, 1, &out, &out_len), -1);
    ASSERT_EQ(out, NULL);
}

UTEST(tar_create, empty_archive_is_two_zero_blocks) {
    unsigned char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(hl_tar_create(NULL, 0, &out, &out_len), 0);
    ASSERT_EQ(out_len, (size_t)1024);
    for (size_t i = 0; i < out_len; i++) ASSERT_EQ(out[i], 0);
    free(out);
}

UTEST_MAIN()
