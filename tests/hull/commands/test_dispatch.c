/*
 * test_dispatch.c - Tests for subcommand dispatcher
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/commands/dispatch.h"

/* ── Dispatch tests ───────────────────────────────────────────────── */

UTEST(dispatch, no_args_returns_neg1)
{
    char *argv[] = { "hull" };
    int rc = hl_command_dispatch(1, argv);
    ASSERT_EQ(rc, -1);
}

UTEST(dispatch, unknown_command_returns_neg1)
{
    char *argv[] = { "hull", "nonexistent" };
    int rc = hl_command_dispatch(2, argv);
    ASSERT_EQ(rc, -1);
}

UTEST(dispatch, server_args_return_neg1)
{
    /* Server flags like -p 3000 should not match any command */
    char *argv[] = { "hull", "-p", "3000" };
    int rc = hl_command_dispatch(3, argv);
    ASSERT_EQ(rc, -1);
}

UTEST(dispatch, entry_point_returns_neg1)
{
    /* A .lua or .js file should not match a command */
    char *argv[] = { "hull", "app.lua" };
    int rc = hl_command_dispatch(2, argv);
    ASSERT_EQ(rc, -1);
}

/*
 * We can't easily test that known commands dispatch correctly without
 * side effects, but we can verify the negative cases above which confirm
 * the dispatcher correctly falls through for non-command args.
 */

/* The private installer checksum helper (src/hull/commands/asset_checksum.h is not
 * on the public -Iinclude path, so forward-declare it; the symbol is in CMD_OBJS,
 * which this test links). H1 S2a de-duplicated feature.c/flavor.c onto it. */
int hl_asset_checksum_eq(const char a[64], const char b[64]);

UTEST(asset_checksum, fixed64_equal_and_diffs)
{
    char a[64], b[64];
    memset(a, 'a', 64); memset(b, 'a', 64);
    ASSERT_EQ(hl_asset_checksum_eq(a, b), 1);   /* equal over the 64 */
    b[0] = 'b';  ASSERT_EQ(hl_asset_checksum_eq(a, b), 0);   /* differ at index 0 */
    b[0] = 'a';  b[63] = 'b';
    ASSERT_EQ(hl_asset_checksum_eq(a, b), 0);   /* differ at index 63 */
}

/* ── Tool sandbox: an output directory that grants too much (audit 5 M3) ──
 *
 * `hull build -o /app` made the tool sandbox's read-write-create grant "/"
 * (the generated Dockerfile did exactly that), and `-o ~/x` the whole home.
 * hl_tool_sandbox_init refuses such an output directory BEFORE it applies
 * anything, so these calls leave this process unsandboxed. */
#include "hull/sandbox.h"
#include "hull/cap/tool.h"
#include "../test_tmpdir.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

UTEST(tool_sandbox, refuses_root_output_dir)
{
    HlToolUnveilCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ASSERT_EQ(hl_tool_sandbox_init(&ctx, ".", "/", NULL, NULL), -1);
}

UTEST(tool_sandbox, refuses_home_output_dir)
{
    char home[512];
    ASSERT_TRUE(hl_test_mkdtemp(home, sizeof home, "hull_fakehome") != NULL);
    const char *old = getenv("HOME");
    char saved[1024] = "";
    if (old) snprintf(saved, sizeof saved, "%s", old);
    setenv("HOME", home, 1);

    HlToolUnveilCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    EXPECT_EQ(hl_tool_sandbox_init(&ctx, ".", home, NULL, NULL), -1);

    if (old) setenv("HOME", saved, 1); else unsetenv("HOME");
    rmdir(home);
}

/* ── Tool sandbox plan (audit 8) ──────────────────────────────────────
 *
 * The plan is what both lists - the userspace allowlist and the kernel
 * unveil - are applied from, so testing it needs no sandbox applied to this
 * process. Each test runs against a fake $HOME. */
#include "hull/tool.h"
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

/* The plan mkdirs the runtime cache under the fake $HOME: remove the tree. */
static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                char sub[PATH_MAX];
                snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                rm_rf(sub);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

typedef struct {
    char home[HL_TEST_PATH_MAX];
    char saved_home[1024];
    int  had_home;
    char saved_cwd[PATH_MAX];
} FakeHome;

static int fake_home_enter(FakeHome *f)
{
    if (!hl_test_mkdtemp(f->home, sizeof f->home, "hull_plan_home")) return -1;
    const char *old = getenv("HOME");
    f->had_home = old != NULL;
    snprintf(f->saved_home, sizeof f->saved_home, "%s", old ? old : "");
    if (!getcwd(f->saved_cwd, sizeof f->saved_cwd)) return -1;
    setenv("HOME", f->home, 1);
    return 0;
}

static void fake_home_leave(FakeHome *f)
{
    if (chdir(f->saved_cwd) != 0) { /* nothing better to do */ }
    if (f->had_home) setenv("HOME", f->saved_home, 1); else unsetenv("HOME");
    rm_rf(f->home);
}

static void touch(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (fp) { fputs("x\n", fp); fclose(fp); }
}

/* Does the plan grant `sub` (relative to $HOME, or absolute when sub[0] is
 * '/') with exactly `perms`? Compared resolved, as the plan stores them. */
static int plan_has(const HlToolSandboxPlan *p, const char *home,
                    const char *sub, const char *perms)
{
    char want[PATH_MAX], rwant[PATH_MAX];
    if (sub[0] == '/') snprintf(want, sizeof want, "%s", sub);
    else snprintf(want, sizeof want, "%s/%s", home, sub);
    const char *w = realpath(want, rwant) ? rwant : want;
    for (int i = 0; i < p->n; i++) {
        char rg[PATH_MAX];
        const char *g = realpath(p->g[i].path, rg) ? rg : p->g[i].path;
        if (strcmp(g, w) == 0) return strcmp(p->g[i].perms, perms) == 0;
    }
    return 0;
}

/* Load the plan as hl_tool_sandbox_init does, minus every grant that is the
 * fake $HOME or above it: that home lives under a temp dir the plan grants
 * wholesale (/tmp, or the cosmo build temp ~/.hull/tmp), which would make
 * every "refused" assertion vacuous. A real home is never under them. */
static void load_plan_sans_tmp(HlToolUnveilCtx *ctx, const HlToolSandboxPlan *p,
                               const char *home)
{
    char rh[PATH_MAX];
    if (!realpath(home, rh)) snprintf(rh, sizeof rh, "%s", home);
    hl_tool_unveil_init(ctx);
    for (int i = 0; i < p->n; i++) {
        char rg[PATH_MAX];
        if (realpath(p->g[i].path, rg)) {
            size_t l = strlen(rg);
            if (strncmp(rh, rg, l) == 0 && (rh[l] == '\0' || rh[l] == '/'))
                continue;
        }
        hl_tool_unveil_add(ctx, p->g[i].path, p->g[i].perms);
    }
    hl_tool_unveil_seal(ctx);
}

/* M2: the trees `hull feature install` / `hull flavor install` / the tools
 * store write are readable (and nothing more), in the one plan both lists
 * come from. */
UTEST(tool_sandbox, plan_grants_hull_caches_read_only)
{
    FakeHome f;
    ASSERT_EQ(fake_home_enter(&f), 0);
    char d[PATH_MAX];
    const char *subs[] = { ".hull", ".hull/feature", ".hull/platform",
                           ".hull/blobs", ".hull/blobs/tools", ".hull/tools", NULL };
    for (int i = 0; subs[i]; i++) {
        snprintf(d, sizeof d, "%s/%s", f.home, subs[i]);
        mkdir(d, 0700);
    }

    HlToolSandboxPlan p;
    ASSERT_EQ(hl_tool_sandbox_plan(&p, NULL, NULL, NULL, NULL), 0);
    EXPECT_TRUE(plan_has(&p, f.home, ".hull/feature", "r"));
    EXPECT_TRUE(plan_has(&p, f.home, ".hull/platform", "r"));
    EXPECT_TRUE(plan_has(&p, f.home, ".hull/blobs/tools", "r"));
    EXPECT_TRUE(plan_has(&p, f.home, ".hull/tools", "rx"));
    EXPECT_FALSE(plan_has(&p, f.home, ".hull", "r"));
    EXPECT_STREQ(p.g[0].path, "/tmp");        /* the kernel probe stays first */

    /* Loaded into a userspace list: a feature archive is readable, not
     * writable; ~/.hull itself is not reachable. */
    HlToolUnveilCtx ctx;
    load_plan_sans_tmp(&ctx, &p, f.home);
    snprintf(d, sizeof d, "%s/.hull/feature/libhull_feature-tui.a", f.home);
    touch(d);
    EXPECT_EQ(hl_tool_unveil_check(&ctx, d, 'r'), 0);
    EXPECT_NE(hl_tool_unveil_check(&ctx, d, 'w'), 0);
    snprintf(d, sizeof d, "%s/.hull/cache.key", f.home);
    EXPECT_NE(hl_tool_unveil_check(&ctx, d, 'r'), 0);
    hl_tool_unveil_free(&ctx);
    hl_tool_sandbox_plan_free(&p);

    fake_home_leave(&f);
}

/* M2: a key named on the command line is granted as that one FILE, read-
 * only - never its directory; a directory named there is not granted. */
UTEST(tool_sandbox, plan_grants_a_named_key_file_alone)
{
    FakeHome f;
    ASSERT_EQ(fake_home_enter(&f), 0);
    char keys[PATH_MAX], key[PATH_MAX], other[PATH_MAX];
    snprintf(keys, sizeof keys, "%s/.hull", f.home);
    mkdir(keys, 0700);
    snprintf(keys, sizeof keys, "%s/.hull/keys", f.home);
    mkdir(keys, 0700);
    snprintf(key, sizeof key, "%s/dev.key", keys);
    snprintf(other, sizeof other, "%s/release.key", keys);
    touch(key);
    touch(other);

    const char *files[] = { key, keys, NULL };
    HlToolSandboxPlan p;
    ASSERT_EQ(hl_tool_sandbox_plan(&p, NULL, NULL, NULL, files), 0);
    EXPECT_TRUE(plan_has(&p, f.home, ".hull/keys/dev.key", "r"));
    EXPECT_FALSE(plan_has(&p, f.home, ".hull/keys", "r"));

    HlToolUnveilCtx ctx;
    load_plan_sans_tmp(&ctx, &p, f.home);
    EXPECT_EQ(hl_tool_unveil_check(&ctx, key, 'r'), 0);
    EXPECT_NE(hl_tool_unveil_check(&ctx, key, 'w'), 0);
    EXPECT_NE(hl_tool_unveil_check(&ctx, other, 'r'), 0);   /* the sibling key */
    hl_tool_unveil_free(&ctx);
    hl_tool_sandbox_plan_free(&p);

    fake_home_leave(&f);
}

/* M1: run from ~ with nothing named (`hull doctor --tui`, and `hull new`
 * once hull_tool has made its target), the plan neither fails nor grants ~;
 * a subdirectory named as the output is granted. */
UTEST(tool_sandbox, plan_from_home_grants_neither_home_nor_fails)
{
    FakeHome f;
    ASSERT_EQ(fake_home_enter(&f), 0);
    ASSERT_EQ(chdir(f.home), 0);
    char sub[PATH_MAX];
    snprintf(sub, sizeof sub, "%s/myapp", f.home);
    mkdir(sub, 0755);

    HlToolSandboxPlan p;
    ASSERT_EQ(hl_tool_sandbox_plan(&p, NULL, NULL, NULL, NULL), 0);
    EXPECT_FALSE(plan_has(&p, f.home, f.home, "rwc"));
    for (int i = 0; i < p.n; i++) EXPECT_STRNE(p.g[i].path, ".");
    hl_tool_sandbox_plan_free(&p);

    ASSERT_EQ(hl_tool_sandbox_plan(&p, NULL, "myapp", NULL, NULL), 0);
    EXPECT_TRUE(plan_has(&p, f.home, "myapp", "rwc"));
    hl_tool_sandbox_plan_free(&p);

    /* ~ itself named as the output (`hull init` run in ~) is still refused. */
    EXPECT_EQ(hl_tool_sandbox_plan(&p, NULL, ".", NULL, NULL), -1);

    fake_home_leave(&f);
}

UTEST(tool_sandbox, path_too_broad)
{
    FakeHome f;
    ASSERT_EQ(fake_home_enter(&f), 0);
    char sub[PATH_MAX];
    snprintf(sub, sizeof sub, "%s/proj", f.home);
    mkdir(sub, 0755);
    EXPECT_EQ(hl_tool_path_too_broad("/"), 1);
    EXPECT_EQ(hl_tool_path_too_broad(f.home), 1);
    EXPECT_EQ(hl_tool_path_too_broad(sub), 0);
    EXPECT_EQ(hl_tool_path_too_broad(NULL), 1);
    fake_home_leave(&f);
}

/* ── What a tool command's argv names ─────────────────────────────── */

UTEST(tool_argv, app_dir_skips_option_values_and_switches)
{
    char dir[HL_TEST_PATH_MAX], app[PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(dir, sizeof dir, "hull_argv") != NULL);
    snprintf(app, sizeof app, "%s/app", dir);
    mkdir(app, 0755);

    /* --install-dir's value is not the app (it became the writable output) */
    char *a1[] = { "systemd", "--install-dir", dir, app, NULL };
    EXPECT_STREQ(hl_tool_argv_app_dir("hull.deploy", 4, a1), app);
    /* `hull deploy --sign` is a switch: the next word is the app */
    char *a2[] = { "dockerfile", "--sign", app, NULL };
    EXPECT_STREQ(hl_tool_argv_app_dir("hull.deploy", 3, a2), app);
    /* ...while `hull build --sign` takes a key file */
    char *a3[] = { "build", "--sign", dir, NULL };
    EXPECT_TRUE(hl_tool_argv_app_dir("hull.build", 3, a3) == NULL);
    /* nothing named: NULL, not "." */
    char *a4[] = { "build", "--no-aot", NULL };
    EXPECT_TRUE(hl_tool_argv_app_dir("hull.build", 2, a4) == NULL);

    rmdir(app);
    rmdir(dir);
}

/* c_core L1: a link named like a subcommand word in a cloned repo
 * (`build -> ~/.ssh`, then `hull compute build`) is not the app dir. */
UTEST(tool_argv, app_dir_is_never_a_symlink)
{
    char dir[HL_TEST_PATH_MAX], target[PATH_MAX], link[PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(dir, sizeof dir, "hull_argv_ln") != NULL);
    snprintf(target, sizeof target, "%s/secret", dir);
    snprintf(link, sizeof link, "%s/build", dir);
    mkdir(target, 0700);
    if (symlink(target, link) != 0) {
        rmdir(target); rmdir(dir);
        UTEST_SKIP("cannot create a symlink here");
    }
    char slash[PATH_MAX];
    snprintf(slash, sizeof slash, "%s/", link);
    char *a1[] = { "compute", link, NULL };
    EXPECT_TRUE(hl_tool_argv_app_dir("hull.compute", 2, a1) == NULL);
    char *a2[] = { "compute", slash, NULL };      /* lstat("x/") follows */
    EXPECT_TRUE(hl_tool_argv_app_dir("hull.compute", 2, a2) == NULL);
    char *a3[] = { "compute", link, target, NULL };
    EXPECT_STREQ(hl_tool_argv_app_dir("hull.compute", 3, a3), target);
    unlink(link);
    rmdir(target);
    rmdir(dir);
}

UTEST(tool_argv, scaffold_target)
{
    char *n1[] = { "new", "--type", "rest", "myapp", "--runtime", "js", NULL };
    EXPECT_STREQ(hl_tool_argv_scaffold_target("hull.new", 6, n1), "myapp");
    char *n2[] = { "new", "--cli", NULL };
    EXPECT_TRUE(hl_tool_argv_scaffold_target("hull.new", 2, n2) == NULL);
    char *i1[] = { "init", "--profile", "htmx", NULL };
    EXPECT_STREQ(hl_tool_argv_scaffold_target("hull.init", 3, i1), ".");
    char *i2[] = { "init", "proj", NULL };
    EXPECT_STREQ(hl_tool_argv_scaffold_target("hull.init", 2, i2), "proj");
    EXPECT_TRUE(hl_tool_argv_scaffold_target("hull.build", 2, i2) == NULL);
}

UTEST(tool_argv, read_files)
{
    char *out[HL_TOOL_SANDBOX_MAX_FILES];
    char *b[] = { "build", "--sign", "k.key", "--platform-sig=p.sig", "app", NULL };
    int n = hl_tool_argv_read_files("hull.build", 5, b, out, HL_TOOL_SANDBOX_MAX_FILES);
    ASSERT_EQ(n, 2);
    EXPECT_STREQ(out[0], "k.key");
    EXPECT_STREQ(out[1], "p.sig");
    for (int i = 0; i < n; i++) free(out[i]);

    char *d[] = { "dockerfile", "--sign", "app", NULL };   /* a switch there */
    EXPECT_EQ(hl_tool_argv_read_files("hull.deploy", 3, d, out, HL_TOOL_SANDBOX_MAX_FILES), 0);

    char *v[] = { "verify", "--developer-key", "d.pub", "--binary", "bin/app", "app", NULL };
    n = hl_tool_argv_read_files("hull.verify", 6, v, out, HL_TOOL_SANDBOX_MAX_FILES);
    ASSERT_EQ(n, 2);
    EXPECT_STREQ(out[0], "d.pub");
    EXPECT_STREQ(out[1], "bin/app");
    for (int i = 0; i < n; i++) free(out[i]);

    char *s[] = { "sign-platform", "--dir", "build", "keys/platform", NULL };
    n = hl_tool_argv_read_files("hull.sign_platform", 4, s, out, HL_TOOL_SANDBOX_MAX_FILES);
    ASSERT_EQ(n, 2);
    EXPECT_STREQ(out[0], "keys/platform.key");
    EXPECT_STREQ(out[1], "keys/platform.pub");
    for (int i = 0; i < n; i++) free(out[i]);
}

UTEST_MAIN();
