/*
 * test_fs_authz.c - hull.fs path authorization THROUGH the cap layer. Unlike
 * test_fs_policy.c (pure compile/select), these drive
 * hl_cap_fs_read / _write / _mmap with a compiled policy and verify the runtime
 * per-kind symlink enforcement, CREATE/PATTERN creation, O_TRUNC, independent
 * read/write sets, and no-grant denial - the Slice-B acceptance boundary.
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

static void setup(void) { snprintf(base, sizeof(base), "/tmp/hull_authz_%d", (int)getpid()); mkdir(base, 0755); }
static int rm_e(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }
static void teardown(void) { if (nftw(base, rm_e, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT) {} }

static void mk_dir(const char *rel)  { char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); mkdir(p, 0755); }
static void mk_file(const char *rel, const char *data)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel);
  FILE *f = fopen(p, "wb"); if (f) { fputs(data, f); fclose(f); } }
static void mk_link(const char *target, const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); unlink(p); symlink(target, p); }

static void build_tree(void)
{
    mk_file("r.txt", "R");
    mk_file("secret.txt", "SECRET");
    mk_dir("data"); mk_file("data/real.txt", "REAL");
    mk_link("real.txt", "data/link");        /* in-subtree symlink -> data/real.txt */
    mk_link("/etc/hostname", "data/esc");     /* escaping symlink (absolute) */
    mk_link("r.txt", "exlink.txt");           /* a symlink used as an EXACT grant */
}

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

/* ── independent read/write sets + no-grant denial ───────────────────────────── */
UTEST(fs_authz, independent_sets_and_deny)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "r.txt" };            /* readable only */
    const char *wr[] = { "w.txt" };            /* writable only (absent -> CREATE) */
    ASSERT_EQ(0, build_policy(&a, rd, 1, wr, 1, &p, &err));
    HlFsConfig c = cfg_with(&p);

    char buf[64];
    ASSERT_EQ((int64_t)1, hl_cap_fs_read(&c, "r.txt", buf, sizeof(buf), &err)); /* read OK */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_write(&c, "r.txt", "x", 1, &err));                  /* not writable */
    ASSERT_STREQ("permission", err);
    ASSERT_EQ(0, hl_cap_fs_write(&c, "w.txt", "hi", 2, &err));                  /* write OK (create) */
    err = NULL;
    ASSERT_EQ((int64_t)-1, hl_cap_fs_read(&c, "w.txt", buf, sizeof(buf), &err));/* not readable */
    ASSERT_STREQ("permission", err);
    err = NULL;
    ASSERT_EQ((int64_t)-1, hl_cap_fs_read(&c, "nope.txt", buf, sizeof(buf), &err)); /* ungranted */
    ASSERT_STREQ("permission", err);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── SUBTREE follows in-subtree symlinks but stays confined to its anchor ─────── */
UTEST(fs_authz, subtree_follows_but_confined)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    HlFsConfig c = cfg_with(&p);

    char buf[64];
    int64_t n = hl_cap_fs_read(&c, "data/real.txt", buf, sizeof(buf), &err);
    ASSERT_EQ((int64_t)4, n); ASSERT_EQ(0, memcmp(buf, "REAL", 4));
    /* in-subtree symlink is FOLLOWED */
    n = hl_cap_fs_read(&c, "data/link", buf, sizeof(buf), &err);
    ASSERT_EQ((int64_t)4, n); ASSERT_EQ(0, memcmp(buf, "REAL", 4));
    /* escaping absolute symlink is CLAMPED to base -> base/etc/hostname (absent),
     * NEVER the host /etc/hostname */
    err = NULL;
    ASSERT_EQ((int64_t)-1, hl_cap_fs_read(&c, "data/esc", buf, sizeof(buf), &err));
    ASSERT_STREQ("not_found", err);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── EXACT refuses a symlink (no aliasing a non-authorized target) ────────────── */
UTEST(fs_authz, exact_refuses_symlink)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    /* exlink.txt is a symlink -> compile refuses it at the anchor walk (its parent
     * is base, the leaf itself is the symlink; EXACT refuses). */
    const char *rd[] = { "exlink.txt" };
    /* the grant compiles (leaf recorded); the OPEN refuses the symlink */
    int rc = build_policy(&a, rd, 1, NULL, 0, &p, &err);
    char buf[64];
    if (rc == 0) {
        HlFsConfig c = cfg_with(&p);
        err = NULL;
        ASSERT_EQ((int64_t)-1, hl_cap_fs_read(&c, "exlink.txt", buf, sizeof(buf), &err));
        ASSERT_STREQ("symlink_denied", err);
        hl_fs_policy_free(&p);
    } else {
        /* or it is refused at compile - either way the symlink is never followed */
        ASSERT_STREQ("symlink_denied", err);
    }
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── CREATE: only constrained components; O_TRUNC preserved ───────────────────── */
UTEST(fs_authz, create_constrained_and_trunc)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *g[] = { "out/result.bin" };    /* out/ absent -> CREATE, in both sets */
    ASSERT_EQ(0, build_policy(&a, g, 1, g, 1, &p, &err));
    HlFsConfig c = cfg_with(&p);

    ASSERT_EQ(0, hl_cap_fs_write(&c, "out/result.bin", "AAAAAA", 6, &err));  /* creates out/ + file */
    struct stat st; char dp[512]; snprintf(dp, sizeof(dp), "%s/out", base);
    ASSERT_EQ(0, stat(dp, &st)); ASSERT_TRUE(S_ISDIR(st.st_mode));           /* out/ created */
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_write(&c, "out/other.bin", "x", 1, &err));       /* sibling denied */
    ASSERT_STREQ("permission", err);
    ASSERT_EQ(0, hl_cap_fs_write(&c, "out/result.bin", "BB", 2, &err));      /* O_TRUNC */
    char buf[64];
    ASSERT_EQ((int64_t)2, hl_cap_fs_read(&c, "out/result.bin", buf, sizeof(buf), &err));
    ASSERT_EQ(0, memcmp(buf, "BB", 2));                                       /* NOT "BBAAAA" */

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── PATTERN write: creates only matched nonterminal dirs + the terminal file ─── */
UTEST(fs_authz, pattern_write_creates_matched)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *g[] = { "logs/*/*.txt" };      /* logs/ absent; in both sets */
    ASSERT_EQ(0, build_policy(&a, g, 1, g, 1, &p, &err));
    HlFsConfig c = cfg_with(&p);

    ASSERT_EQ(0, hl_cap_fs_write(&c, "logs/2026/a.txt", "L", 1, &err));      /* creates logs/2026 + a.txt */
    struct stat st; char dp[512]; snprintf(dp, sizeof(dp), "%s/logs/2026", base);
    ASSERT_EQ(0, stat(dp, &st)); ASSERT_TRUE(S_ISDIR(st.st_mode));
    char buf[8];
    ASSERT_EQ((int64_t)1, hl_cap_fs_read(&c, "logs/2026/a.txt", buf, sizeof(buf), &err));
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_write(&c, "logs/2026/a.csv", "x", 1, &err));     /* .csv not matched */
    ASSERT_STREQ("permission", err);
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_write(&c, "logs/2026/sub/a.txt", "x", 1, &err)); /* '*' never crosses '/' */
    ASSERT_STREQ("permission", err);

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── no policy on the cfg -> denied (the no-grant migration) ──────────────────── */
UTEST(fs_authz, no_policy_denies)
{
    setup(); build_tree();
    HlFsConfig c; memset(&c, 0, sizeof c);
    c.base_dir = base; c.base_len = strlen(base); c.policy = NULL;
    char buf[64]; const char *err = NULL;
    ASSERT_EQ((int64_t)-1, hl_cap_fs_read(&c, "r.txt", buf, sizeof(buf), &err));
    ASSERT_STREQ("permission", err);
    err = NULL;
    ASSERT_EQ(-1, hl_cap_fs_write(&c, "r.txt", "x", 1, &err));
    ASSERT_STREQ("permission", err);
    teardown();
}

/* ── manifest compile REJECTS a bad grant with its precise token (no silent skip,
 * no leak) - a declared capability must not silently become unusable. ─────────── */
UTEST(fs_authz, manifest_rejects_bad_grants)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;

    const char *abs_g[] = { "/tmp/hull-tail.log", "r.txt" };   /* absolute -> reject */
    ASSERT_EQ(-1, hl_fs_policy_compile_manifest(base, &a, abs_g, 2, NULL, 0, &p, &err));
    ASSERT_STREQ("invalid_path", err);

    const char *pat_g[] = { "data/**", "r.txt" };              /* unsupported pattern -> reject */
    err = NULL;
    ASSERT_EQ(-1, hl_fs_policy_compile_manifest(base, &a, pat_g, 2, NULL, 0, &p, &err));
    ASSERT_STREQ("unsupported_pattern", err);

    ASSERT_EQ((size_t)0, hl_alloc_used(&a));                   /* no leak on the failure path */
    teardown();
}

/* ── base-root "." grant authorizes the whole app dir (least-specific SUBTREE) ─── */
UTEST(fs_authz, base_root_grant)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *g[] = { "." };                 /* the whole app dir + descendants */
    ASSERT_EQ(0, build_policy(&a, g, 1, g, 1, &p, &err));
    ASSERT_EQ((size_t)1, p.read_n);
    ASSERT_EQ((int)HL_FS_ENTRY_SUBTREE, (int)p.read[0].kind);
    ASSERT_EQ((size_t)0, p.read[0].grant_n);   /* 0 components == base root */

    HlFsConfig c = cfg_with(&p);
    char buf[64];
    ASSERT_EQ((int64_t)1, hl_cap_fs_read(&c, "r.txt", buf, sizeof(buf), &err));          /* any path */
    ASSERT_EQ((int64_t)4, hl_cap_fs_read(&c, "data/real.txt", buf, sizeof(buf), &err));  /* nested */
    ASSERT_EQ(0, hl_cap_fs_write(&c, "created.txt", "N", 1, &err));                       /* create */

    hl_fs_policy_free(&p); ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

UTEST_MAIN();
