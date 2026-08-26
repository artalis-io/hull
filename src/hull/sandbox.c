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
#include "hull/shared/cache_dir.h"
#include "log.h"
#ifdef HL_ENABLE_DB
#include "hull/cap/db_backend.h"   /* hl_db_feature_backends: detect a composed DuckDB feature */
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp (DSN scheme match) */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>    /* isatty / STDIN_FILENO (controlling-terminal probe) */

/* Buffer size for cross-platform path resolution. Big enough to
 * hold any realpath() output (PATH_MAX-bound) and any reasonable
 * app_dir + relpath concatenation. Used by
 * sandbox_resolve_manifest_path; the Apple-side SeatbeltScratch
 * has its own equivalent (SEATBELT_PATH_SIZE) for SBPL-time
 * scratch, kept separate so its name still telegraphs context. */
#define SANDBOX_PATH_MAX  4096
_Static_assert(SANDBOX_PATH_MAX >= PATH_MAX,
               "SANDBOX_PATH_MAX must be >= PATH_MAX");

/* Resolve a manifest fs.read / fs.write path against the app's
 * root directory. The manifest declares paths like "data/" or
 * "uploads/", which the capability layer interprets relative to
 * app_dir (HlFsConfig.base_dir). The sandbox MUST match that
 * resolution — otherwise the seatbelt subpath rule (or unveil
 * subtree) covers a different directory than the one the app
 * actually writes to. Steps:
 *
 *   1. Validate shape: reject absolute paths and any segment
 *      containing "..". Defense-in-depth — manifest parser only
 *      truncates.
 *   2. Concatenate app_dir + "/" + relpath into a temp buffer.
 *   3. mkdir -p so realpath() can canonicalize even when the
 *      app hasn't created the dir yet (hull/blob@1 makes its
 *      root lazily on init).
 *   4. realpath() the result into out_abs.
 *
 * Returns 0 on success (out_abs filled), -1 on rejection or
 * buffer overflow. On -1 the caller should log + skip — never
 * fall back to an unresolved relative path, since unveil() and
 * seatbelt both reject those.
 *
 * Platform-agnostic — both the seatbelt SBPL build (macOS) and
 * the unveil setup (Linux / OpenBSD / Cosmopolitan) call this.
 */
static int sandbox_resolve_manifest_path(const char *app_dir,
                                          const char *relpath,
                                          char *out_abs,
                                          size_t out_cap)
{
    if (!app_dir || !relpath || !out_abs) return -1;
    if (relpath[0] == '/' || relpath[0] == '\0') return -1;
    for (const char *p = relpath; *p; ) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\0')) return -1;
        const char *slash = strchr(p, '/');
        if (!slash) break;
        p = slash + 1;
    }

    size_t adir_len = strlen(app_dir);
    size_t rel_len  = strlen(relpath);
    while (adir_len > 1 && app_dir[adir_len - 1] == '/') adir_len--;
    if (adir_len + 1 + rel_len + 1 > out_cap) return -1;

    char joined[SANDBOX_PATH_MAX];
    if (adir_len + 1 + rel_len + 1 > sizeof(joined)) return -1;
    memcpy(joined, app_dir, adir_len);
    joined[adir_len] = '/';
    memcpy(joined + adir_len + 1, relpath, rel_len + 1);

    /* mkdir -p — best-effort. EEXIST is harmless. */
    char buf[SANDBOX_PATH_MAX];
    size_t jlen = strlen(joined);
    if (jlen >= sizeof(buf)) return -1;
    memcpy(buf, joined, jlen + 1);
    while (jlen > 1 && buf[jlen - 1] == '/') buf[--jlen] = '\0';
    for (size_t k = 1; k <= jlen; k++) {
        if (buf[k] == '/' || buf[k] == '\0') {
            char saved = buf[k];
            buf[k] = '\0';
            (void)mkdir(buf, 0755);
            buf[k] = saved;
        }
    }

    if (realpath(joined, out_abs) == NULL) {
        size_t need = strlen(joined) + 1;
        if (need > out_cap) return -1;
        memcpy(out_abs, joined, need);
    }
    return 0;
}

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
/* sandbox.h is deprecated since macOS 10.8 — use free() for error strings.
 * stdlib.h is included at the top of the file. */

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

/* ── Hardened Runtime probe (release-only fail-closed) ─────────────
 *
 * `csops` is a private but stable syscall used by Apple's own services
 * to read code-signing flags. We use it to verify Hardened Runtime is
 * active when wx_enforced is set.
 *
 * In DEBUG builds we warn but allow startup (locally-built binaries are
 * never signed with Hardened Runtime). In release builds the absence
 * of Hardened Runtime is treated as a fail-closed condition, because
 * the signed gethull.dev release binary always carries it.
 *
 * CS_HARD  (0x0100) — Hardened Runtime in effect (kill invalid)
 * CS_KILL  (0x0200) — kill the process on invalid signature
 */
#define HL_CS_OPS_STATUS    0
#define HL_CS_HARD          0x00000100
#define HL_CS_KILL          0x00000200

extern int csops(int pid, unsigned int ops, void *useraddr, size_t usersize);

static int hl_macos_hardened_runtime_active(void)
{
    uint32_t flags = 0;
    if (csops(0, HL_CS_OPS_STATUS, &flags, sizeof(flags)) != 0)
        return 0;
    return (flags & HL_CS_HARD) ? 1 : 0;
}

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
static int seatbelt_build_profile(const HlSandboxPolicy *policy,
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

    for (int i = 0; i < policy->fs_read_count; i++) {
        snprintf(scratch->fs_read_keys[i],
                 sizeof(scratch->fs_read_keys[i]), "FS_R_%d", i);
        /* Resolve symlinks — Seatbelt matches real paths */
        const char *rpath = policy->fs_read[i];
        if (realpath(rpath, scratch->fs_read_real[i]))
            rpath = scratch->fs_read_real[i];
        PARAM_ADD(scratch->fs_read_keys[i], rpath);
        SBPL_FMT("(allow file-read* (subpath (param \"%s\")))\n",
                  scratch->fs_read_keys[i]);
    }

    /* ── Policy fs_write[] paths ────────────────────────────── */

    for (int i = 0; i < policy->fs_write_count; i++) {
        snprintf(scratch->fs_write_keys[i],
                 sizeof(scratch->fs_write_keys[i]), "FS_W_%d", i);
        /* Resolve manifest fs.write path against app_dir so the
         * seatbelt subpath rule covers the SAME absolute dir the
         * capability layer (hl_cap_blob_init, hl_cap_fs_validate)
         * resolves to. Mismatches here = silent app-init failures
         * (blob.init returns -1 because its target dir isn't
         * actually allowed by the sandbox). Helper also pre-mkdirs
         * so realpath() can canonicalize even for lazily-created
         * directories. */
        const char *wpath = policy->fs_write[i];
        if (sandbox_resolve_manifest_path(app_dir, wpath,
                                           scratch->fs_write_real[i],
                                           sizeof(scratch->fs_write_real[i])) != 0) {
            log_warn("[sandbox] fs.write path '%s' rejected (absolute "
                     "or contains '..') - skipping. Fix the manifest.",
                     wpath);
            continue;
        }
        PARAM_ADD(scratch->fs_write_keys[i], scratch->fs_write_real[i]);
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

    /* ── Hull runtime cache root ($HOME/.hull/blobs/runtime/) ──
     *
     * Runtime-infrastructure caches (Lua bytecode, compute AOT,
     * template AST) live in a shared system-wide blob pool under
     * `blobs/runtime/<kind>/`, NOT in the app's manifest. The
     * sandbox auto-allows them like the CA bundle — apps can't opt
     * out per-app, but the global HULL_NO_CACHE=1 env var disables
     * every consumer at runtime. See docs/blob.md §"Runtime-
     * infrastructure caches".
     *
     * hl_hull_cache_dir() creates the path as a side effect so the
     * subpath rule resolves to a real inode under seatbelt. If
     * $HOME is unset (rare — happens in some CI sandboxes), the
     * call fails and we skip the allow rule — the cache consumers
     * then fail-closed on their own. */
    {
        static char cache_real[SEATBELT_PATH_SIZE];
        char cache_raw[SEATBELT_PATH_SIZE];
        if (hl_hull_cache_dir(cache_raw, sizeof(cache_raw)) == 0) {
            const char *cp = cache_raw;
            if (realpath(cache_raw, cache_real)) cp = cache_real;
            PARAM_ADD("HULL_CACHE", cp);
            SBPL_LIT("; Hull runtime cache (auto-allowed, not in manifest)\n"
                     "(allow file-read* file-write*\n"
                     "    (subpath (param \"HULL_CACHE\")))\n");
        }
    }

    SBPL_LIT("\n");

    /* ── Network ────────────────────────────────────────────── */

    /* Inbound + bind only if the app may accept connections (server
     * mode). CLI mode apps (app.main, no routes) get a strictly
     * tighter profile — no network-inbound, no network-bind. The
     * `system-socket` rule is needed for any socket creation so it
     * stays whenever any network rule is wanted. */
    if (policy->network_inbound) {
        SBPL_LIT("; Network (server socket already bound)\n"
                 "(allow network-inbound network-bind)\n"
                 "(allow system-socket)\n");
    } else if (policy->network_outbound) {
        SBPL_LIT("; Network (outbound only - CLI mode)\n"
                 "(allow system-socket)\n");
    }
    if (policy->network_outbound) {
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

    if (policy->gpu) {
        SBPL_LIT("; GPU compute (Metal)\n"
                 "(allow iokit-open)\n"
                 "(allow mach-lookup (global-name\n"
                 "    \"com.apple.MTLCompilerService\"))\n\n");
    }

    /* ── Terminal UI ─────────────────────────────────────────── */

    if (policy->tui) {
        SBPL_LIT("; Terminal UI (tcsetattr, alt screen, TIOCGWINSZ)\n"
                 "(allow file-read* file-write* (literal \"/dev/tty\"))\n"
                 "(allow file-read* file-write* (regex #\"^/dev/ttys[0-9]+\"))\n"
                 "(allow file-ioctl)\n\n");
    }

    /* ── Signals, sysctl ────────────────────────────────────── */

    SBPL_LIT("; Signals and sysctl\n"
             "(allow signal (target self))\n"
             "(allow sysctl-read)\n\n");

    /* ── Block dangerous operations ─────────────────────────── */

    SBPL_LIT("; Block exec and fork\n"
             "(deny process-exec*)\n"
             "(deny process-fork)\n");

    /* W^X on macOS is enforced by three other layers, not by an SBPL
     * `(deny dynamic-code-generation)` clause:
     *   1. WAMR is built without WASM_ENABLE_JIT / WASM_ENABLE_FAST_JIT
     *      (the _Static_assert in src/hull/cap/wasm.c fails the build
     *      if either is ever turned on). The interpreter and AOT paths
     *      never allocate PROT_EXEC pages from guest code.
     *   2. Lua and QuickJS have no JIT.
     *   3. The signed release binary uses Hardened Runtime and lacks
     *      the `cs.allow-jit` / `cs.allow-unsigned-executable-memory`
     *      entitlements. The kernel refuses any RWX mapping without
     *      them. `hl_macos_hardened_runtime_active()` above fails
     *      closed in `HL_RELEASE_BUILD` if the runtime is missing.
     *
     * We deliberately do NOT add `(deny dynamic-code-generation)`:
     * WAMR uses `MAP_JIT` on macOS/arm64 for non-executable linear
     * memory housekeeping (posix_memmap.c). Denying it crashes the
     * runtime even though no code is being generated. See
     * docs/security.md §4 (macOS) for the full rationale. */

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

/* Whether any of stdin/stdout/stderr is a controlling terminal, probed once
 * in phase 1 BEFORE any pledge is applied. Read by phase 2 (which cannot probe
 * itself: phase-1 pledge is already active there, and isatty()'s ioctl(TCGETS)
 * is not permitted by the `stdio` promise). See the probe site below. */
static int g_stdio_is_tty = 0;

int hl_sandbox_apply_pledge(void)
{
    /* Probe the controlling terminal before ANY sandbox is installed. glibc
     * stdio issues isatty() -> ioctl(TCGETS) on first write to a character
     * device (a real tty) to pick line- vs full-buffering; that ioctl is
     * covered by the `tty` promise, not `stdio`. Without this, a plain CLI
     * app.main that prints to a terminal is killed by the seccomp filter the
     * moment it writes. Probing here, unsandboxed, lets both phases grant
     * `tty` when a terminal is present (seccomp can only restrict, not widen,
     * so phase 1 must be a superset of phase 2). Piped/redirected fds are not
     * terminals, so a server/CI process gets nothing extra. */
    g_stdio_is_tty = (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO) ||
                      isatty(STDERR_FILENO)) ? 1 : 0;

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

    /* Phase 1: permissive — needs to allow whatever phase 2 will allow,
     * since seccomp filters can only restrict on Linux, not widen.
     * On Linux, "netlink" is required so glibc getaddrinfo() can open
     * AF_NETLINK sockets to enumerate interfaces; "unix" is required so
     * nsswitch can talk to systemd-resolved / nscd via AF_UNIX.
     * `fattr` is required because module loading (between phase 1 and
     * phase 2) can hit the bytecode/template cache which calls
     * futimes() via bump_atime_fd for LRU tracking. Without it the
     * JS / Lua runtime gets killed during stdlib module compilation. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
    const char *phase1_base = "stdio inet unix rpath wpath cpath flock fattr dns netlink unveil";
#else
    const char *phase1_base = "stdio inet rpath wpath cpath flock fattr dns unveil";
#endif
    /* Grant `tty` in phase 1 too when a terminal is present, so phase 2's
     * `tty` (for TUI or a terminal-connected CLI) is actually effective -
     * seccomp only restricts, so phase 1 must be a superset. */
    char phase1[256];
    snprintf(phase1, sizeof(phase1), "%s%s",
             phase1_base, g_stdio_is_tty ? " tty" : "");
    if (pledge(phase1, NULL) != 0) {
        log_error("[sandbox] phase 1 pledge failed");
        return -1;
    }

    log_info("[sandbox] phase 1 pledge applied (exec/proc/fork blocked)");
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────── */
/* hl_tool_sandbox_init (build-tool unveil) lives in sandbox_tool.c so the
 * app-runtime sandbox links without the build machinery — see the note in
 * that file. */

int hl_sandbox_apply(const HlSandboxPolicy *policy, const char *app_dir,
                      const char *db_path,
                      const char *ca_bundle_path,
                      const char *tls_cert_path,
                      const char *tls_key_path)
{
    if (!policy)
        return 0;

    /* ── W^X / no-runtime-dynamic-code fail-closed check ───────────
     *
     * When wx_enforced=1 (the default posture set by
     * hl_sandbox_policy_from_manifest), refuse any manifest that
     * opts into JIT / runtime codegen / dlopen, AND refuse to start
     * on platforms without a kernel sandbox.
     *
     * The escape hatch is `--no-sandbox`, which causes main.c to
     * skip this function entirely and log a warning. We do NOT add
     * a second downgrade flag — there is exactly one. */
    if (policy->wx_enforced) {
        if (policy->allow_dynamic_code) {
            log_error("[sandbox] manifest requests allow_dynamic_code=true "
                      "but W^X is enforced - fail-closed. "
                      "Use --no-sandbox to opt out (development only).");
            return -1;
        }
        if (policy->allow_dynamic_libraries) {
            log_error("[sandbox] manifest requests allow_dynamic_libraries=true "
                      "but W^X is enforced - fail-closed.");
            return -1;
        }
        if (!sb_supported()) {
            log_error("[sandbox] W^X enforcement requested but no kernel "
                      "sandbox is available on this platform - fail-closed. "
                      "Use --no-sandbox to opt out (development only).");
            return -1;
        }
    }

    if (!sb_supported()) {
        log_info("[sandbox] kernel sandbox not available on this platform");
        return 0;
    }

#ifdef __APPLE__
    /* ── macOS: Hardened Runtime check (release-only fail-closed) ─
     *
     * The signed gethull.dev release binary always has Hardened
     * Runtime active and is built with -DHL_RELEASE_BUILD. A locally
     * `make`-built binary is unsigned and lacks Hardened Runtime —
     * we degrade to a warning there so dev/test workflows continue
     * to run with the sandbox enabled. CI sets HL_RELEASE_BUILD to
     * make the missing Hardened Runtime fail-closed. */
    if (policy->wx_enforced && !hl_macos_hardened_runtime_active()) {
#ifdef HL_RELEASE_BUILD
        log_error("[sandbox] Hardened Runtime not active and W^X is enforced "
                  "- refusing to start. Release binaries must be code-signed "
                  "with --options=runtime and notarized.");
        return -1;
#else
        log_warn("[sandbox] Hardened Runtime not active - local/dev build. "
                 "Release binaries must be signed with --options=runtime.");
#endif
    }

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

        if (seatbelt_build_profile(policy, resolved_app, resolved_db,
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
                 policy->fs_read_count, policy->fs_write_count,
                 policy->network_outbound ? ", network-outbound" : "");
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

    /* DuckDB's C++ runtime reads these to detect CPU count, memory limits, and
     * timezone data (its ICU extension). Without them its :memory: engine (or a
     * read_csv/read_parquet) throws an internal NULL-deref and aborts (SIGABRT)
     * under the sandbox. All read-only system-info paths. DuckDB reaches here
     * two ways: compiled into the base (HL_ENABLE_DUCKDB), OR composed as a
     * build feature (`hull build --with=duckdb`) into an app whose base sandbox
     * was built WITHOUT that flag. The feature case has no compile-time signal,
     * so detect the linked backend at runtime via hl_db_feature_backends(). */
    int duckdb_present = 0;
#ifdef HL_ENABLE_DUCKDB
    duckdb_present = 1;
#endif
#ifdef HL_ENABLE_DB
    if (!duckdb_present) {
        size_t feat_count = 0;
        (void)hl_db_feature_backends(&feat_count);
        duckdb_present = (feat_count > 0);
    }
#endif
    if (duckdb_present) {
        unveil("/etc/localtime",                    "r");
        unveil("/sys/devices/system/cpu",           "r");
        unveil("/sys/fs/cgroup",                    "r");   /* cgroup v2 memory.max */
        unveil("/proc/self/cgroup",                 "r");
        unveil("/proc/sys/vm/overcommit_memory",    "r");
        unveil("/proc/meminfo",                     "r");
    }

    /* Manifest fs.read / fs.write paths: resolve relative to
     * app_dir (matching the capability layer) AND pre-mkdir so
     * unveil() can canonicalize. Without app_dir-relative
     * resolution, unveil sees "data/" and fails with ENOENT (the
     * polyfill resolves relative to cwd, which often isn't
     * app_dir under e.g. CI / hull build). */
    for (int i = 0; i < policy->fs_read_count; i++) {
        char abs[SANDBOX_PATH_MAX];
        if (sandbox_resolve_manifest_path(app_dir, policy->fs_read[i],
                                           abs, sizeof(abs)) != 0 ||
            unveil(abs, "r") != 0) {
            log_warn("[sandbox] unveil failed for read path: %s",
                     policy->fs_read[i]);
        }
    }

    for (int i = 0; i < policy->fs_write_count; i++) {
        char abs[SANDBOX_PATH_MAX];
        if (sandbox_resolve_manifest_path(app_dir, policy->fs_write[i],
                                           abs, sizeof(abs)) != 0 ||
            unveil(abs, "rwc") != 0) {
            log_warn("[sandbox] unveil failed for write path: %s",
                     policy->fs_write[i]);
        }
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

    /* Hull runtime cache root ($HOME/.hull/cache/) — see SBPL
     * counterpart above. Auto-allowed read/write/create so the
     * bytecode/AOT/template caches work without polluting the app
     * manifest. */
    {
        char cache_path[PATH_MAX];
        if (hl_hull_cache_dir(cache_path, sizeof(cache_path)) == 0) {
            if (unveil(cache_path, "rwc") != 0)
                log_warn("[sandbox] unveil failed for Hull cache: %s",
                         cache_path);
        }
    }

    /* DNS resolution: when manifest declares outbound hosts, glibc's
     * getaddrinfo() needs to read these files. The "dns" pledge promise
     * allows the syscalls; unveil controls which paths they may touch.
     * Errors are non-fatal — the file may not exist on minimal systems. */
    if (policy->network_outbound) {
        unveil("/etc/resolv.conf",   "r");
        unveil("/etc/hosts",         "r");
        unveil("/etc/nsswitch.conf", "r");
        unveil("/etc/services",      "r");
        /* systemd-resolved IPC socket (modern Linux) and nscd socket */
        unveil("/run/systemd/resolve", "r");
        unveil("/var/run/nscd",        "r");
        /* /etc/ssl/certs is needed when CA bundle uses the system path
         * (mbedTLS doesn't read it directly, but glibc nss modules may
         * touch /etc files generally — give the whole /etc readonly to
         * cover gai_conf, hosts.d, etc.). */
        unveil("/etc",               "r");
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
    if (policy->gpu) {
        if (policy->gpu_device_count > 0) {
            /* Unveil only specific render nodes for allowed devices */
            for (int i = 0; i < policy->gpu_device_count; i++) {
                char node[64];
                snprintf(node, sizeof(node), "/dev/dri/renderD%d",
                         128 + policy->gpu_devices[i]);
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

    /* Terminal UI — unveil the controlling tty so tcsetattr /
     * TIOCGWINSZ keep working after the filesystem seal. */
    if (policy->tui) {
        if (unveil("/dev/tty", "rw") != 0)
            log_warn("[sandbox] unveil failed for /dev/tty");
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
     *   rpath  — SQLite reads, app file reads
     *   wpath  — SQLite WAL writes
     *   cpath  — SQLite journal/WAL creation
     *   flock  — SQLite locking
     *   fattr  — futimes() on owned blob-cache fds (LRU atime touch
     *            in hl_blob_store_reader_open when track_access=1).
     *            Glibc since 2.6.22 implements futimes() as utimensat(),
     *            which pledge categorises under fattr. Scope is narrow:
     *            owned fds we just opened from $HOME/.hull/blobs/runtime/,
     *            unveil prevents any other path from being touched. The
     *            bytecode and template caches need this to drive their
     *            LRU eviction policy.
     *
     * Conditional:
     *   inet   — socket-family syscalls. Needed when the app serves
     *            HTTP (network_inbound) OR makes outbound requests
     *            (network_outbound). CLI-mode apps with neither drop
     *            it entirely; their kernel-visible syscall surface
     *            shrinks accordingly.
     *   dns    — outbound HTTP name resolution (when hosts declared)
     */
    char promises[256];
    int plen = snprintf(promises, sizeof(promises),
                        "stdio rpath wpath cpath flock fattr");
    if (plen > 0 && (size_t)plen < sizeof(promises) &&
        (policy->network_inbound || policy->network_outbound)) {
        int n = snprintf(promises + plen, sizeof(promises) - (size_t)plen,
                         " inet");
        if (n > 0) plen += n;
    }
    /* `tty` covers tcsetattr / tcgetattr / TIOCGWINSZ / ioctl(TCGETS) on the
     * controlling terminal. Required for TUI's raw-mode toggle and size
     * queries, AND for the isatty()/buffering-probe ioctl glibc stdio runs on
     * first write to a terminal-connected fd (so a plain CLI app.main that
     * prints to a terminal is not killed). Granted when the app is a TUI OR a
     * terminal is present (g_stdio_is_tty, probed unsandboxed in phase 1 -
     * we must NOT call isatty() here, as phase-1 pledge already forbids that
     * ioctl). Phase 1 grants the matching superset. Piped/redirected fds are
     * not terminals, so a server/CI process's promise set is unchanged. */
    if (plen > 0 && (policy->tui || g_stdio_is_tty) &&
        (size_t)plen < sizeof(promises)) {
        int n = snprintf(promises + plen, sizeof(promises) - (size_t)plen,
                         " tty");
        if (n > 0) plen += n;
    }
    /* When the app declares outbound hosts, allow DNS resolution.
     *   dns      — UDP/IP queries to the resolver
     *   netlink  — Linux/glibc getaddrinfo() opens AF_NETLINK to enumerate
     *              interfaces. Cosmo's pledge() rejects the unknown promise
     *              entirely; OpenBSD's pledge() does not have it either
     *              (its getaddrinfo reads /etc/resolv.conf directly).
     */
    if (plen > 0 && policy->network_outbound &&
        (size_t)plen < sizeof(promises)) {
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
        /* Linux glibc DNS needs: dns (UDP queries), netlink (interface
         * enumeration), unix (nsswitch IPC with systemd-resolved/nscd). */
        const char *dns_promises = " dns netlink unix";
#else
        const char *dns_promises = " dns";
#endif
        size_t remaining = sizeof(promises) - (size_t)plen;
        size_t dns_len = strlen(dns_promises);
        if (dns_len + 1 > remaining) {
            /* Should never happen — promises[256] vs ~50 bytes total —
             * but fail loud rather than silently drop dns permissions. */
            log_error("[sandbox] pledge promises buffer overflow "
                      "(need %zu, have %zu) - refusing to start with "
                      "an under-permissioned sandbox", dns_len + 1, remaining);
            return -1;
        }
        int dlen = snprintf(promises + plen, remaining, "%s", dns_promises);
        if (dlen < 0 || (size_t)dlen >= remaining) {
            log_error("[sandbox] pledge promise concat failed");
            return -1;
        }
    }

    if (pledge(promises, NULL) != 0) {
        log_error("[sandbox] pledge failed");
        return -1;
    }

    log_info("[sandbox] applied (unveil: %d read, %d write; pledge: %s)",
             policy->fs_read_count, policy->fs_write_count, promises);

    return 0;
}

/* ── Policy builder ────────────────────────────────────────────────── */

/* True if a DSN names a NETWORK database backend (dials out), vs a local
 * sqlite/duckdb file. A "$VAR"/"${VAR}" env-ref has an unknown scheme at
 * manifest time, so it's treated as possibly-network (the common
 * `$DATABASE_URL` = postgres case must not be blocked). A scheme-less bare
 * path is SQLite (local). */
static int sandbox_dsn_is_network(const char *dsn)
{
    if (!dsn || !*dsn) return 0;
    if (dsn[0] == '$') return 1;                 /* env-ref: assume network */
    /* Case-insensitive to match the DSN backend selector (db_select.c) and
     * serve.c's db_dsn_is_network — else an uppercase-scheme named DSN would
     * be classified non-network and the app SIGKILLed on connect. */
    return strncasecmp(dsn, "postgres://",   11) == 0
        || strncasecmp(dsn, "postgresql://", 13) == 0
        || strncasecmp(dsn, "mysql://",       8) == 0
        || strncasecmp(dsn, "mariadb://",    10) == 0
        || strncasecmp(dsn, "redis://",       8) == 0
        || strncasecmp(dsn, "rediss://",      9) == 0
        || strncasecmp(dsn, "valkey://",      9) == 0
        || strncasecmp(dsn, "valkeys://",    10) == 0;
}

/* A KV dynamic scheme that dials the network (all of them: Valkey/Redis are
 * always networked). */
static int kv_scheme_is_network(const char *s)
{
    return s && (strcmp(s, "redis") == 0 || strcmp(s, "rediss") == 0
              || strcmp(s, "valkey") == 0 || strcmp(s, "valkeys") == 0);
}

/* True if the manifest declares any database connection that may dial the
 * network: a named connection with a network (or env-ref) DSN, or a
 * databases.dynamic policy admitting a network scheme. Grants network_outbound
 * for a DB-only app that has no http `hosts` — without it the kernel sandbox
 * blocks the connect (pledge SIGKILL on Linux). The `-d` default DSN is not in
 * the manifest (it lives in cfg); serve.c ORs that in separately. */
static int manifest_has_network_db(const HlManifest *m)
{
    if (!m) return 0;
    for (int i = 0; i < m->databases.named_count; i++)
        if (sandbox_dsn_is_network(m->databases.named[i].dsn)) return 1;
    if (m->databases.dynamic.declared) {
        for (int i = 0; i < m->databases.dynamic.scheme_count; i++) {
            const char *s = m->databases.dynamic.schemes[i];
            if (s && (strcmp(s, "postgres") == 0 || strcmp(s, "postgresql") == 0
                   || strcmp(s, "mysql") == 0 || strcmp(s, "mariadb") == 0))
                return 1;
        }
    }
    /* A kv.dynamic policy admitting a Valkey/Redis scheme also dials out, so it
     * needs network_outbound exactly like a network DB (kv.open would otherwise
     * be SIGKILLed on connect). */
    if (m->kv.dynamic.declared) {
        for (int i = 0; i < m->kv.dynamic.scheme_count; i++)
            if (kv_scheme_is_network(m->kv.dynamic.schemes[i])) return 1;
    }
    return 0;
}

void hl_sandbox_policy_from_manifest(HlSandboxPolicy *policy,
                                     const HlManifest *manifest)
{
    if (!policy)
        return;

    /* Default-deny: everything starts at zero/NULL. */
    *policy = (HlSandboxPolicy){0};

    /* W^X is the platform's default posture — independent of manifest.
     * Only `--no-sandbox` disables it (by skipping hl_sandbox_apply
     * entirely in main.c). */
    policy->wx_enforced = 1;

    if (!manifest)
        return;

    /* Borrow array pointers — manifest must outlive the policy. */
    policy->fs_read         = manifest->fs_read;
    policy->fs_read_count   = manifest->fs_read_count;
    policy->fs_write        = manifest->fs_write;
    policy->fs_write_count  = manifest->fs_write_count;

    /* Sandbox only needs the boolean "any outbound network?" decision. The
     * actual host allowlist is enforced in cap/http.c (http.fetch) and, for
     * DB, by the backend + databases.dynamic policy. Granted for http `hosts`
     * OR a declared network database connection, so a DB-only app can reach
     * its database (previously blocked -> pledge SIGKILL on connect). */
    policy->network_outbound = (manifest->hosts_count > 0)
                             || manifest_has_network_db(manifest);

    /* Inbound: assume the app may serve (default-permissive). serve.c
     * narrows this to 0 right before applying the sandbox when the
     * loaded app turned out to be CLI mode (app.main, no routes). */
    policy->network_inbound = 1;

    policy->gpu              = manifest->gpu;
    policy->gpu_devices      = manifest->gpu_devices;
    policy->gpu_device_count = manifest->gpu_device_count;

    policy->tui              = manifest->tui;

    /* Mirror manifest-declared dynamic-code intent. Defaults are 0
     * (deny). `hl_sandbox_apply` enforces the W^X conflict check. */
    policy->allow_dynamic_code      = manifest->allow_dynamic_code;
    policy->allow_dynamic_libraries = manifest->allow_dynamic_libraries;
}
