/*
 * test_fs_statlist.c - hull.fs metadata (stat) + enumeration (list) THROUGH the
 * cap layer (checkpoint 3, Slice C). Proves the Slice-C acceptance boundary:
 * READ-only selection; exact-grant cannot list a parent or inspect siblings;
 * write-only grants leak no metadata; PATTERN enumeration exposes only matches;
 * SUBTREE symlinks stay confined; terminal symlinks are reported as links (lstat),
 * never followed; deterministic unsigned-byte ordering; most-specific shadowing
 * before listability; empty / missing / FIFO cases; and resource-bound rollback.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/utils/alloc.h"
#include <errno.h>
#include <ftw.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static char base[256];

static void setup(void) { snprintf(base, sizeof(base), "/tmp/hull_statlist_%d", (int)getpid()); mkdir(base, 0755); }
static int rm_e(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }
static void teardown(void) { if (nftw(base, rm_e, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT) {} }

static void mk_dir(const char *rel)  { char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); mkdir(p, 0755); }
static void mk_file(const char *rel, const char *data)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel);
  FILE *f = fopen(p, "wb"); if (f) { fputs(data, f); fclose(f); } }
static void mk_link(const char *target, const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); unlink(p); symlink(target, p); }
/* cosmo does not declare mkfifo under the feature level Hull's tests use (same as
 * test_fs_resolve.c); the FIFO-reporting checks are compiled out there. */
#if !defined(__COSMOPOLITAN__)
static void mk_fifo(const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); unlink(p); mkfifo(p, 0644); }
#endif

/* Compile a policy from raw read/write grant arrays (grants freed here). */
static int build_policy(HlAllocator *a, const char **rd, size_t rn,
                        const char **wr, size_t wn, HlFsPolicy *out, const char **err)
{
    HlFsGrant rg[8], wg[8]; size_t ri = 0, wi = 0; int rc = -1; const char *e = "?";
    for (; ri < rn; ri++) if (hl_fs_grant_parse(rd[ri], strlen(rd[ri]), a, &rg[ri], &e) != 0) goto done;
    for (; wi < wn; wi++) if (hl_fs_grant_parse(wr[wi], strlen(wr[wi]), a, &wg[wi], &e) != 0) goto done;
    rc = hl_fs_policy_compile(base, a, rg, rn, wg, wn, out, &e);
done:
    for (size_t i = 0; i < ri; i++) hl_fs_grant_free(&rg[i]);
    for (size_t i = 0; i < wi; i++) hl_fs_grant_free(&wg[i]);
    if (err) *err = e; return rc;
}

static HlFsConfig cfg_with(HlFsPolicy *p)
{ HlFsConfig c; memset(&c, 0, sizeof c); c.base_dir = base; c.base_len = strlen(base); c.policy = p; return c; }

/* Find an entry by name in a list; returns its index or -1. */
static long find_entry(const HlFsDirEntry *e, size_t n, const char *name)
{ for (size_t i = 0; i < n; i++) if (strcmp(e[i].name, name) == 0) return (long)i; return -1; }

/* ── stat/list select ONLY from the READ set; absent vs error; write-only leaks
 * no metadata (points 1, 3, 10) ──────────────────────────────────────────────── */
UTEST(fs_statlist, read_only_selection_absent_and_writeonly)
{
    setup();
    mk_dir("data"); mk_file("data/real.txt", "REAL");
    mk_file("secret.txt", "S");
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data" };          /* readable subtree */
    const char *wr[] = { "wonly.bin" };     /* write-only (absent -> CREATE) */
    ASSERT_EQ(0, build_policy(&a, rd, 1, wr, 1, &p, &err));
    HlFsConfig c = cfg_with(&p);

    HlFsStatInfo st;
    /* present read-set target */
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data/real.txt", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_FILE, (int)st.type);
    ASSERT_EQ((uint64_t)4, st.size);
    /* absent authorized path -> rc 1 (nil), NOT an error */
    err = NULL;
    ASSERT_EQ(1, hl_cap_fs_stat(&c, "data/none.txt", &st, &err));
    ASSERT_TRUE(err == NULL);
    /* write-only grant reveals no metadata through stat -> permission */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_stat(&c, "wonly.bin", &st, &err));
    ASSERT_STREQ("permission", err);
    /* ungranted path -> permission */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_stat(&c, "secret.txt", &st, &err));
    ASSERT_STREQ("permission", err);

    /* stat of the SUBTREE anchor itself resolves the "." residual (fstat the dir). */
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_DIR, (int)st.type);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── an EXACT-file grant cannot list its parent or inspect siblings (point 2) ─── */
UTEST(fs_statlist, exact_grant_no_parent_no_sibling)
{
    setup();
    mk_dir("data"); mk_file("data/real.txt", "REAL"); mk_file("data/other.txt", "O");
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data/real.txt" };   /* EXACT */
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    HlFsStatInfo st;
    /* the exact file itself stats fine */
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data/real.txt", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_FILE, (int)st.type);
    /* the parent directory is NOT authorized via an exact-file grant -> permission */
    HlFsDirEntry *e = NULL; size_t n = 0; err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_list(&c, "data", &e, &n, &a, &err));
    ASSERT_STREQ("permission", err);
    ASSERT_TRUE(e == NULL); ASSERT_EQ((size_t)0, n);
    /* the exact path IS authorized but is not a directory -> not_a_directory
     * (the ratified distinction: authorized-but-wrong-type, not "permission"). */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_list(&c, "data/real.txt", &e, &n, &a, &err));
    ASSERT_STREQ("not_a_directory", err);
    ASSERT_TRUE(e == NULL); ASSERT_EQ((size_t)0, n);
    /* a sibling cannot be stat'd */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_stat(&c, "data/other.txt", &st, &err));
    ASSERT_STREQ("permission", err);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── PATTERN enumeration exposes ONLY matching entries; stat gated too (point 4) ─ */
UTEST(fs_statlist, pattern_enumeration_only_matches)
{
    setup();
    mk_dir("data");
    mk_file("data/a.csv", "1"); mk_file("data/b.txt", "22"); mk_file("data/c.csv", "333");
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data/*.csv" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    HlFsDirEntry *e = NULL; size_t n = 0;
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "data", &e, &n, &a, &err));
    ASSERT_EQ((size_t)2, n);                       /* only the two .csv */
    ASSERT_STREQ("a.csv", e[0].name);              /* deterministic order */
    ASSERT_STREQ("c.csv", e[1].name);
    ASSERT_EQ(-1L, find_entry(e, n, "b.txt"));     /* .txt filtered out */
    hl_cap_fs_list_free(e, n, &a);

    HlFsStatInfo st;
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data/a.csv", &st, &err));   /* matches */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_stat(&c, "data/b.txt", &st, &err));  /* not matched */
    ASSERT_STREQ("permission", err);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── SUBTREE list stays confined; terminal symlinks reported as links, escaping
 * symlink target never leaked (points 5, 6, 10) ──────────────────────────────── */
UTEST(fs_statlist, subtree_symlinks_reported_not_followed)
{
    setup();
    mk_dir("data"); mk_file("data/real.txt", "REAL");
    mk_link("real.txt", "data/link");          /* in-subtree symlink */
    mk_link("/etc/hostname", "data/esc");      /* escaping absolute symlink */
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    HlFsDirEntry *e = NULL; size_t n = 0;
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "data", &e, &n, &a, &err));
    long il = find_entry(e, n, "link"), ie = find_entry(e, n, "esc"),
         ir = find_entry(e, n, "real.txt");
    ASSERT_TRUE(il >= 0 && ie >= 0 && ir >= 0);
    ASSERT_EQ((int)HL_FS_NODE_SYMLINK, (int)e[il].type);   /* link reported AS a link */
    ASSERT_EQ((int)HL_FS_NODE_SYMLINK, (int)e[ie].type);   /* escaping link too */
    ASSERT_EQ((int)HL_FS_NODE_FILE,    (int)e[ir].type);
    hl_cap_fs_list_free(e, n, &a);

    /* stat reports the link's OWN type, NEVER following to the target - so the
     * escaping symlink's target (/etc/hostname) leaks no metadata. */
    HlFsStatInfo st;
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data/esc", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_SYMLINK, (int)st.type);
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data/link", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_SYMLINK, (int)st.type);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── PATTERN terminal symlink: reported as a link by stat/list, REFUSED by read
 * (the ratified metadata contract, point 6) ──────────────────────────────────── */
UTEST(fs_statlist, pattern_terminal_symlink_reported_read_refused)
{
    setup();
    mk_dir("data"); mk_file("data/real.txt", "REAL");
    mk_link("real.txt", "data/link.csv");      /* a symlink whose name matches *.csv */
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data/*.csv" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    /* list exposes it (matches *.csv) and reports it as a link */
    HlFsDirEntry *e = NULL; size_t n = 0; err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "data", &e, &n, &a, &err));
    long i = find_entry(e, n, "link.csv");
    ASSERT_TRUE(i >= 0);
    ASSERT_EQ((int)HL_FS_NODE_SYMLINK, (int)e[i].type);
    hl_cap_fs_list_free(e, n, &a);

    /* stat reports the link (metadata), never following */
    HlFsStatInfo st; err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data/link.csv", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_SYMLINK, (int)st.type);
    /* but READ refuses the symlink (PATTERN refuses; no aliasing the target) */
    char buf[16]; err = NULL;
    ASSERT_EQ((int64_t)-1, hl_cap_fs_read(&c, "data/link.csv", buf, sizeof(buf), &err));
    ASSERT_STREQ("symlink_denied", err);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── deterministic unsigned-byte order, shorter-prefix-first (point 7) ────────── */
UTEST(fs_statlist, deterministic_byte_order)
{
    setup();
    mk_dir("d");
    /* creation order deliberately unsorted; includes an uppercase (0x42 < 0x61)
     * and a prefix pair to prove shorter-first. */
    mk_file("d/z", "1"); mk_file("d/a", "1"); mk_file("d/m", "1");
    mk_file("d/B", "1"); mk_file("d/abc", "1"); mk_file("d/ab", "1");
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "d" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    HlFsDirEntry *e = NULL; size_t n = 0; err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "d", &e, &n, &a, &err));
    ASSERT_EQ((size_t)6, n);
    /* 'B'(0x42) < 'a'(0x61) < 'ab' < 'abc' < 'm' < 'z' */
    ASSERT_STREQ("B",   e[0].name);
    ASSERT_STREQ("a",   e[1].name);
    ASSERT_STREQ("ab",  e[2].name);   /* shorter prefix precedes longer */
    ASSERT_STREQ("abc", e[3].name);
    ASSERT_STREQ("m",   e[4].name);
    ASSERT_STREQ("z",   e[5].name);
    hl_cap_fs_list_free(e, n, &a);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── empty dir -> empty list; missing dir -> not_found; FIFO reported not opened
 * (point 10) ─────────────────────────────────────────────────────────────────── */
UTEST(fs_statlist, empty_missing_and_fifo)
{
    setup();
    mk_dir("empty");
    mk_dir("data"); mk_file("data/f", "1");
#if !defined(__COSMOPOLITAN__)
    mk_fifo("data/pipe");
#endif
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "." };                 /* base-root: everything readable */
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    /* empty directory -> rc 0, count 0, NULL array */
    HlFsDirEntry *e = NULL; size_t n = 99; err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "empty", &e, &n, &a, &err));
    ASSERT_EQ((size_t)0, n); ASSERT_TRUE(e == NULL);

    /* missing directory -> error (not_found), no partial */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_list(&c, "nope", &e, &n, &a, &err));
    ASSERT_STREQ("not_found", err);
    ASSERT_TRUE(e == NULL); ASSERT_EQ((size_t)0, n);

#if !defined(__COSMOPOLITAN__)
    /* FIFO is REPORTED as "other" via lstat - never opened (would block) */
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "data", &e, &n, &a, &err));
    long ip = find_entry(e, n, "pipe");
    ASSERT_TRUE(ip >= 0);
    ASSERT_EQ((int)HL_FS_NODE_OTHER, (int)e[ip].type);
    hl_cap_fs_list_free(e, n, &a);

    HlFsStatInfo st; err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "data/pipe", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_OTHER, (int)st.type);   /* stat lstat's it; no block */
#endif

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── most-specific shadowing BEFORE listability: a governing multi-component
 * PATTERN denies list("logs") and does NOT fall through to the SUBTREE; deeper
 * lists that only the SUBTREE governs are allowed (reviewer requirement) ──────── */
UTEST(fs_statlist, multi_pattern_shadows_subtree)
{
    setup();
    mk_dir("logs"); mk_dir("logs/2026"); mk_file("logs/2026/x.txt", "1");
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "logs", "logs/*/*.txt" };
    ASSERT_EQ(0, build_policy(&a, rd, 2, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    /* the more-specific multi-component PATTERN governs list("logs") and denies it */
    HlFsDirEntry *e = NULL; size_t n = 0; err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_list(&c, "logs", &e, &n, &a, &err));
    ASSERT_STREQ("permission", err);       /* NOT listed via the broader SUBTREE */
    ASSERT_TRUE(e == NULL);

    /* deeper: only the SUBTREE governs -> authorized */
    err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "logs/2026", &e, &n, &a, &err));
    ASSERT_EQ((size_t)1, n);
    ASSERT_STREQ("x.txt", e[0].name);
    hl_cap_fs_list_free(e, n, &a);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── multiple same-prefix terminal patterns resolve to ONE most-specific /
 * lexicographic filter, NEVER a union (reviewer requirement) ──────────────────── */
UTEST(fs_statlist, same_prefix_patterns_no_union)
{
    setup();
    mk_dir("data");
    mk_file("data/a.csv", "1"); mk_file("data/b.txt", "1"); mk_file("data/c.csv", "1");
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data/*.csv", "data/*.txt" };
    ASSERT_EQ(0, build_policy(&a, rd, 2, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    /* grant-text ASC breaks the specificity tie: "*.csv" < "*.txt", so the csv
     * filter wins alone - b.txt is NOT exposed (no filter union). */
    HlFsDirEntry *e = NULL; size_t n = 0; err = NULL;
    ASSERT_EQ(0, hl_cap_fs_list(&c, "data", &e, &n, &a, &err));
    ASSERT_EQ((size_t)2, n);
    ASSERT_STREQ("a.csv", e[0].name);
    ASSERT_STREQ("c.csv", e[1].name);
    ASSERT_EQ(-1L, find_entry(e, n, "b.txt"));
    hl_cap_fs_list_free(e, n, &a);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── resource bounds: an allocation-failure mid-list rolls back COMPLETELY (no
 * partial result, no leak); one directory fd is used (no fd leak over many calls)
 * (point 11) ─────────────────────────────────────────────────────────────────── */
UTEST(fs_statlist, list_rollback_and_no_fd_leak)
{
    setup();
    mk_dir("data");
    for (int i = 0; i < 40; i++) { char nm[64]; snprintf(nm, sizeof(nm), "data/f%02d", i); mk_file(nm, "x"); }
    HlAllocator pa; hl_alloc_init(&pa, 0);        /* policy allocator (unlimited) */
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data" };
    ASSERT_EQ(0, build_policy(&pa, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    /* A tiny-limit allocator for the LIST buffers forces a mid-enumeration alloc
     * failure; the op must roll back completely and leak nothing. */
    HlAllocator la; hl_alloc_init(&la, 256);      /* far below 40 names + array */
    HlFsDirEntry *e = NULL; size_t n = 99; err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_list(&c, "data", &e, &n, &la, &err));
    ASSERT_STREQ("io_error", err);
    ASSERT_TRUE(e == NULL); ASSERT_EQ((size_t)0, n);
    ASSERT_EQ((size_t)0, hl_alloc_used(&la));     /* complete rollback, no leak */

    /* No directory-fd leak across many list calls: the fd number stays bounded. */
    int f0 = dup(1); close(f0);
    for (int i = 0; i < 200; i++) {
        HlFsDirEntry *ee = NULL; size_t nn = 0; const char *e2 = NULL;
        ASSERT_EQ(0, hl_cap_fs_list(&c, "data", &ee, &nn, &pa, &e2));
        ASSERT_EQ((size_t)40, nn);
        hl_cap_fs_list_free(ee, nn, &pa);
    }
    int f1 = dup(1);
    ASSERT_TRUE(f1 <= f0 + 4);                     /* no accumulating open fds */
    close(f1);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&pa));
    teardown();
}

/* ── a read-set CREATE whose target (or an intermediate) is absent -> stat returns
 * ABSENT (nil), never (nil, "not_found") (issue 1) ───────────────────────────── */
UTEST(fs_statlist, stat_read_create_absent_is_nil)
{
    setup();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "out/result.bin" };   /* out/ absent -> read-set CREATE */
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    HlFsStatInfo st;
    /* the intermediate out/ is missing -> ABSENT, and *err is cleared (issue 2). */
    err = "stale";
    ASSERT_EQ(1, hl_cap_fs_stat(&c, "out/result.bin", &st, &err));
    ASSERT_TRUE(err == NULL);
    /* out/ exists but the file is still absent -> still ABSENT */
    mk_dir("out"); err = "stale";
    ASSERT_EQ(1, hl_cap_fs_stat(&c, "out/result.bin", &st, &err));
    ASSERT_TRUE(err == NULL);
    /* create the file -> present */
    mk_file("out/result.bin", "R"); err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, "out/result.bin", &st, &err));
    ASSERT_EQ((int)HL_FS_NODE_FILE, (int)st.type);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── a prior error token does not leak into a later ABSENT via a reused pointer
 * (issue 2) ──────────────────────────────────────────────────────────────────── */
UTEST(fs_statlist, stat_error_then_absent_same_pointer)
{
    setup();
    mk_dir("data"); mk_file("data/real.txt", "R"); mk_file("secret.txt", "S");
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    HlFsStatInfo st;
    /* a DENIED path sets the token... */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_stat(&c, "secret.txt", &st, &err));
    ASSERT_STREQ("permission", err);
    /* ...then the SAME pointer for an ABSENT authorized path clears it. */
    ASSERT_EQ(1, hl_cap_fs_stat(&c, "data/none.txt", &st, &err));
    ASSERT_TRUE(err == NULL);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── stat(".") of the app root works under a base-root grant, and is denied
 * without one (issue 3) ──────────────────────────────────────────────────────── */
UTEST(fs_statlist, stat_root_via_base_grant)
{
    setup();
    mk_file("f.txt", "1"); mk_dir("data");
    HlAllocator a; hl_alloc_init(&a, 0);

    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "." };                 /* base-root */
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);
    HlFsStatInfo st; err = NULL;
    ASSERT_EQ(0, hl_cap_fs_stat(&c, ".", &st, &err));       /* the root, a directory */
    ASSERT_EQ((int)HL_FS_NODE_DIR, (int)st.type);
    hl_fs_policy_free(&p);

    /* a non-root grant does NOT authorize stat(".") */
    HlFsPolicy p2 = HL_FS_POLICY_INIT;
    const char *rd2[] = { "data" };
    ASSERT_EQ(0, build_policy(&a, rd2, 1, NULL, 0, &p2, &err));
    HlFsConfig c2 = cfg_with(&p2);
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_stat(&c2, ".", &st, &err));
    ASSERT_STREQ("permission", err);
    hl_fs_policy_free(&p2);

    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

UTEST_MAIN();
