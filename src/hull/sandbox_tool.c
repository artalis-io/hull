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
#include "log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
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

/* Is `path` (resolved) the filesystem root, or the user's home directory or
 * one of its ancestors? A grant there is the whole filesystem, or every
 * file the user owns (~/.ssh, ~/.bashrc - and ~/.hull/tools, which Landlock
 * unions into rwc although it is granted rx below). An unresolvable path
 * counts as broad: there is nothing safe to grant. */
static int path_too_broad(const char *path)
{
    char rp[PATH_MAX];
    if (!path || !realpath(path, rp)) return 1;
    if (strcmp(rp, "/") == 0) return 1;
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");   /* Windows */
    char hr[PATH_MAX];
    if (home && *home && realpath(home, hr)) {
        size_t l = strlen(rp);
        if (strncmp(hr, rp, l) == 0 && (hr[l] == '\0' || hr[l] == '/'))
            return 1;
    }
    return 0;
}

/* The invocation directory is granted read-write-create - except when that
 * grant would be the whole filesystem (cwd "/"), or the user's home (a
 * build run from ~ executes the app's top-level code and the toolchain with
 * every file in ~ writable). `hull new` / `hull init` (scaffold) only write
 * their scaffold and run no app code: they still get a cwd of ~, never "/". */
static int cwd_grantable(int scaffold)
{
    char cwd[PATH_MAX];
    if (!realpath(".", cwd) || strcmp(cwd, "/") == 0) return 0;
    return scaffold || !path_too_broad(cwd);
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
                         int scaffold)
{
    if (!ctx) return -1;

    hl_tool_unveil_init(ctx);

    /* The output directory is granted read-write-create. `-o /app` made that
     * "/" - the whole filesystem writable to the app code a build runs (the
     * generated Dockerfile did exactly this) - and `-o ~/x` the whole home.
     * Refuse rather than grant it. */
    if (output_dir && dir_exists_p(output_dir) && path_too_broad(output_dir)) {
        log_error("[sandbox] tool mode: refusing to make '%s' writable (it is "
                  "the filesystem root, or your home directory or above); "
                  "write the output into a subdirectory", output_dir);
        return -1;
    }

    /* hull's own directory (the re-exec in manifest_extract_file.c, and the
     * platform archives beside it), read + execute. Resolved once for both
     * lists; never a directory that grants too much ("/hull", ~/hull). */
    char self_dir[PATH_MAX];
    int have_self_dir = 0;
    if (hl_release_io_self_path(self_dir, sizeof(self_dir)) == 0) {
        char *slash = strrchr(self_dir, '/');
        if (slash && slash != self_dir) {
            *slash = '\0';
            have_self_dir = !path_too_broad(self_dir);
        }
    }
    if (platform_dir && path_too_broad(platform_dir))
        platform_dir = NULL;   /* argv[0] "/hull" made this "/" */

    /* App sources: read-only, and only when the path is a real directory
     * (see the output_dir note below) that is not "/" or $HOME or above -
     * the app code a build loads would otherwise read every file the user
     * owns. A command pointed there still reads the invocation directory if
     * that is grantable. */
    if (app_dir && dir_exists_p(app_dir) && path_too_broad(app_dir)) {
        log_warn("[sandbox] tool mode: not granting '%s' (the filesystem "
                 "root, or your home directory or above) as the app "
                 "directory", app_dir);
        app_dir = NULL;
    }
    if (app_dir && dir_exists_p(app_dir))
        hl_tool_unveil_add(ctx, app_dir, "r");

    /* Temp directory: read/write/create (build artifacts) */
    /* /tmp needs execute too: the build pipeline stages tooling and
     * intermediate artifacts there that must be executable. */
    hl_tool_unveil_add(ctx, "/tmp", "rwcx");

    /* System compilers and headers */
    hl_tool_unveil_add(ctx, "/usr", "rx");

#if defined(__COSMOPOLITAN__) || defined(__linux__)
    hl_tool_unveil_add(ctx, "/bin", "rx");   /* APE shebang invokes /bin/sh */
    hl_tool_unveil_add(ctx, "/lib", "rx");   /* shared libs need execute for mmap */
    hl_tool_unveil_add(ctx, "/lib64", "rx");
    /* /opt: `make fetch-cosmocc COSMOCC_DIR=/opt/cosmo` (CI) and other
     * package managers commonly install toolchains here. */
    hl_tool_unveil_add(ctx, "/opt", "rx");
    /* $HOME/.cosmocc and $HOME/cosmocc: cosmocc.zip default user-install
     * paths. Narrow - only the .cosmocc / cosmocc subtree, not all of $HOME. */
    {
        const char *home = getenv("HOME");
        if (home && *home) {
            char path[PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/.cosmocc", home);
            if (n > 0 && (size_t)n < sizeof(path))
                hl_tool_unveil_add(ctx, path, "rx");
            n = snprintf(path, sizeof(path), "%s/cosmocc", home);
            if (n > 0 && (size_t)n < sizeof(path))
                hl_tool_unveil_add(ctx, path, "rx");
        }
    }
#endif

#ifdef __APPLE__
    /* Homebrew compilers and Xcode CLT */
    hl_tool_unveil_add(ctx, "/opt", "rx");
    hl_tool_unveil_add(ctx, "/Library", "r");
#endif

    /* The invocation directory: `hull new` / `hull init` create their
     * scaffold relative to it. Unconditional, so `output_dir` is free to
     * carry where `hull build` actually writes rather than doubling as
     * this. */
    if (cwd_grantable(scaffold))
        hl_tool_unveil_add(ctx, ".", "rwc");

    /* Output directory: write/create. app_dir above is READ-ONLY, so a
     * build writing into the app tree (app.com, and the .hull/build the
     * post-link tidy-up moves debug sidecars into) depends on this.
     *
     * Only when it EXISTS. parse_app_dir returns the first positional
     * argument, which for a non-build subcommand is a word like "test"
     * rather than a directory - unveiling that grants nothing and, on
     * Linux, is a failed unveil against a path that is not there. */
    if (output_dir && dir_exists_p(output_dir))
        hl_tool_unveil_add(ctx, output_dir, "rwc");

    /* Hull runtime cache root - `hull build` writes AOT artifacts
     * into $HOME/.hull/cache/compute-aot/ for cross-app reuse. Same
     * auto-allow rationale as the bytecode cache (see seatbelt
     * counterpart in hl_sandbox_apply). hl_hull_cache_dir() mkdirs
     * the path as a side effect. */
    {
        char cache_path[PATH_MAX];
        if (hl_hull_cache_dir(cache_path, sizeof(cache_path)) == 0)
            hl_tool_unveil_add(ctx, cache_path, "rwc");
    }

    /* Platform library + hull binary: read + execute */
    if (platform_dir)
        hl_tool_unveil_add(ctx, platform_dir, "rx");

    /* hull re-execs ITSELF to isolate JS manifest extraction (issue #427), so
     * the running binary has to stay executable under this sandbox.
     * platform_dir covers it only when argv[0] carried a slash; the common
     * `hull build myapp` off $PATH leaves it NULL, and an install prefix like
     * ~/.local/bin is under none of the roots unveiled above. Resolve the real
     * path and unveil its DIRECTORY (not $HOME, not "/"). Best-effort: cosmo
     * has no self-path route, and the extraction falls back to in-process when
     * the spawn is refused. */
    if (have_self_dir)
        hl_tool_unveil_add(ctx, self_dir, "rx");

    /* Side-loaded tool assets: $HOME/.hull/tools holds tool binaries (wamrc,
     * lld) that get executed AND the libc-musl-<arch> floor bundle
     * (crt*.o / libc.a / libgcc.a) that Tier B's `ld.lld` reads at link time.
     * Read + execute; narrow to the .hull/tools subtree, not all of $HOME. */
    {
        const char *home = getenv("HOME");
        if (!home || !*home) home = getenv("USERPROFILE");   /* Windows */
        if (home && *home) {
            char path[PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/.hull/tools", home);
            if (n > 0 && (size_t)n < sizeof(path))
                hl_tool_unveil_add(ctx, path, "rx");
        }
    }

    /* cosmo/Windows: the shared build temp dir (~/.hull/tmp) that cosmocc's
     * driver - driven through the bundled busybox - writes its mktemper scratch
     * to. It must be creatable + writable + executable; without it the child
     * busybox is denied mkdir and cosmocc fails "nonexistent directory". unveil
     * needs the path to EXIST when added and the sandbox seals before the build
     * could create it, so hl_tool_cosmo_prepare_tmpdir creates it here (pre-seal,
     * filesystem still open) and records the SAME forward-slash path the reroute
     * later exports as TMPDIR (so the unveil string matches what busybox uses).
     * No-op / returns -1 off a cosmo hull on Windows. */
    {
        hl_tool_cosmo_prepare_tmpdir();
        char td[PATH_MAX];
        if (hl_tool_cosmo_tmpdir(td, sizeof(td)) == 0)
            hl_tool_unveil_add(ctx, td, "rwcx");
    }

    hl_tool_unveil_seal(ctx);

    /* Also apply kernel-level unveil on supported platforms. Fails closed:
     * a failed unveil or pledge on a host whose kernel enforces them is an
     * error, not a log line. */
    int kfail = 0;
    int kernel = sb_supported();
    if (kernel) {
        /* The first rule doubles as the probe a cosmo APE needs: it gates on
         * IsLinux() alone, and on a Linux kernel without Landlock its
         * unveil() fails ENOSYS - which made every tool command refuse to
         * run. ENOSYS means nothing was applied: that host simply has no
         * kernel tool sandbox, as a native build on it reports. */
        if (unveil("/tmp", "rwcx") != 0) {
            if (errno == ENOSYS) {
                kernel = 0;
            } else {
                log_error("[sandbox] tool mode: unveil(/tmp, rwcx) failed");
                kfail++;
            }
        }
    }
    if (kernel) {
        if (app_dir && dir_exists_p(app_dir)) kunveil(app_dir, "r", 0, &kfail);
        kunveil("/usr", "rx", 1, &kfail);
#if defined(__COSMOPOLITAN__) || defined(__linux__)
        kunveil("/bin", "rx", 1, &kfail);   /* APE shebang invokes /bin/sh */
        kunveil("/lib", "rx", 1, &kfail);   /* shared libs need execute for mmap */
        kunveil("/lib64", "rx", 1, &kfail);
        kunveil("/opt", "rx", 1, &kfail);
        {
            const char *home = getenv("HOME");
            if (home && *home) {
                char path[PATH_MAX];
                int n = snprintf(path, sizeof(path), "%s/.cosmocc", home);
                if (n > 0 && (size_t)n < sizeof(path))
                    kunveil(path, "rx", 1, &kfail);
                n = snprintf(path, sizeof(path), "%s/cosmocc", home);
                if (n > 0 && (size_t)n < sizeof(path))
                    kunveil(path, "rx", 1, &kfail);
            }
        }
#endif
#ifdef __APPLE__
        kunveil("/opt", "rx", 1, &kfail);
        kunveil("/Library", "r", 1, &kfail);
#endif
        /* Mirror of the userspace grant above. These two lists must stay in
         * step: the ctx is what hl_tool_* check, the kernel unveil is what
         * actually stops a write. Adding "." to only the first is how
         * moving output_dir off "." quietly removed the CWD's kernel
         * grant on Linux, while Windows - which has no kernel sandbox -
         * looked fine. */
        if (cwd_grantable(scaffold)) kunveil(".", "rwc", 0, &kfail);
        if (output_dir && dir_exists_p(output_dir))
            kunveil(output_dir, "rwc", 0, &kfail);
        if (platform_dir) kunveil(platform_dir, "rx", 1, &kfail);
        /* hull's own directory: it was in the userspace list only, so with
         * Landlock the #427 re-exec got EACCES and quietly fell back to
         * in-process manifest extraction. */
        if (have_self_dir) kunveil(self_dir, "rx", 1, &kfail);
        {
            char cache_path[PATH_MAX];
            if (hl_hull_cache_dir(cache_path, sizeof(cache_path)) == 0)
                kunveil(cache_path, "rwc", 1, &kfail);
        }
        {
            const char *home = getenv("HOME");
            if (home && *home) {
                char path[PATH_MAX];
                int n = snprintf(path, sizeof(path), "%s/.hull/tools", home);
                if (n > 0 && (size_t)n < sizeof(path))
                    kunveil(path, "rx", 1, &kfail);   /* installed tools + Tier B floor bundle */
            }
        }
        if (unveil(NULL, NULL) != 0) {   /* seal */
            log_error("[sandbox] tool mode: sealing unveil failed");
            kfail++;
        }
    }
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
     * the cap. The temp dir is added last, so an overflow shows up as
     * unrelated tempdir failures far from here. Say so. */
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
