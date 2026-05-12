/*
 * sandbox.c — Kernel-level sandbox enforcement
 *
 * Applies OS-level restrictions derived from the application manifest.
 * After hl_sandbox_apply(), the process can only access the filesystem
 * paths and syscall families that the manifest declares.
 *
 * Platform implementations:
 *   OpenBSD       — native pledge() + unveil() (originated here)
 *   Cosmopolitan  — pledge() + unveil() built into cosmo libc
 *   Linux 5.13+   — jart/pledge polyfill (seccomp-bpf + landlock)
 *   macOS         — Seatbelt (sandbox_init_with_parameters)
 *   other         — no-op (C-level cap validation is the defense)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/sandbox.h"
#include "log.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ── Platform pledge/unveil providers ──────────────────────────────── */

#if defined(__COSMOPOLITAN__)

/* Cosmopolitan libc provides pledge() and unveil() natively. */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);

static int sb_supported(void) { return 1; }

#elif defined(__OpenBSD__)

/*
 * OpenBSD: pledge() and unveil() are native system calls.
 * This is where these APIs originated — declared in <unistd.h>.
 */
#include <unistd.h>

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

#elif defined(__APPLE__)

/*
 * macOS: Seatbelt sandbox via sandbox_init_with_parameters().
 * This is a private API (not in public SDK headers) but is exported
 * by libsystem_sandbox.dylib and used by Chrome, Firefox, and macOS
 * system services.  Declared manually via extern.
 *
 * sandbox_init is irreversible — once applied, cannot be modified.
 * Phase 1 (pledge) is a no-op on macOS; phase 2 applies the full
 * Seatbelt profile.
 */
/* sandbox.h is deprecated since macOS 10.8 — use free() for error strings */
#include <stdlib.h> /* free */

extern int sandbox_init_with_parameters(const char *profile,
                                         uint64_t flags,
                                         const char **parameters,
                                         char **errorbuf);

/* pledge/unveil remain as no-ops — used by tool mode and phase 1 */
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

/* ── Seatbelt profile builder ─────────────────────────────────────── */

/*
 * SBPL (Sandbox Profile Language) is a Scheme dialect used by macOS
 * kernel sandbox.  We build a profile string dynamically from the
 * manifest and pass path values via parameter substitution to avoid
 * escaping issues with special characters in paths.
 *
 * Parameter substitution: (param "KEY") in SBPL, matched to key-value
 * pairs passed to sandbox_init_with_parameters().
 */

#define SEATBELT_PROFILE_SIZE  8192
#define SEATBELT_MAX_PARAMS    256   /* key-value pairs + NULL terminator */
#define SEATBELT_PATH_SIZE     4096  /* max path length for resolved paths */

/* realpath(3) writes up to PATH_MAX bytes — assert our buffer is large enough */
_Static_assert(SEATBELT_PATH_SIZE >= PATH_MAX,
               "SEATBELT_PATH_SIZE must be >= PATH_MAX");

/* Scratch buffers for derived paths (stack-allocated in caller) */
typedef struct {
    char db_dir[SEATBELT_PATH_SIZE];
    char fs_read_keys[HL_MANIFEST_MAX_PATHS][16];    /* "FS_R_0" .. "FS_R_31" */
    char fs_write_keys[HL_MANIFEST_MAX_PATHS][16];   /* "FS_W_0" .. "FS_W_31" */
    char fs_read_real[HL_MANIFEST_MAX_PATHS][SEATBELT_PATH_SIZE];
    char fs_write_real[HL_MANIFEST_MAX_PATHS][SEATBELT_PATH_SIZE];
} SeatbeltScratch;

/*
 * Build a Seatbelt SBPL profile and params array from manifest + paths.
 *
 * profile_buf: output buffer for SBPL string (SEATBELT_PROFILE_SIZE)
 * params:      output array for key-value pairs (SEATBELT_MAX_PARAMS)
 * scratch:     scratch buffers for derived paths
 *
 * Returns 0 on success, -1 on buffer overflow.
 */
static int seatbelt_build_profile(const HlManifest *manifest,
                                   const char *app_dir,
                                   const char *db_path,
                                   const char *ca_bundle_path,
                                   const char *tls_cert_path,
                                   const char *tls_key_path,
                                   char *profile_buf,
                                   size_t profile_size,
                                   const char **params,
                                   size_t max_params,
                                   SeatbeltScratch *scratch)
{
    size_t pos = 0;
    size_t pi = 0;  /* params index */

    /* Macros for safe appending */
    #define SBPL_LIT(s) do { \
        int _n = snprintf(profile_buf + pos, profile_size - pos, "%s", (s)); \
        if (_n < 0 || (size_t)_n >= profile_size - pos) return -1; \
        pos += (size_t)_n; \
    } while (0)

    #define SBPL_FMT(fmt, arg) do { \
        int _n = snprintf(profile_buf + pos, profile_size - pos, (fmt), (arg)); \
        if (_n < 0 || (size_t)_n >= profile_size - pos) return -1; \
        pos += (size_t)_n; \
    } while (0)

    #define PARAM_ADD(key, val) do { \
        if (pi + 2 >= max_params) return -1; \
        params[pi++] = (key); \
        params[pi++] = (val); \
    } while (0)

    /* ── Profile header ─────────────────────────────────────── */

    SBPL_LIT("(version 1)\n");
    SBPL_LIT("(deny default)\n\n");

    /* stat()/lstat() on any path — kernel needs this to traverse parent
     * directories (e.g. /private, /private/var) when opening files inside
     * allowed subpaths.  Only exposes metadata, not file content. */
    SBPL_LIT("(allow file-read-metadata)\n\n");

    /* ── System frameworks (dyld, libc, libsystem) ──────────── */

    SBPL_LIT("; System frameworks and libraries\n"
             "(allow file-read*\n"
             "    (subpath \"/System/Library\")\n"
             "    (subpath \"/usr/lib\")\n"
             "    (subpath \"/private/var/db/dyld\")\n"
             "    (subpath \"/Library/Preferences/Logging\")\n"
             "    (literal \"/usr/share/icu\")\n"
             "    (subpath \"/usr/share/icu\"))\n"
             "(allow file-map-executable\n"
             "    (subpath \"/System/Library\")\n"
             "    (subpath \"/usr/lib\"))\n\n");

    /* ── Essential devices ──────────────────────────────────── */

    SBPL_LIT("; Essential devices\n"
             "(allow file-read-data\n"
             "    (literal \"/dev/urandom\")\n"
             "    (literal \"/dev/random\"))\n"
             "(allow file-read* file-write-data\n"
             "    (literal \"/dev/null\")\n"
             "    (literal \"/dev/dtracehelper\"))\n"
             "(allow file-ioctl\n"
             "    (literal \"/dev/dtracehelper\"))\n\n");

    /* ── App directory (read-only) ──────────────────────────── */

    if (app_dir) {
        PARAM_ADD("APP_DIR", app_dir);
        SBPL_LIT("; App directory (read-only)\n"
                 "(allow file-read* (subpath (param \"APP_DIR\")))\n\n");
    }

    /* ── SQLite database (4 file variants) ──────────────────── */

    if (db_path) {
        /* Extract parent directory — SQLite needs the dir for locking/fsync,
         * and subpath covers the db file plus WAL/SHM/journal variants */
        snprintf(scratch->db_dir, sizeof(scratch->db_dir), "%s", db_path);
        char *slash = strrchr(scratch->db_dir, '/');
        if (slash && slash != scratch->db_dir)
            *slash = '\0';
        else
            snprintf(scratch->db_dir, sizeof(scratch->db_dir), ".");

        PARAM_ADD("DB_DIR", scratch->db_dir);

        SBPL_LIT("; SQLite database directory (covers db, WAL, SHM, journal)\n"
                 "(allow file-read* file-write*\n"
                 "    (subpath (param \"DB_DIR\")))\n\n");
    }

    /* ── Manifest fs_read[] paths ───────────────────────────── */

    for (int i = 0; i < manifest->fs_read_count; i++) {
        snprintf(scratch->fs_read_keys[i],
                 sizeof(scratch->fs_read_keys[i]), "FS_R_%d", i);
        /* Resolve symlinks — Seatbelt matches real paths */
        const char *rpath = manifest->fs_read[i];
        if (realpath(rpath, scratch->fs_read_real[i]))
            rpath = scratch->fs_read_real[i];
        PARAM_ADD(scratch->fs_read_keys[i], rpath);
        SBPL_FMT("(allow file-read* (subpath (param \"%s\")))\n",
                  scratch->fs_read_keys[i]);
    }

    /* ── Manifest fs_write[] paths ──────────────────────────── */

    for (int i = 0; i < manifest->fs_write_count; i++) {
        snprintf(scratch->fs_write_keys[i],
                 sizeof(scratch->fs_write_keys[i]), "FS_W_%d", i);
        /* Resolve symlinks — Seatbelt matches real paths */
        const char *wpath = manifest->fs_write[i];
        if (realpath(wpath, scratch->fs_write_real[i]))
            wpath = scratch->fs_write_real[i];
        PARAM_ADD(scratch->fs_write_keys[i], wpath);
        SBPL_FMT("(allow file-read* file-write*"
                  " (subpath (param \"%s\")))\n",
                  scratch->fs_write_keys[i]);
    }

    /* ── CA bundle, TLS cert, TLS key ───────────────────────── */

    if (ca_bundle_path) {
        PARAM_ADD("CA_BUNDLE", ca_bundle_path);
        SBPL_LIT("(allow file-read* (literal (param \"CA_BUNDLE\")))\n");
    }
    if (tls_cert_path) {
        PARAM_ADD("TLS_CERT", tls_cert_path);
        SBPL_LIT("(allow file-read* (literal (param \"TLS_CERT\")))\n");
    }
    if (tls_key_path) {
        PARAM_ADD("TLS_KEY", tls_key_path);
        SBPL_LIT("(allow file-read* (literal (param \"TLS_KEY\")))\n");
    }

    SBPL_LIT("\n");

    /* ── Network ────────────────────────────────────────────── */

    SBPL_LIT("; Network (server socket already bound)\n"
             "(allow network-inbound network-bind)\n"
             "(allow system-socket)\n");
    if (manifest->hosts_count > 0) {
        SBPL_LIT("(allow network-outbound)\n");
    }
    SBPL_LIT("\n");

    /* ── Mach services (logging, DNS, essential) ────────────── */

    SBPL_LIT("; Mach services\n"
             "(allow mach-lookup (global-name\n"
             "    \"com.apple.system.logger\"\n"
             "    \"com.apple.system.notification_center\"\n"
             "    \"com.apple.logd\"\n"
             "    \"com.apple.diagnosticd\"))\n\n");

    /* ── GPU compute (Metal) ─────────────────────────────────── */

    if (manifest->gpu) {
        SBPL_LIT("; GPU compute (Metal)\n"
                 "(allow iokit-open)\n"
                 "(allow mach-lookup (global-name\n"
                 "    \"com.apple.MTLCompilerService\"))\n\n");
    }

    /* ── Signals, sysctl ────────────────────────────────────── */

    SBPL_LIT("; Signals and sysctl\n"
             "(allow signal (target self))\n"
             "(allow sysctl-read)\n\n");

    /* ── Block dangerous operations ─────────────────────────── */

    SBPL_LIT("; Block exec and fork\n"
             "(deny process-exec*)\n"
             "(deny process-fork)\n");

    #undef SBPL_LIT
    #undef SBPL_FMT
    #undef PARAM_ADD

    /* NULL-terminate params array */
    params[pi] = NULL;

    return 0;
}

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

/* ── Phase 1: pre-load pledge ──────────────────────────────────────── */

int hl_sandbox_apply_pledge(void)
{
    if (!sb_supported()) {
        log_info("[sandbox] kernel sandbox not available on this platform");
        return 0;
    }

#ifdef __APPLE__
    /* sandbox_init is irreversible — can't do two-phase on macOS.
     * Phase 2 applies the full Seatbelt profile after manifest extraction.
     * C-level runtime sandboxes (Lua: os removed, JS: no std/os) prevent
     * exec during load_app(). */
    log_info("[sandbox] phase 1 skipped (macOS seatbelt applied in phase 2)");
    return 0;
#endif

#ifdef __linux__
    __pledge_mode = 0x0001 | 0x0010; /* KILL_PROCESS | STDERR_LOGGING */
#endif

    if (pledge("stdio inet rpath wpath cpath flock dns unveil", NULL) != 0) {
        log_error("[sandbox] phase 1 pledge failed");
        return -1;
    }

    log_info("[sandbox] phase 1 pledge applied (exec/proc/fork blocked)");
    return 0;
}

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

#if defined(__COSMOPOLITAN__) || defined(__linux__)
    hl_tool_unveil_add(ctx, "/bin", "rx");   /* APE shebang invokes /bin/sh */
    hl_tool_unveil_add(ctx, "/lib", "rx");   /* shared libs need execute for mmap */
    hl_tool_unveil_add(ctx, "/lib64", "rx");
#endif

#ifdef __APPLE__
    /* Homebrew compilers and Xcode CLT */
    hl_tool_unveil_add(ctx, "/opt", "rx");
    hl_tool_unveil_add(ctx, "/Library", "r");
#endif

    /* Output directory: write/create */
    if (output_dir)
        hl_tool_unveil_add(ctx, output_dir, "rwc");

    /* Platform library + hull binary: read + execute */
    if (platform_dir)
        hl_tool_unveil_add(ctx, platform_dir, "rx");

    hl_tool_unveil_seal(ctx);

    /* Also apply kernel-level unveil on supported platforms */
    if (sb_supported()) {
        if (app_dir)     unveil(app_dir, "r");
        unveil("/tmp", "rwc");
        unveil("/usr", "rx");
#if defined(__COSMOPOLITAN__) || defined(__linux__)
        unveil("/bin", "rx");   /* APE shebang invokes /bin/sh */
        unveil("/lib", "rx");   /* shared libs need execute for mmap */
        unveil("/lib64", "rx");
#endif
#ifdef __APPLE__
        unveil("/opt", "rx");
        unveil("/Library", "r");
#endif
        if (output_dir)   unveil(output_dir, "rwc");
        if (platform_dir) unveil(platform_dir, "rx");
        unveil(NULL, NULL); /* seal */

        /* Pledge for tool mode: needs proc + exec for fork/execvp */
        pledge("stdio rpath wpath cpath proc exec fattr", NULL);
    }

    log_info("[sandbox] tool mode applied (%d unveiled paths)",
             ctx->count);

    return 0;
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_sandbox_apply(const HlManifest *manifest, const char *app_dir,
                      const char *db_path,
                      const char *ca_bundle_path,
                      const char *tls_cert_path,
                      const char *tls_key_path)
{
    if (!manifest)
        return 0;

    if (!sb_supported()) {
        log_info("[sandbox] kernel sandbox not available on this platform");
        return 0;
    }

#ifdef __APPLE__
    /* ── macOS: Seatbelt sandbox ──────────────────────────────── */
    {
        char profile_buf[SEATBELT_PROFILE_SIZE];
        const char *params[SEATBELT_MAX_PARAMS];
        SeatbeltScratch scratch;

        /* Resolve symlinks — Seatbelt matches against real paths.
         * e.g. /tmp → /private/tmp on macOS.  Fall back to original
         * if realpath fails (path may not exist yet). */
        char real_app[SEATBELT_PATH_SIZE], real_db[SEATBELT_PATH_SIZE];
        const char *resolved_app = app_dir;
        const char *resolved_db = db_path;

        if (app_dir && realpath(app_dir, real_app))
            resolved_app = real_app;
        if (db_path && realpath(db_path, real_db))
            resolved_db = real_db;

        if (seatbelt_build_profile(manifest, resolved_app, resolved_db,
                                    ca_bundle_path, tls_cert_path,
                                    tls_key_path,
                                    profile_buf, sizeof(profile_buf),
                                    params, SEATBELT_MAX_PARAMS,
                                    &scratch) != 0) {
            log_error("[sandbox] seatbelt profile too large");
            return -1;
        }

        char *errorbuf = NULL;
        if (sandbox_init_with_parameters(profile_buf, 0,
                                          params, &errorbuf) != 0) {
            log_error("[sandbox] seatbelt init failed: %s",
                       errorbuf ? errorbuf : "unknown error");
            if (errorbuf)
                free(errorbuf);
            return -1;
        }

        log_info("[sandbox] applied (seatbelt: %d read, %d write%s)",
                 manifest->fs_read_count, manifest->fs_write_count,
                 manifest->hosts_count > 0 ? ", network-outbound" : "");
        return 0;
    }
#endif /* __APPLE__ */

#ifdef __linux__
    /* Kill the process on violation + log to stderr */
    __pledge_mode = 0x0001 | 0x0010; /* KILL_PROCESS | STDERR_LOGGING */
#endif

    /* ── Unveil: restrict filesystem visibility ─────────────── */

    /* App directory: always readable (templates, static assets, source) */
    if (app_dir) {
        if (unveil(app_dir, "r") != 0)
            log_warn("[sandbox] unveil failed for app dir: %s", app_dir);
    }

    /* /dev/urandom: needed by crypto.random and password hashing */
    unveil("/dev/urandom", "r");

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

    /* TLS certificate and private key for HTTPS server */
    if (tls_cert_path) {
        if (unveil(tls_cert_path, "r") != 0)
            log_warn("[sandbox] unveil failed for TLS cert: %s",
                     tls_cert_path);
    }
    if (tls_key_path) {
        if (unveil(tls_key_path, "r") != 0)
            log_warn("[sandbox] unveil failed for TLS key: %s",
                     tls_key_path);
    }

    /* GPU compute (Vulkan) */
    if (manifest->gpu) {
        if (manifest->gpu_device_count > 0) {
            /* Unveil only specific render nodes for allowed devices */
            for (int i = 0; i < manifest->gpu_device_count; i++) {
                char node[64];
                snprintf(node, sizeof(node), "/dev/dri/renderD%d",
                         128 + manifest->gpu_devices[i]);
                if (unveil(node, "rw") != 0)
                    log_warn("[sandbox] unveil failed for %s", node);
            }
        } else {
            /* No device restriction — unveil all of /dev/dri */
            if (unveil("/dev/dri", "rw") != 0)
                log_warn("[sandbox] unveil failed for /dev/dri");
        }
        if (unveil("/proc/self", "r") != 0)
            log_warn("[sandbox] unveil failed for /proc/self");
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
    int plen = snprintf(promises, sizeof(promises),
                        "stdio inet rpath wpath cpath flock");
    if (plen > 0 && manifest->hosts_count > 0 &&
        (size_t)plen < sizeof(promises))
        snprintf(promises + plen, sizeof(promises) - (size_t)plen, " dns");

    if (pledge(promises, NULL) != 0) {
        log_error("[sandbox] pledge failed");
        return -1;
    }

    log_info("[sandbox] applied (unveil: %d read, %d write; pledge: %s)",
             manifest->fs_read_count, manifest->fs_write_count, promises);

    return 0;
}
