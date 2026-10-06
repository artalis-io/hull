/*
 * sandbox_tool.c - build-tool ("hull build") kernel sandbox.
 *
 * Split out of sandbox.c so the app-runtime sandbox (hl_sandbox_apply /
 * _apply_pledge / _policy_from_manifest) can ship without the build-tool
 * unveil machinery. `hl_tool_sandbox_init` is the only sandbox entry
 * point that references cap/tool.c's hl_tool_unveil_* helpers, which in
 * turn live next to the compiler-spawn surface - none of which belongs
 * in the no-runtime libhull core. Keeping it in its own translation unit
 * means libhull links the app sandbox alone, while the full hull binary
 * links both.
 *
 * The pledge/unveil provider block below deliberately mirrors the one in
 * sandbox.c rather than sharing a header: it keeps sandbox.c's security
 * paths untouched by this extraction (a pure deletion there), and the
 * "does this platform have pledge/unveil" decision is OS-determined and
 * ABI-stable, so the two copies cannot drift in any meaningful way.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/sandbox.h"
#include "hull/cap/tool.h"   /* hl_tool_cosmo_prepare_tmpdir / _tmpdir */
#include "hull/shared/cache_dir.h"
#include "hull/release_io.h"  /* hl_release_io_self_path */
#include "hull/shared/host.h" /* hl_host_is_windows */
#include "log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ── Platform pledge/unveil providers (mirror of sandbox.c) ────────── */

#if defined(__COSMOPOLITAN__)

#include <cosmo.h>   /* IsLinux() / IsOpenbsd() */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);
/* As sandbox.c: an APE's pledge/unveil enforce on Linux and OpenBSD only;
 * elsewhere (Windows, macOS, the other BSDs) they return 0 and do nothing. */
static int sb_supported(void) { return IsLinux() || IsOpenbsd(); }
static int sb_pledge_supported(void) { return IsLinux() || IsOpenbsd(); }

#elif defined(__OpenBSD__)

/* pledge()/unveil() are native - declared in <unistd.h> (already included). */
static int sb_supported(void) { return 1; }
static int sb_pledge_supported(void) { return 1; }

#elif defined(__linux__)

#include <sys/syscall.h>
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);
/* The polyfill's unveil is Landlock: without it nothing restricts the
 * filesystem, whatever unveil returns. */
static int sb_supported(void)
{
#ifdef SYS_landlock_create_ruleset
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, 1UL /* VERSION */);
    return abi >= 1;
#else
    return 0;
#endif
}
/* The polyfill's pledge (seccomp) works without Landlock - but tool mode does
 * not use it on Linux (see tool_pledge below): Linux without Landlock has NO
 * kernel tool sandbox, only the userspace allowlist. */
static int sb_pledge_supported(void) { return 1; }

#elif defined(__APPLE__)

/* macOS has no pledge/unveil - no-ops. The tool-mode allowlist is still
 * enforced in userspace via hl_tool_unveil_check in cap/tool.c. */
static int pledge(const char *p, const char *ep)
{
    (void)p; (void)ep;
    return 0;
}

static int unveil(const char *p, const char *perm)
{
    (void)p; (void)perm;
    return 0;
}

/* No kernel route for tool mode here: only the userspace allowlist. */
static int sb_supported(void) { return 0; }
static int sb_pledge_supported(void) { return 0; }

#else /* unsupported platforms - full no-op */

static int pledge(const char *p, const char *ep)
{
    (void)p; (void)ep;
    return 0;
}

static int unveil(const char *p, const char *perm)
{
    (void)p; (void)perm;
    return 0;
}

static int sb_supported(void) { return 0; }
static int sb_pledge_supported(void) { return 0; }

#endif /* platform dispatch */

/* ── Tool-mode sandbox ─────────────────────────────────────────────── */

/* Does this path name a directory that exists? Unveiling one that does not
 * grants nothing and, on the kernel path, is a failed call. */
static int dir_exists_p(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The user's home directory: $HOME, else %USERPROFILE% (Windows). */
static const char *tool_home(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    return (home && *home) ? home : NULL;
}

/* A grant there is the whole filesystem, or every file the user owns
 * (~/.ssh, ~/.bashrc - and ~/.hull/tools, which Landlock unions into rwc
 * although it is granted rx below). On Windows cosmo's realpath keeps the
 * caller's spelling ("c:/users/mark" -> "/c/users/mark") while $HOME
 * resolves to "/C/Users/Mark", so a case-sensitive compare called the home
 * directory "not broad" when spelled in another case. */
int hl_tool_path_too_broad(const char *path)
{
    char rp[PATH_MAX];
    if (!path || !realpath(path, rp)) return 1;
    if (strcmp(rp, "/") == 0) return 1;
    const char *home = tool_home();
    char hr[PATH_MAX];
    if (home && realpath(home, hr)) {
        size_t l = strlen(rp);
        int same = hl_host_is_windows() ? strncasecmp(hr, rp, l) == 0
                                        : strncmp(hr, rp, l) == 0;
        if (same && (hr[l] == '\0' || hr[l] == '/'))
            return 1;
    }
    return 0;
}

/* The invocation directory is granted read-write-create - except when that
 * grant would be the whole filesystem (cwd "/"), or the user's home or above
 * (a build run from ~ executes the app's top-level code and the toolchain
 * with every file in ~ writable). `hull new` / `hull init` need no
 * exception: hull_tool creates their target directory before the sandbox
 * applies and passes it as the output directory. */
static int cwd_grantable(void)
{
    return !hl_tool_path_too_broad(".");
}

static void plan_add(HlToolSandboxPlan *p, const char *path, const char *perms,
                     int optional)
{
    if (!path || !*path) return;
    if (p->n >= HL_TOOL_SANDBOX_MAX_GRANTS) {
        log_warn("[sandbox] tool mode: grant table full (%d) - '%s' DROPPED; "
                 "raise HL_TOOL_SANDBOX_MAX_GRANTS", p->n, path);
        return;
    }
    char *dup = strdup(path);
    if (!dup) return;
    HlToolGrant *g = &p->g[p->n++];
    g->path = dup;
    snprintf(g->perms, sizeof g->perms, "%s", perms);
    g->optional = optional;
}

/* <home>/<sub>, optional (a directory this user may not have yet). */
static void plan_add_home(HlToolSandboxPlan *p, const char *home,
                          const char *sub, const char *perms)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", home, sub);
    if (n > 0 && (size_t)n < sizeof(path))
        plan_add(p, path, perms, 1);
}

void hl_tool_sandbox_plan_free(HlToolSandboxPlan *plan)
{
    if (!plan) return;
    for (int i = 0; i < plan->n; i++) free(plan->g[i].path);
    memset(plan, 0, sizeof(*plan));
}

int hl_tool_sandbox_plan(HlToolSandboxPlan *plan,
                         const char *app_dir,
                         const char *output_dir,
                         const char *platform_dir,
                         const char *const *read_files)
{
    if (!plan) return -1;
    memset(plan, 0, sizeof(*plan));

    /* The output directory is granted read-write-create. `-o /app` made that
     * "/" - the whole filesystem writable to the app code a build runs (the
     * generated Dockerfile did exactly this) - and `-o ~/x` the whole home.
     * Refuse rather than grant it. */
    if (output_dir && dir_exists_p(output_dir) && hl_tool_path_too_broad(output_dir)) {
        log_error("[sandbox] tool mode: refusing to make '%s' writable (it is "
                  "the filesystem root, or your home directory or above); "
                  "write the output into a subdirectory", output_dir);
        return -1;
    }

    /* hull's own directory (the re-exec in manifest_extract_file.c, and the
     * platform archives beside it), read + execute; never a directory that
     * grants too much ("/hull", ~/hull). */
    char self_dir[PATH_MAX];
    int have_self_dir = 0;
    if (hl_release_io_self_path(self_dir, sizeof(self_dir)) == 0) {
        char *slash = strrchr(self_dir, '/');
        if (slash && slash != self_dir) {
            *slash = '\0';
            have_self_dir = !hl_tool_path_too_broad(self_dir);
        }
    }
    if (platform_dir && hl_tool_path_too_broad(platform_dir))
        platform_dir = NULL;   /* argv[0] "/hull" made this "/" */

    /* App sources: read-only, and only when the path is a real directory
     * that is not "/" or $HOME or above - the app code a build loads would
     * otherwise read every file the user owns. */
    if (app_dir && dir_exists_p(app_dir) && hl_tool_path_too_broad(app_dir)) {
        log_warn("[sandbox] tool mode: not granting '%s' (the filesystem "
                 "root, or your home directory or above) as the app "
                 "directory", app_dir);
        app_dir = NULL;
    }

    /* Temp directory: read/write/create, and execute - the build stages
     * tooling and intermediate artifacts there. First: it doubles as the
     * kernel probe (see hl_tool_sandbox_init). */
    plan_add(plan, "/tmp", "rwcx", 0);

    if (app_dir && dir_exists_p(app_dir))
        plan_add(plan, app_dir, "r", 0);

    /* System compilers and headers */
    plan_add(plan, "/usr", "rx", 1);
#if defined(__COSMOPOLITAN__) || defined(__linux__)
    plan_add(plan, "/bin", "rx", 1);     /* APE shebang invokes /bin/sh */
    plan_add(plan, "/lib", "rx", 1);     /* shared libs need execute for mmap */
    plan_add(plan, "/lib64", "rx", 1);
    /* /opt: `make fetch-cosmocc COSMOCC_DIR=/opt/cosmo` (CI) and other
     * package managers commonly install toolchains here. */
    plan_add(plan, "/opt", "rx", 1);
#endif
#ifdef __APPLE__
    /* Homebrew compilers and Xcode CLT */
    plan_add(plan, "/opt", "rx", 1);
    plan_add(plan, "/Library", "r", 1);
#endif

    /* The invocation directory (`hull build` from inside the app writes
     * ./app here). Otherwise say why nothing there is reachable. */
    if (cwd_grantable())
        plan_add(plan, ".", "rwc", 0);
    else
        log_info("[sandbox] tool mode: not granting the current directory "
                 "(the filesystem root, or your home directory or above)");

    /* Output directory: write/create. app_dir above is READ-ONLY, so a
     * build writing into the app tree (app.com, and the .hull/build the
     * post-link tidy-up moves debug sidecars into) depends on this. Only
     * when it EXISTS: unveiling a missing path grants nothing and, on Linux,
     * is a failed unveil. */
    if (output_dir && dir_exists_p(output_dir))
        plan_add(plan, output_dir, "rwc", 0);

    /* Hull runtime cache root - `hull build` writes AOT artifacts there for
     * cross-app reuse. hl_hull_cache_dir() mkdirs the path. */
    {
        char cache_path[PATH_MAX];
        if (hl_hull_cache_dir(cache_path, sizeof(cache_path)) == 0)
            plan_add(plan, cache_path, "rwc", 1);
    }

    /* Platform library + hull binary: read + execute */
    if (platform_dir)
        plan_add(plan, platform_dir, "rx", 1);

    /* hull re-execs ITSELF to isolate JS manifest extraction (issue #427), so
     * the running binary has to stay executable: platform_dir covers it only
     * when argv[0] carried a slash, and an install prefix like ~/.local/bin
     * is under none of the roots above. Its DIRECTORY (not $HOME, not "/").
     * Best-effort: cosmo has no self-path route, and the extraction falls
     * back to in-process when the spawn is refused. */
    if (have_self_dir)
        plan_add(plan, self_dir, "rx", 1);

    /* Narrow subtrees of ~/.hull (never ~ itself):
     *   tools        - side-loaded tool binaries (wamrc, zig) and the
     *                  libc-musl floor bundles Tier B's ld.lld reads: rx
     *   feature      - `hull feature install`'s signed archives + their
     *                  cached manifests, composed by `hull build --with`
     *                  and for every hull/tui app: r
     *   platform     - `hull flavor install`'s per-flavor libs: r
     *   blobs/tools  - the content-addressed tools store that
     *                  tool.bundle_verify re-hashes a bundle from: r
     * Install writes these from C outside tool mode, so no write grant. */
    const char *home = tool_home();
    if (home) {
#if defined(__COSMOPOLITAN__) || defined(__linux__)
        /* cosmocc.zip's default user-install paths. */
        plan_add_home(plan, home, ".cosmocc", "rx");
        plan_add_home(plan, home, "cosmocc", "rx");
#endif
        plan_add_home(plan, home, ".hull/tools", "rx");
        plan_add_home(plan, home, ".hull/feature", "r");
        plan_add_home(plan, home, ".hull/platform", "r");
        plan_add_home(plan, home, ".hull/blobs/tools", "r");
    }

    /* cosmo/Windows: the shared build temp dir (~/.hull/tmp) cosmocc's
     * driver writes its mktemper scratch to, recorded with the SAME
     * forward-slash spelling the reroute later exports as TMPDIR.
     * hl_tool_sandbox_init creates it before planning; unset elsewhere. */
    {
        char td[PATH_MAX];
        if (hl_tool_cosmo_tmpdir(td, sizeof(td)) == 0)
            plan_add(plan, td, "rwcx", 1);
    }

    /* Files named on the command line (signing keys, --platform-sig,
     * verify --binary): each FILE read-only, never its directory - a key
     * under ~/.hull/keys must not open the rest of ~/.hull/keys. */
    for (int i = 0; read_files && i < HL_TOOL_SANDBOX_MAX_FILES && read_files[i]; i++) {
        char rp[PATH_MAX];
        struct stat st;
        if (!realpath(read_files[i], rp) || stat(rp, &st) != 0)
            continue;   /* missing: the command reports it */
        if (!S_ISREG(st.st_mode)) {
            log_warn("[sandbox] tool mode: not granting '%s' (not a regular "
                     "file)", read_files[i]);
            continue;
        }
        plan_add(plan, rp, "r", 0);
    }
    return 0;
}

/* Kernel unveil of one path. An OPTIONAL path that does not exist (a
 * toolchain dir this host lacks) is skipped; any other failure is counted -
 * it used to be ignored, and "applied" logged regardless. */
static void kunveil(const char *path, const char *perm, int optional, int *failed)
{
    if (optional && access(path, F_OK) != 0) return;
    if (unveil(path, perm) != 0) {
        log_error("[sandbox] tool mode: unveil(%s, %s) failed", path, perm);
        (*failed)++;
    }
}

int hl_tool_sandbox_init(HlToolUnveilCtx *ctx,
                         const char *app_dir,
                         const char *output_dir,
                         const char *platform_dir,
                         const char *const *read_files)
{
    if (!ctx) return -1;

    hl_tool_unveil_init(ctx);

    /* cosmo/Windows: unveil needs the build temp dir to EXIST when added,
     * and the sandbox seals before the build could create it. No-op off a
     * cosmo hull on Windows. */
    hl_tool_cosmo_prepare_tmpdir();

    HlToolSandboxPlan plan;
    if (hl_tool_sandbox_plan(&plan, app_dir, output_dir, platform_dir,
                             read_files) != 0)
        return -1;

    /* The userspace list: what hl_tool_* check. */
    for (int i = 0; i < plan.n; i++)
        hl_tool_unveil_add(ctx, plan.g[i].path, plan.g[i].perms);
    hl_tool_unveil_seal(ctx);

    /* The kernel list: the same plan. Fails closed: a failed unveil or
     * pledge on a host whose kernel enforces them is an error, not a log
     * line. */
    int kfail = 0;
    int kernel = sb_supported();
    if (kernel) {
        /* grant[0] ("/tmp") doubles as the probe a cosmo APE needs: it gates
         * on IsLinux() alone, and on a Linux kernel without Landlock its
         * unveil() fails ENOSYS - which made every tool command refuse to
         * run. ENOSYS means nothing was applied: that host simply has no
         * kernel tool sandbox, as a native build on it reports. */
        if (unveil(plan.g[0].path, plan.g[0].perms) != 0) {
            if (errno == ENOSYS) {
                kernel = 0;
            } else {
                log_error("[sandbox] tool mode: unveil(%s, %s) failed",
                          plan.g[0].path, plan.g[0].perms);
                kfail++;
            }
        }
    }
    if (kernel) {
        for (int i = 1; i < plan.n; i++)
            kunveil(plan.g[i].path, plan.g[i].perms, plan.g[i].optional, &kfail);
        if (unveil(NULL, NULL) != 0) {   /* seal */
            log_error("[sandbox] tool mode: sealing unveil failed");
            kfail++;
        }
    }
    hl_tool_sandbox_plan_free(&plan);

    /* Pledge for tool mode (proc + exec, for the spawned toolchain) applies
     * on OpenBSD only. The Linux polyfill refuses "exec" without
     * execpromises (EINVAL), and those become a seccomp filter on every
     * compiler and linker the tool runs - a subset of these promises that
     * must still let gcc / lld / zig map executable code and start threads.
     * Broad enough for that, it would block next to nothing; so on Linux
     * tool mode rests on the Landlock unveil above plus the spawn
     * allowlist. (Before, the call was made there and its EINVAL ignored.) */
#if defined(__COSMOPOLITAN__)
    int tool_pledge = IsOpenbsd();
#elif defined(__OpenBSD__)
    int tool_pledge = 1;
#else
    int tool_pledge = 0;
#endif
    if (tool_pledge && sb_pledge_supported() &&
        pledge("stdio rpath wpath cpath proc exec fattr", NULL) != 0) {
        log_error("[sandbox] tool mode: pledge failed: %s", strerror(errno));
        kfail++;
    }
    if (kfail) {
        log_error("[sandbox] tool mode: kernel sandbox NOT applied "
                  "(%d failure(s)) - refusing to continue", kfail);
        return -1;
    }

    /* A full table means later adds were dropped, and the drop is silent at
     * the cap. Say so. */
    if (ctx->count >= HL_TOOL_MAX_UNVEILED)
        log_warn("[sandbox] unveil table full (%d) - later paths were "
                 "DROPPED; raise HL_TOOL_MAX_UNVEILED", ctx->count);

    if (kernel)
        log_info("[sandbox] tool mode applied (%d unveiled paths, kernel-enforced)",
                 ctx->count);
    else
        log_info("[sandbox] tool mode: %d unveiled paths, checked in userspace "
                 "only (no kernel sandbox on this host)", ctx->count);

    return 0;
}
