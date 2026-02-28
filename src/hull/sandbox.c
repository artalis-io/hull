/*
 * sandbox.c — Kernel-level sandbox enforcement
 *
 * Applies pledge()/unveil() restrictions derived from the
 * application manifest.  After hl_sandbox_apply(), the process
 * can only access the filesystem paths and syscall families that
 * the manifest declares.
 *
 * Platform implementations:
 *   Cosmopolitan  — pledge() + unveil() built into cosmo libc
 *   Linux 5.13+   — jart/pledge polyfill (seccomp-bpf + landlock)
 *   macOS/other   — no-op (C-level cap validation is the defense)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/sandbox.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

/* ── Platform pledge/unveil providers ──────────────────────────────── */

#if defined(__COSMOPOLITAN__)

/* Cosmopolitan libc provides pledge() and unveil() natively. */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);

static int sb_supported(void) { return 1; }

#elif defined(__linux__)

/*
 * Linux: jart/pledge polyfill provides pledge() + unveil()
 * using seccomp-bpf (syscall restriction) and Landlock (filesystem).
 * The polyfill gracefully returns 0 if the kernel is too old.
 */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);
extern int __pledge_mode;

static int sb_supported(void) { return 1; }

#else /* macOS, other */

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
    hl_tool_unveil_add(ctx, "/tmp", "rwc");

    /* System compilers and headers */
    hl_tool_unveil_add(ctx, "/usr", "rx");

#ifdef __linux__
    hl_tool_unveil_add(ctx, "/lib", "r");
    hl_tool_unveil_add(ctx, "/lib64", "r");
#endif

#ifdef __APPLE__
    /* Homebrew compilers and Xcode CLT */
    hl_tool_unveil_add(ctx, "/opt", "rx");
    hl_tool_unveil_add(ctx, "/Library", "r");
#endif

    /* Output directory: write/create */
    if (output_dir)
        hl_tool_unveil_add(ctx, output_dir, "rwc");

    /* Platform library: read */
    if (platform_dir)
        hl_tool_unveil_add(ctx, platform_dir, "r");

    hl_tool_unveil_seal(ctx);

    /* Also apply kernel-level unveil on supported platforms */
    if (sb_supported()) {
        if (app_dir)     unveil(app_dir, "r");
        unveil("/tmp", "rwc");
        unveil("/usr", "rx");
#ifdef __linux__
        unveil("/lib", "r");
        unveil("/lib64", "r");
#endif
#ifdef __APPLE__
        unveil("/opt", "rx");
        unveil("/Library", "r");
#endif
        if (output_dir)   unveil(output_dir, "rwc");
        if (platform_dir) unveil(platform_dir, "r");
        unveil(NULL, NULL); /* seal */

        /* Pledge for tool mode: needs proc + exec for fork/execvp */
        pledge("stdio rpath wpath cpath proc exec fattr", NULL);
    }

    log_info("[sandbox] tool mode applied (%d unveiled paths)",
             ctx->count);

    return 0;
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_sandbox_apply(const HlManifest *manifest, const char *db_path,
                      const char *ca_bundle_path)
{
    if (!manifest || !manifest->present) {
        log_info("[sandbox] no manifest — sandbox not applied");
        return 0;
    }

    if (!sb_supported()) {
        log_info("[sandbox] kernel sandbox not available on this platform");
        return 0;
    }

#ifdef __linux__
    /* Kill the process on violation + log to stderr */
    __pledge_mode = 0x0001 | 0x0010; /* KILL_PROCESS | STDERR_LOGGING */
#endif

    /* ── Unveil: restrict filesystem visibility ─────────────── */

    for (int i = 0; i < manifest->fs_read_count; i++) {
        if (unveil(manifest->fs_read[i], "r") != 0)
            log_warn("[sandbox] unveil failed for read path: %s",
                     manifest->fs_read[i]);
    }

    for (int i = 0; i < manifest->fs_write_count; i++) {
        if (unveil(manifest->fs_write[i], "rwc") != 0)
            log_warn("[sandbox] unveil failed for write path: %s",
                     manifest->fs_write[i]);
    }

    /* SQLite database always needs read + write + create */
    if (db_path) {
        if (unveil(db_path, "rwc") != 0)
            log_warn("[sandbox] unveil failed for database: %s", db_path);
    }

    /* CA certificate bundle for HTTPS client verification */
    if (ca_bundle_path) {
        if (unveil(ca_bundle_path, "r") != 0)
            log_warn("[sandbox] unveil failed for CA bundle: %s",
                     ca_bundle_path);
    }

    /* Seal: no more unveil calls allowed */
    if (unveil(NULL, NULL) != 0) {
        log_error("[sandbox] failed to seal filesystem restrictions");
        return -1;
    }

    /* ── Pledge: restrict syscall families ──────────────────── */

    /*
     * Build promise string from manifest capabilities.
     *
     * Always needed:
     *   stdio  — basic I/O on open fds, epoll/kqueue, signals
     *   inet   — accept connections on the bound server socket
     *   rpath  — SQLite reads, app file reads
     *   wpath  — SQLite WAL writes
     *   cpath  — SQLite journal/WAL creation
     *   flock  — SQLite locking
     *
     * Conditional:
     *   dns    — outbound HTTP name resolution (when hosts declared)
     */
    char promises[256];
    snprintf(promises, sizeof(promises),
             "stdio inet rpath wpath cpath flock");

    if (manifest->hosts_count > 0) {
        size_t len = strlen(promises);
        snprintf(promises + len, sizeof(promises) - len, " dns");
    }

    if (pledge(promises, NULL) != 0) {
        log_error("[sandbox] pledge failed");
        return -1;
    }

    log_info("[sandbox] applied (unveil: %d read, %d write; pledge: %s)",
             manifest->fs_read_count, manifest->fs_write_count, promises);

    return 0;
}
