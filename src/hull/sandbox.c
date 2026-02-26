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
#include <libc/calls/pledge.h>

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

/* ── Public API ────────────────────────────────────────────────────── */

int hl_sandbox_apply(const HlManifest *manifest, const char *db_path)
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
