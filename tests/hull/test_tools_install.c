/* FTW_DEPTH / FTW_PHYS are XSI extensions to nftw; on glibc they're
 * only declared when _XOPEN_SOURCE >= 500. macOS exposes them
 * unconditionally AND uses _XOPEN_SOURCE to gate Darwin extensions
 * the other way (defining it hides clock_gettime_nsec_np /
 * CLOCK_UPTIME_RAW that utest.h needs), so this define has to stay
 * Linux-only. */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

/*
 * test_tools_install.c — Unit tests for the tool registry + path helpers.
 *
 * Covers the API in include/hull/tools_install.h:
 *   - hl_tools_registry() returns a NUL-terminated table with at least
 *     one entry (wamrc).
 *   - hl_tools_find() — present, absent, NULL inputs.
 *   - hl_tools_name_valid() — accepted chars, rejected escape attempts.
 *   - hl_tools_published_for() — known + unknown platforms.
 *   - hl_tools_asset_name() — composition + buffer overflow rejection.
 *   - hl_tools_install_path() — refuses traversal-bearing names.
 *   - hl_tools_dir() — creates ~/.hull/tools, idempotent.
 *   - hl_tools_lookup_path() — find an installed binary, miss when
 *     absent, prefer ~/.hull/tools/ over $PATH.
 *
 * The tests redirect HOME and PATH to a tempdir per fixture so a real
 * user environment is never touched.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/tools_install.h"

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Fixture: per-test sandbox under /tmp ───────────────────────────── */

struct tools_fixture {
    char tmpdir[PATH_MAX];          /* sandbox root */
    char saved_home[PATH_MAX];      /* original HOME, restored on teardown */
    char saved_path[2048];          /* original PATH */
    int  had_home;
    int  had_path;
};

static int rm_recursive_entry(const char *path, const struct stat *sb,
                              int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

static int rm_recursive(const char *path)
{
    /* In-process recursive delete via nftw(FTW_DEPTH). Replaces
     * `system("rm -rf ...")` because Cosmopolitan's toybox rm
     * rejects `-r` ("rm: illegal option -- r"), and a missing
     * cleanup leaves the next mkdir hitting EEXIST and the rest
     * of the suite cascading-failing. POSIX nftw works on Linux,
     * macOS, and cosmo uniformly. Ignore ENOENT — missing-path
     * is the steady state we're trying to reach anyway. */
    if (nftw(path, rm_recursive_entry, 16, FTW_DEPTH | FTW_PHYS) != 0
        && errno != ENOENT) {
        return -1;
    }
    return 0;
}

UTEST_F_SETUP(tools_fixture) {
    snprintf(utest_fixture->tmpdir, sizeof(utest_fixture->tmpdir),
             "/tmp/hull-tools-test-%d", getpid());
    rm_recursive(utest_fixture->tmpdir);
    ASSERT_EQ(mkdir(utest_fixture->tmpdir, 0700), 0);

    const char *h = getenv("HOME");
    utest_fixture->had_home = h != NULL;
    if (h) snprintf(utest_fixture->saved_home, sizeof(utest_fixture->saved_home), "%s", h);
    setenv("HOME", utest_fixture->tmpdir, 1);

    const char *p = getenv("PATH");
    utest_fixture->had_path = p != NULL;
    if (p) snprintf(utest_fixture->saved_path, sizeof(utest_fixture->saved_path), "%s", p);
    /* Empty PATH so step (3) of lookup is a no-op unless the test sets
     * its own — keeps tests hermetic. */
    setenv("PATH", "", 1);
}

UTEST_F_TEARDOWN(tools_fixture) {
    (void)utest_result;
    if (utest_fixture->had_home) setenv("HOME", utest_fixture->saved_home, 1);
    else                          unsetenv("HOME");
    if (utest_fixture->had_path) setenv("PATH", utest_fixture->saved_path, 1);
    else                          unsetenv("PATH");
    rm_recursive(utest_fixture->tmpdir);
}

/* ── Registry ──────────────────────────────────────────────────────── */

UTEST(registry, sentinel_terminated) {
    const HlToolSpec *r = hl_tools_registry();
    ASSERT_TRUE(r != NULL);
    /* At least one real entry. */
    ASSERT_TRUE(r[0].name != NULL);
    /* Walk to sentinel without running forever. */
    int n = 0;
    for (const HlToolSpec *t = r; t->name; t++) {
        n++;
        ASSERT_TRUE(t->description != NULL);
        if (n > 64) break;  /* registry is small; fail loud if it isn't */
    }
    ASSERT_LT(n, 64);
}

UTEST(registry, wamrc_present_with_platform_publish_flags) {
    const HlToolSpec *spec = hl_tools_find("wamrc");
    ASSERT_TRUE(spec != NULL);
    /* Native binaries are published for the three native targets; cosmo
     * is intentionally skipped (LLVM doesn't fit). */
    ASSERT_EQ(spec->has_linux_x86_64, 1);
    ASSERT_EQ(spec->has_linux_aarch64, 1);
    ASSERT_EQ(spec->has_darwin_arm64, 1);
    ASSERT_EQ(spec->has_cosmo, 0);
}

UTEST(registry, find_unknown_returns_null) {
    ASSERT_TRUE(hl_tools_find("not-a-real-tool") == NULL);
    ASSERT_TRUE(hl_tools_find("") == NULL);
    ASSERT_TRUE(hl_tools_find(NULL) == NULL);
}

/* ── Name validation ──────────────────────────────────────────────── */

UTEST(name_valid, accepts_simple_identifiers) {
    ASSERT_EQ(hl_tools_name_valid("wamrc"), 1);
    ASSERT_EQ(hl_tools_name_valid("wgpu-native"), 1);
    ASSERT_EQ(hl_tools_name_valid("Some_Tool9"), 1);
    ASSERT_EQ(hl_tools_name_valid("a"), 1);  /* single char OK */
}

UTEST(name_valid, rejects_traversal_and_separators) {
    ASSERT_EQ(hl_tools_name_valid(""), 0);
    ASSERT_EQ(hl_tools_name_valid(NULL), 0);
    ASSERT_EQ(hl_tools_name_valid(".."), 0);
    ASSERT_EQ(hl_tools_name_valid("../etc/passwd"), 0);
    ASSERT_EQ(hl_tools_name_valid("/etc/passwd"), 0);
    ASSERT_EQ(hl_tools_name_valid("foo/bar"), 0);
    ASSERT_EQ(hl_tools_name_valid("foo bar"), 0);     /* space */
    ASSERT_EQ(hl_tools_name_valid("foo\nbar"), 0);    /* newline */
    ASSERT_EQ(hl_tools_name_valid("foo:bar"), 0);     /* PATH separator */
    ASSERT_EQ(hl_tools_name_valid("foo.bar"), 0);     /* dot — would shadow ext */
}

UTEST(name_valid, rejects_overlong) {
    char overlong[HL_TOOL_NAME_MAX + 8];
    memset(overlong, 'a', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = '\0';
    ASSERT_EQ(hl_tools_name_valid(overlong), 0);
}

/* ── Platform / asset name ───────────────────────────────────────── */

UTEST(asset_name, composes_correctly) {
    const HlToolSpec *spec = hl_tools_find("wamrc");
    ASSERT_TRUE(spec != NULL);
    char out[128];
    ASSERT_EQ(hl_tools_asset_name(spec, "linux-x86_64", out, sizeof(out)), 0);
    ASSERT_STREQ(out, "hull-wamrc-linux-x86_64");
    ASSERT_EQ(hl_tools_asset_name(spec, "darwin-arm64", out, sizeof(out)), 0);
    ASSERT_STREQ(out, "hull-wamrc-darwin-arm64");
}

UTEST(asset_name, refuses_unpublished_platform) {
    const HlToolSpec *spec = hl_tools_find("wamrc");
    char out[128];
    /* wamrc is not published for cosmo. */
    ASSERT_EQ(hl_tools_asset_name(spec, "cosmo", out, sizeof(out)), -1);
}

UTEST(asset_name, refuses_unknown_platform) {
    const HlToolSpec *spec = hl_tools_find("wamrc");
    char out[128];
    ASSERT_EQ(hl_tools_asset_name(spec, "freebsd-riscv32", out, sizeof(out)), -1);
}

UTEST(asset_name, refuses_short_buffer) {
    const HlToolSpec *spec = hl_tools_find("wamrc");
    char tiny[4];
    ASSERT_EQ(hl_tools_asset_name(spec, "darwin-arm64", tiny, sizeof(tiny)), -1);
}

UTEST(published_for, null_safe) {
    ASSERT_EQ(hl_tools_published_for(NULL, "linux-x86_64"), 0);
    const HlToolSpec *spec = hl_tools_find("wamrc");
    ASSERT_EQ(hl_tools_published_for(spec, NULL), 0);
}

/* ── Directory creation ──────────────────────────────────────────── */

UTEST_F(tools_fixture, dir_creates_and_returns_trailing_slash) {
    (void)utest_fixture;
    char dir[PATH_MAX];
    ASSERT_EQ(hl_tools_dir(dir, sizeof(dir)), 0);
    /* ends with slash */
    size_t L = strlen(dir);
    ASSERT_GT(L, 0u);
    ASSERT_EQ(dir[L - 1], '/');

    /* Created on disk. */
    struct stat st;
    ASSERT_EQ(stat(dir, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    /* Mode masked by umask, but at least owner-readable/writable. */
    ASSERT_NE((st.st_mode & 0700), (mode_t)0);
}

UTEST_F(tools_fixture, dir_is_idempotent) {
    (void)utest_fixture;
    char a[PATH_MAX], b[PATH_MAX];
    ASSERT_EQ(hl_tools_dir(a, sizeof(a)), 0);
    ASSERT_EQ(hl_tools_dir(b, sizeof(b)), 0);
    ASSERT_STREQ(a, b);
}

UTEST_F(tools_fixture, dir_fails_without_home) {
    (void)utest_fixture;
    char out[PATH_MAX];
    unsetenv("HOME");
    ASSERT_EQ(hl_tools_dir(out, sizeof(out)), -1);
}

/* ── Install path ────────────────────────────────────────────────── */

UTEST_F(tools_fixture, install_path_composes) {
    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_install_path("wamrc", out, sizeof(out)), 0);
    char expect[PATH_MAX];
    snprintf(expect, sizeof(expect), "%s/.hull/tools/wamrc", utest_fixture->tmpdir);
    ASSERT_STREQ(out, expect);
}

UTEST_F(tools_fixture, install_path_rejects_traversal) {
    (void)utest_fixture;
    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_install_path("../etc/passwd", out, sizeof(out)), -1);
    ASSERT_EQ(hl_tools_install_path("foo/bar", out, sizeof(out)), -1);
    ASSERT_EQ(hl_tools_install_path("", out, sizeof(out)), -1);
}

/* ── Lookup ──────────────────────────────────────────────────────── */

/* Write an executable stub at `path`. Returns 0 on success. */
static int touch_exec(const char *path)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        /* Walk down and mkdir each component (cheap mkdir -p). */
        char acc[PATH_MAX] = "";
        if (dir[0] == '/') { acc[0] = '/'; acc[1] = '\0'; }
        char *t = dir;
        while (*t == '/') t++;
        char buf[PATH_MAX]; snprintf(buf, sizeof(buf), "%s", t);
        char *save = NULL;
        for (char *seg = strtok_r(buf, "/", &save); seg;
             seg = strtok_r(NULL, "/", &save)) {
            size_t L = strlen(acc);
            if (L > 0 && acc[L-1] != '/')
                snprintf(acc + L, sizeof(acc) - L, "/%s", seg);
            else
                snprintf(acc + L, sizeof(acc) - L, "%s", seg);
            mkdir(acc, 0755);
        }
    }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    return chmod(path, 0755);
}

UTEST_F(tools_fixture, lookup_misses_when_nothing_exists) {
    (void)utest_fixture;
    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_lookup_path("wamrc", NULL, out, sizeof(out)), -1);
}

UTEST_F(tools_fixture, lookup_finds_canonical_install) {
    (void)utest_fixture;
    /* Place a stub at ~/.hull/tools/wamrc. */
    char dir[PATH_MAX];
    ASSERT_EQ(hl_tools_dir(dir, sizeof(dir)), 0);
    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%swamrc", dir);
    ASSERT_EQ(touch_exec(target), 0);

    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_lookup_path("wamrc", NULL, out, sizeof(out)), 0);
    ASSERT_STREQ(out, target);
}

UTEST_F(tools_fixture, lookup_follows_symlink_to_blob) {
    /* The post-§1.5.b-X-3 install layout: the canonical
     * ~/.hull/tools/<name> path is a symlink into the
     * ~/.hull/blobs/tools/blobs/<XX>/<sha>/ pool. Lookup must
     * accept the symlink transparently because access(X_OK)
     * follows symlinks by default. This test mocks that layout
     * without going through a real network install. */

    /* 1. Create the blob target — pretend `hull tools install`
     *    put it there via hl_blob_store_put_durable + chmod 0755. */
    char blob_dir[PATH_MAX];
    snprintf(blob_dir, sizeof(blob_dir),
             "%s/.hull/blobs/tools/blobs/ab", utest_fixture->tmpdir);
    /* mkdir -p: .hull/blobs/tools/blobs/ab */
    char step[PATH_MAX];
    snprintf(step, sizeof(step), "%s/.hull", utest_fixture->tmpdir);
    mkdir(step, 0755);
    snprintf(step, sizeof(step), "%s/.hull/blobs", utest_fixture->tmpdir);
    mkdir(step, 0755);
    snprintf(step, sizeof(step), "%s/.hull/blobs/tools", utest_fixture->tmpdir);
    mkdir(step, 0755);
    snprintf(step, sizeof(step), "%s/.hull/blobs/tools/blobs",
             utest_fixture->tmpdir);
    mkdir(step, 0755);
    ASSERT_EQ(mkdir(blob_dir, 0755), 0);

    char blob_path[PATH_MAX];
    snprintf(blob_path, sizeof(blob_path),
             "%s/ab1234567890abcdef1234567890abcdef1234567890abcdef"
             "1234567890ab12", blob_dir);
    ASSERT_EQ(touch_exec(blob_path), 0);

    /* 2. Plant the symlink at the canonical lookup path. */
    char dir[PATH_MAX];
    ASSERT_EQ(hl_tools_dir(dir, sizeof(dir)), 0);
    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%swamrc", dir);
    ASSERT_EQ(symlink(blob_path, link_path), 0);

    /* 3. lookup_path finds it. access(X_OK) follows the symlink, so
     *    a 0755 blob target satisfies the check. The returned path
     *    is the symlink path (not the resolved blob path) — exec(2)
     *    callers transparently follow the link. */
    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_lookup_path("wamrc", NULL, out, sizeof(out)), 0);
    ASSERT_STREQ(out, link_path);
}

UTEST_F(tools_fixture, lookup_finds_sibling_of_hull_exe) {
    /* No canonical install; place stub next to "hull". */
    char hull_dir[PATH_MAX];
    snprintf(hull_dir, sizeof(hull_dir), "%s/bin", utest_fixture->tmpdir);
    ASSERT_EQ(mkdir(hull_dir, 0755), 0);

    char hull_path[PATH_MAX];
    snprintf(hull_path, sizeof(hull_path), "%s/hull", hull_dir);
    ASSERT_EQ(touch_exec(hull_path), 0);

    char tool_path[PATH_MAX];
    snprintf(tool_path, sizeof(tool_path), "%s/wamrc", hull_dir);
    ASSERT_EQ(touch_exec(tool_path), 0);

    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_lookup_path("wamrc", hull_path, out, sizeof(out)), 0);
    ASSERT_STREQ(out, tool_path);
}

UTEST_F(tools_fixture, lookup_finds_on_PATH) {
    char path_dir[PATH_MAX];
    snprintf(path_dir, sizeof(path_dir), "%s/path-stub", utest_fixture->tmpdir);
    ASSERT_EQ(mkdir(path_dir, 0755), 0);

    char tool_path[PATH_MAX];
    snprintf(tool_path, sizeof(tool_path), "%s/wamrc", path_dir);
    ASSERT_EQ(touch_exec(tool_path), 0);

    setenv("PATH", path_dir, 1);

    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_lookup_path("wamrc", NULL, out, sizeof(out)), 0);
    ASSERT_STREQ(out, tool_path);
}

UTEST_F(tools_fixture, lookup_prefers_canonical_over_PATH) {
    /* Both ~/.hull/tools and PATH have a wamrc. Canonical must win. */
    char dir[PATH_MAX];
    ASSERT_EQ(hl_tools_dir(dir, sizeof(dir)), 0);
    char canonical[PATH_MAX];
    snprintf(canonical, sizeof(canonical), "%swamrc", dir);
    ASSERT_EQ(touch_exec(canonical), 0);

    char path_dir[PATH_MAX];
    snprintf(path_dir, sizeof(path_dir), "%s/path-stub", utest_fixture->tmpdir);
    ASSERT_EQ(mkdir(path_dir, 0755), 0);
    char path_tool[PATH_MAX];
    snprintf(path_tool, sizeof(path_tool), "%s/wamrc", path_dir);
    ASSERT_EQ(touch_exec(path_tool), 0);
    setenv("PATH", path_dir, 1);

    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_lookup_path("wamrc", NULL, out, sizeof(out)), 0);
    ASSERT_STREQ(out, canonical);
}

UTEST_F(tools_fixture, lookup_rejects_invalid_name) {
    (void)utest_fixture;
    char out[PATH_MAX];
    ASSERT_EQ(hl_tools_lookup_path("../etc/passwd", NULL, out, sizeof(out)), -1);
    ASSERT_EQ(hl_tools_lookup_path("foo/bar", NULL, out, sizeof(out)), -1);
}

/* ── libc-musl bundle (.tar) tests ─────────────────────────────────── */

/* Minimal ustar header (name, size, regular-file typeflag). Our reader ignores
 * the checksum field, so we leave it blank. */
static void tar_put_header(unsigned char *b, const char *name, size_t size) {
    memset(b, 0, 512);
    strncpy((char *)b, name, 99);
    snprintf((char *)(b + 124), 12, "%011o", (unsigned)size);  /* size, octal */
    memset(b + 148, ' ', 8);                                   /* chksum: blank */
    b[156] = '0';                                              /* regular file */
}

static void tar_add_file(unsigned char *buf, size_t *off,
                         const char *name, const char *data, size_t len) {
    tar_put_header(buf + *off, name, len);
    *off += 512;
    memcpy(buf + *off, data, len);
    *off += (len + 511) & ~(size_t)511;
}

UTEST(tools_bundle, registry_has_musl_floors) {
    const HlToolSpec *x = hl_tools_find("libc-musl-x86_64");
    const HlToolSpec *a = hl_tools_find("libc-musl-aarch64");
    ASSERT_NE(x, NULL);
    ASSERT_NE(a, NULL);
    ASSERT_TRUE(x->is_bundle);
    ASSERT_TRUE(a->is_bundle);
    ASSERT_TRUE(hl_tools_published_for(x, "linux-x86_64"));
    ASSERT_EQ(hl_tools_published_for(x, "linux-aarch64"), 0);  /* wrong arch */
    ASSERT_TRUE(hl_tools_published_for(a, "linux-aarch64"));
    ASSERT_EQ(hl_tools_published_for(a, "darwin-arm64"), 0);
}

UTEST(tools_bundle, asset_name_is_dot_tar) {
    char asset[128];
    ASSERT_EQ(hl_tools_asset_name(hl_tools_find("libc-musl-x86_64"),
                                  "linux-x86_64", asset, sizeof(asset)), 0);
    ASSERT_STREQ(asset, "hull-libc-musl-x86_64.tar");
    /* a binary tool keeps the per-platform form */
    ASSERT_EQ(hl_tools_asset_name(hl_tools_find("wamrc"),
                                  "linux-x86_64", asset, sizeof(asset)), 0);
    ASSERT_STREQ(asset, "hull-wamrc-linux-x86_64");
}

UTEST_F(tools_fixture, extract_tar_bundle) {
    unsigned char *buf = calloc(1, 8192);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "./crt1.o", "OBJ1", 4);        /* leading ./ stripped */
    tar_add_file(buf, &off, "libgcc.a", "ARCHIVE-BYTES", 13);
    off += 512;                                            /* zero terminator block */

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/floor", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tools_extract_tar(buf, off, dest), 0);

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
UTEST_F(tools_fixture, extract_real_tar) {
    char srcdir[PATH_MAX], f1[PATH_MAX], tarpath[PATH_MAX], cmd[PATH_MAX * 3];
    snprintf(srcdir, sizeof(srcdir), "%s/src", utest_fixture->tmpdir);
    ASSERT_EQ(mkdir(srcdir, 0755), 0);
    snprintf(f1, sizeof(f1), "%s/crt1.o", srcdir);
    FILE *w = fopen(f1, "wb");
    ASSERT_NE(w, NULL);
    fwrite("REALTARBYTES", 1, 12, w);
    fclose(w);

    snprintf(tarpath, sizeof(tarpath), "%s/floor.tar", utest_fixture->tmpdir);
    /* The fixture blanks PATH for hermetic lookup tests; give system() a PATH. */
    setenv("PATH", "/usr/bin:/bin", 1);
    snprintf(cmd, sizeof(cmd), "tar cf '%s' -C '%s' . 2>/dev/null", tarpath, srcdir);
    if (system(cmd) != 0) { setenv("PATH", "", 1); return; }  /* no tar → skip */
    setenv("PATH", "", 1);

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
    ASSERT_EQ(hl_tools_extract_tar(tbuf, (size_t)tlen, dest), 0);
    snprintf(out, sizeof(out), "%s/crt1.o", dest);
    FILE *rf = fopen(out, "rb");
    ASSERT_NE(rf, NULL);
    size_t n = fread(rd, 1, sizeof(rd), rf);
    fclose(rf);
    ASSERT_EQ(n, (size_t)12);
    ASSERT_EQ(memcmp(rd, "REALTARBYTES", 12), 0);
    free(tbuf);
}

UTEST_F(tools_fixture, extract_tar_rejects_traversal) {
    unsigned char *buf = calloc(1, 4096);
    ASSERT_NE(buf, NULL);
    size_t off = 0;
    tar_add_file(buf, &off, "../escape.o", "X", 1);
    off += 512;
    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/floor2", utest_fixture->tmpdir);
    ASSERT_EQ(hl_tools_extract_tar(buf, off, dest), -1);   /* traversal refused */
    free(buf);
}

UTEST_MAIN()
