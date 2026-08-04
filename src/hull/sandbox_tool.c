/*
 * sandbox_tool.c — build-tool ("hull build") kernel sandbox.
 *
 * Split out of sandbox.c so the app-runtime sandbox (hl_sandbox_apply /
 * _apply_pledge / _policy_from_manifest) can ship without the build-tool
 * unveil machinery. `hl_tool_sandbox_init` is the only sandbox entry
 * point that references cap/tool.c's hl_tool_unveil_* helpers, which in
 * turn live next to the compiler-spawn surface — none of which belongs
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
#include "log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ── Platform pledge/unveil providers (mirror of sandbox.c) ────────── */

#if defined(__COSMOPOLITAN__)

extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);
static int sb_supported(void) { return 1; }

#elif defined(__OpenBSD__)

/* pledge()/unveil() are native — declared in <unistd.h> (already included). */
static int sb_supported(void) { return 1; }

#elif defined(__linux__)

extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);
static int sb_supported(void) { return 1; }

#elif defined(__APPLE__)

/* macOS has no pledge/unveil — no-ops. The tool-mode allowlist is still
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

static int sb_supported(void) { return 1; }

#else /* unsupported platforms — full no-op */

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

#endif /* platform dispatch */

/* ── Tool-mode sandbox ─────────────────────────────────────────────── */

int hl_tool_sandbox_init(HlToolUnveilCtx *ctx,
                         const char *app_dir,
                         const char *output_dir,
                         const char *platform_dir)
{
    if (!ctx) return -1;

    hl_tool_unveil_init(ctx);

    /* App sources: read-only */
    if (app_dir)
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
     * paths. Narrow — only the .cosmocc / cosmocc subtree, not all of $HOME. */
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

    /* Output directory: write/create */
    if (output_dir)
        hl_tool_unveil_add(ctx, output_dir, "rwc");

    /* Hull runtime cache root — `hull build` writes AOT artifacts
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

    /* Also apply kernel-level unveil on supported platforms */
    if (sb_supported()) {
        if (app_dir)     unveil(app_dir, "r");
        unveil("/tmp", "rwcx");
        unveil("/usr", "rx");
#if defined(__COSMOPOLITAN__) || defined(__linux__)
        unveil("/bin", "rx");   /* APE shebang invokes /bin/sh */
        unveil("/lib", "rx");   /* shared libs need execute for mmap */
        unveil("/lib64", "rx");
        unveil("/opt", "rx");
        {
            const char *home = getenv("HOME");
            if (home && *home) {
                char path[PATH_MAX];
                int n = snprintf(path, sizeof(path), "%s/.cosmocc", home);
                if (n > 0 && (size_t)n < sizeof(path))
                    unveil(path, "rx");
                n = snprintf(path, sizeof(path), "%s/cosmocc", home);
                if (n > 0 && (size_t)n < sizeof(path))
                    unveil(path, "rx");
            }
        }
#endif
#ifdef __APPLE__
        unveil("/opt", "rx");
        unveil("/Library", "r");
#endif
        if (output_dir)   unveil(output_dir, "rwc");
        if (platform_dir) unveil(platform_dir, "rx");
        {
            char cache_path[PATH_MAX];
            if (hl_hull_cache_dir(cache_path, sizeof(cache_path)) == 0)
                unveil(cache_path, "rwc");
        }
        {
            const char *home = getenv("HOME");
            if (home && *home) {
                char path[PATH_MAX];
                int n = snprintf(path, sizeof(path), "%s/.hull/tools", home);
                if (n > 0 && (size_t)n < sizeof(path))
                    unveil(path, "rx");   /* installed tools + Tier B floor bundle */
            }
        }
        unveil(NULL, NULL); /* seal */

        /* Pledge for tool mode: needs proc + exec for fork/execvp */
        pledge("stdio rpath wpath cpath proc exec fattr", NULL);
    }

    log_info("[sandbox] tool mode applied (%d unveiled paths)",
             ctx->count);

    return 0;
}
