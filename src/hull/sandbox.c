/*
 * sandbox.c - Kernel-level sandbox enforcement
 *
 * Applies OS-level restrictions derived from the application manifest.
 * After hl_sandbox_apply(), the process can only access the filesystem
 * paths and syscall families that the manifest declares.
 *
 * Platform implementations:
 *   OpenBSD       - native pledge() + unveil() (originated here)
 *   Cosmopolitan  - pledge() + unveil() built into cosmo libc
 *   Linux 5.13+   - jart/pledge polyfill (seccomp-bpf + landlock)
 *   macOS         - Seatbelt (sandbox_init_with_parameters)
 *   other         - no-op (C-level cap validation is the defense)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/sandbox.h"
#include "hull/shared/cache_dir.h"
#include "hull/shared/host.h"   /* hl_host_path_too_broad */
#include "hull/limits/gpu.h"     /* HL_GPU_MAX_DEVICES */
#include "log.h"
#ifdef HL_ENABLE_DB
#include "hull/cap/db_backend.h"   /* hl_db_backend_select: detect a composed DuckDB feature */
#endif

#include <ctype.h>     /* isalpha / isalnum (DSN scheme syntax) */
#include <errno.h>
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
 * hl_sandbox_resolve_grant; the Apple-side SeatbeltScratch
 * has its own equivalent (SEATBELT_PATH_SIZE) for SBPL-time
 * scratch, kept separate so its name still telegraphs context. */
#define SANDBOX_PATH_MAX  4096
_Static_assert(SANDBOX_PATH_MAX >= PATH_MAX,
               "SANDBOX_PATH_MAX must be >= PATH_MAX");

#ifdef HL_ENABLE_DB
/* SQLite's private temp directory (hl_hull_sqlite_temp_dir), granted only when
 * this binary has a SQLite backend at all - in the base or composed as a
 * feature (audit 12). An app built without one never opens SQLite, so it
 * neither makes the directory nor gets a writable path it has no use for. */
static const char *sb_sqlite_temp_dir(void)
{
    if (!hl_db_backend_select("sqlite://", NULL)) return NULL;
    return hl_hull_sqlite_temp_dir();
}
#endif

/* Resolve a manifest fs.read / fs.write path against the app's
 * root directory. The manifest declares paths like "data/" or
 * "uploads/", which the capability layer interprets relative to
 * app_dir (HlFsConfig.base_dir). The sandbox MUST match that
 * resolution - otherwise the seatbelt subpath rule (or unveil
 * subtree) covers a different directory than the one the app
 * actually writes to. Steps:
 *
 *   1. Validate shape: reject absolute paths and any segment
 *      containing "..". Defense-in-depth - manifest parser only
 *      truncates.
 *   2. Concatenate app_dir + "/" + relpath into a temp buffer.
 *   3. Pick what the kernel grant covers from the grant's shape: a
 *      glob's literal directory; a "dir/" grant (created if absent,
 *      never through a symlink, so a lazily made blob root
 *      canonicalizes); for a file WRITE grant its parent directory
 *      (always - see below) unless that parent is app_dir itself;
 *      else the path itself, or its parent when it does not exist yet.
 *      No component may be a symlink.
 *   4. realpath() the result into out_abs.
 *
 * Returns 0 on success (out_abs filled), -1 on rejection or
 * buffer overflow. On -1 the caller should log + skip - never
 * fall back to an unresolved relative path, since unveil() and
 * seatbelt both reject those.
 *
 * Platform-agnostic - both the seatbelt SBPL build (macOS) and
 * the unveil setup (Linux / OpenBSD / Cosmopolitan) call this.
 */
int hl_sandbox_resolve_grant(const char *app_dir, const char *relpath,
                             char *out_abs, size_t out_cap, int for_write)
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
    while (adir_len > 1 && app_dir[adir_len - 1] == '/') adir_len--;

    /* Spell the grant the way the capability layer reads it
     * (hl_fs_grant_parse): "." and empty components dropped, a trailing
     * slash kept. Every decision below is made on the joined string, so
     * "./out.txt" (joined "app/./out.txt") was not seen as top-level: its
     * parent "app/." was granted, and realpath made that app_dir itself -
     * the whole app directory kernel-writable, with no warning, for a grant
     * the cap layer compiles to the one file "out.txt". */
    char rel[SANDBOX_PATH_MAX];
    size_t rel_len = 0;
    for (const char *p = relpath; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *c = p;
        while (*p && *p != '/') p++;
        size_t cl = (size_t)(p - c);
        if (cl == 1 && c[0] == '.') continue;
        if (rel_len + (rel_len ? 1 : 0) + cl + 2 > sizeof(rel)) return -1;
        if (rel_len) rel[rel_len++] = '/';
        memcpy(rel + rel_len, c, cl);
        rel_len += cl;
    }
    if (rel_len == 0) {
        /* "." / "./": the cap layer's base-root grant, the app directory and
         * everything under it - asked for in so many words. */
        if (realpath(app_dir, out_abs) == NULL) {
            if (adir_len + 1 > out_cap) return -1;
            memcpy(out_abs, app_dir, adir_len);
            out_abs[adir_len] = '\0';
        }
        return 0;
    }
    if (relpath[strlen(relpath) - 1] == '/') rel[rel_len++] = '/';
    rel[rel_len] = '\0';
    if (adir_len + 1 + rel_len + 1 > out_cap) return -1;

    char joined[SANDBOX_PATH_MAX];
    if (adir_len + 1 + rel_len + 1 > sizeof(joined)) return -1;
    memcpy(joined, app_dir, adir_len);
    joined[adir_len] = '/';
    memcpy(joined + adir_len + 1, rel, rel_len + 1);

    /* What the kernel grant covers depends on the grant's shape (the
     * capability layer keeps the precise rule; this is the outer bound):
     *   - a glob ("data/*.csv"): the literal directory before the first
     *     pattern component - it was mkdir'd and unveiled as a literal
     *     "*.csv" directory, so the kernel denied the real files;
     *   - "dir/" (trailing slash): that directory, created if absent (a
     *     lazily created blob / upload root);
     *   - anything else: the path itself when it exists, else its parent -
     *     a file grant ("out.txt") was created as a DIRECTORY, so the
     *     write then failed with EISDIR.
     * Creation never walks through a symlink: mkdir followed one out of
     * app_dir. */
    size_t jlen = strlen(joined);
    size_t lit = jlen;                        /* end of the literal prefix */
    {
        size_t comp_start = adir_len + 1;
        for (size_t k = adir_len + 1; k <= jlen; k++) {
            if (joined[k] == '/' || joined[k] == '\0') {
                if (memchr(joined + comp_start, '*', k - comp_start) ||
                    memchr(joined + comp_start, '?', k - comp_start) ||
                    memchr(joined + comp_start, '[', k - comp_start)) {
                    lit = comp_start > 0 ? comp_start - 1 : 0;   /* drop "/pat..." */
                    break;
                }
                comp_start = k + 1;
            }
        }
    }
    int is_glob = lit < jlen;
    int is_dir = !is_glob && jlen > 0 && joined[jlen - 1] == '/';
    /* A top-level write glob ("*.log") has no literal directory but the app
     * directory itself, so the kernel grant is the whole of it - app.lua,
     * migrations/, package.sig - for every run. Said, as for a top-level
     * file that does not exist yet (audit 9); the cap layer still limits
     * the app to the pattern. */
    if (is_glob && for_write && lit <= adir_len)
        log_warn("[sandbox] fs.write '%s' is a pattern directly in the app "
                 "directory: the whole app directory is writable at the "
                 "kernel level. Put written files in a subdirectory "
                 "(\"logs/*.log\") to avoid this.", relpath);
    /* Zeroed: only buf[0..lit] is written below, and the symlink walk reads
     * up to strlen(buf), which the analyzer cannot tie to that prefix. */
    char buf[SANDBOX_PATH_MAX] = { 0 };
    if (lit >= sizeof(buf)) return -1;
    memcpy(buf, joined, lit);
    buf[lit] = '\0';
    while (lit > 1 && buf[lit - 1] == '/') buf[--lit] = '\0';

    if (is_dir) {
        for (size_t k = adir_len + 1; k <= lit; k++) {
            if (buf[k] == '/' || buf[k] == '\0') {
                char saved = buf[k];
                buf[k] = '\0';
                struct stat st;
                if (lstat(buf, &st) == 0) {
                    if (S_ISLNK(st.st_mode)) { buf[k] = saved; return -1; }
                } else {
                    (void)mkdir(buf, 0755);
                }
                buf[k] = saved;
            }
        }
    } else if (!is_glob) {
        /* A file WRITE grant in a subdirectory is its parent directory,
         * whether or not the file exists yet. It used to be the parent only
         * while the file was absent: once it existed (every restart after
         * the first write) the grant became the file alone, and on Landlock
         * the cap layer's temp-beside-then-rename write lost MAKE_REG on the
         * parent - and the in-place O_TRUNC fallback lacked TRUNCATE - so
         * the grant stopped working. The capability layer keeps the exact
         * rule.
         *
         * Not when that parent is app_dir itself: then a grant of one file
         * ("out.txt") made the whole app directory - app.lua, migrations/,
         * package.sig - kernel-writable for the life of the process. Such a
         * file, once it exists, is granted alone: the temp beside it is
         * refused, and the cap layer writes it in place (O_TRUNC, which 'w'
         * covers). Before it exists there is nothing to grant but app_dir,
         * so this first run is warned about. A read grant is the file
         * itself when it exists.
         *
         * An existing DIRECTORY named without the trailing slash ("data") is
         * what the cap layer compiles it to: that directory's subtree. It
         * used to fall to the parent too - app_dir for a top-level one,
         * under a warning about a file. */
        struct stat st;
        int exists = lstat(buf, &st) == 0;
        char *sl = strrchr(buf, '/');
        if (!sl || (size_t)(sl - buf) < adir_len) return -1;
        int top_level = (size_t)(sl - buf) == adir_len;
        if (exists && S_ISDIR(st.st_mode)) {
            /* the directory itself */
        } else if (for_write && top_level && exists && S_ISREG(st.st_mode)) {
            /* the file alone */
        } else if (for_write || !exists) {        /* the parent */
            if (for_write && top_level)
                log_warn("[sandbox] fs.write '%s' is not an existing file "
                         "or directory: the whole app directory is writable "
                         "at the kernel level for this run (once it exists, "
                         "the file alone). Put written files in a "
                         "subdirectory to avoid this.", relpath);
            *sl = '\0';
        }
    }

    /* Never through a symlink, in any shape: a glob's or a file's directory
     * that is a link (an in-tree "data -> /" with fs.read={"data/x.csv"})
     * was unveiled - followed - as its target. Only "dir/" checked. */
    size_t blen = strlen(buf);
    for (size_t k = adir_len + 1; k <= blen; k++) {
        if (buf[k] == '/' || buf[k] == '\0') {
            char saved = buf[k];
            buf[k] = '\0';
            struct stat lst;
            int is_link = lstat(buf, &lst) == 0 && S_ISLNK(lst.st_mode);
            buf[k] = saved;
            if (is_link) return -1;
            if (saved == '\0') break;
        }
    }

    if (realpath(buf, out_abs) == NULL) {
        size_t need = strlen(buf) + 1;
        if (need > out_cap) return -1;
        memcpy(out_abs, buf, need);
    }
    return 0;
}

/* ── Platform pledge/unveil providers ──────────────────────────────── */

#if defined(__COSMOPOLITAN__)

#include <cosmo.h>   /* IsLinux() / IsOpenbsd() - see sb_supported() below */

/* Cosmopolitan libc provides pledge() and unveil() natively. */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);

/* ...but they only ENFORCE where the host gives them something to enforce
 * with: seccomp-bpf + Landlock on Linux, the native syscalls on OpenBSD.
 * Everywhere else - Windows above all, the platform this build exists to
 * reach - both calls return 0 and do nothing.
 *
 * Returning 1 unconditionally therefore made Hull ANNOUNCE a sandbox it did
 * not have. Measured on Windows 11 with cosmocc 4.0.2: pledge("stdio", NULL)
 * returns 0, and a socket(AF_INET, SOCK_STREAM, 0) immediately afterwards
 * SUCCEEDS - `stdio` grants no `inet`, so under a pledge that is enforcing,
 * that call is denied. The startup log nonetheless read
 *
 *   [sandbox] phase 1 pledge applied (exec/proc/fork blocked)
 *   [sandbox] applied (unveil: 0 read, 0 write; pledge: stdio ... inet)
 *
 * which a reader can only take as "this process is confined". It is not. The
 * same is true of a cosmo APE on macOS or a BSD: the cosmo branch is selected
 * before the __APPLE__ one, so a Seatbelt profile is never reached there
 * either.
 *
 * Two things follow from getting this right, beyond the log being true. The
 * "kernel sandbox not available on this platform" path - which already exists
 * for exactly this case - becomes the one taken; and a manifest asking for W^X
 * enforcement FAILS CLOSED there, instead of being accepted with nothing
 * behind it. The C capability layer is untouched and remains the enforcement
 * boundary on such hosts, as it is on any platform without a kernel backend.
 */
static int sb_supported(void) { return IsLinux() || IsOpenbsd(); }

#elif defined(__OpenBSD__)

/*
 * OpenBSD: pledge() and unveil() are native system calls.
 * This is where these APIs originated - declared in <unistd.h>.
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
/* vendor/pledge HULL PATCH 0004 (docs/pledge_patches.md): how the Landlock
 * domain reached the threads that already existed at unveil(NULL, NULL), and
 * how many it could not reach. */
extern int hull_unveil_unsynced_threads;
extern int hull_unveil_sync_mode;
extern int landlock_create_ruleset(const void *attr, size_t size,
                                    unsigned int flags);

/* LANDLOCK_CREATE_RULESET_VERSION has been stable since Landlock's first ABI.
 * Declare the value locally so Hull still builds with old libc kernel headers. */
#define HL_LANDLOCK_CREATE_RULESET_VERSION 1u

static int linux_landlock_abi(void)
{
    static int probed;
    static int abi;
    if (!probed) {
        int saved_errno = errno;
        abi = landlock_create_ruleset(NULL, 0,
                                      HL_LANDLOCK_CREATE_RULESET_VERSION);
        errno = saved_errno;
        probed = 1;
    }
    return abi;
}

static int sb_supported(void) { return 1; }

#elif defined(__APPLE__)

/*
 * macOS: Seatbelt sandbox via sandbox_init_with_parameters().
 * This is a private API (not in public SDK headers) but is exported
 * by libsystem_sandbox.dylib and used by Chrome, Firefox, and macOS
 * system services.  Declared manually via extern.
 *
 * sandbox_init is irreversible - once applied, cannot be modified.
 * Phase 1 (pledge) is a no-op on macOS; phase 2 applies the full
 * Seatbelt profile.
 */
/* sandbox.h is deprecated since macOS 10.8 - use free() for error strings.
 * stdlib.h is included at the top of the file. */

extern int sandbox_init_with_parameters(const char *profile,
                                         uint64_t flags,
                                         const char **parameters,
                                         char **errorbuf);

/* pledge/unveil remain as no-ops - used by tool mode and phase 1 */
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
 * CS_HARD  (0x0100) - Hardened Runtime in effect (kill invalid)
 * CS_KILL  (0x0200) - kill the process on invalid signature
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

/* realpath(3) writes up to PATH_MAX bytes - assert our buffer is large enough */
_Static_assert(SEATBELT_PATH_SIZE >= PATH_MAX,
               "SEATBELT_PATH_SIZE must be >= PATH_MAX");

/* @p path with its parent directory canonicalised (the leaf may not exist
 * yet): Seatbelt matches real paths (/var -> /private/var). */
static int sb_canonical_file(const char *path, char *out, size_t outsz)
{
    char dir[SEATBELT_PATH_SIZE], rdir[SEATBELT_PATH_SIZE];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return -1;
    char *sl = strrchr(dir, '/');
    if (!sl) return -1;
    const char *leaf = path + (sl - dir) + 1;
    if (sl == dir) sl[1] = '\0'; else *sl = '\0';
    if (!realpath(dir, rdir)) return -1;
    int n = snprintf(out, outsz, "%s/%s", strcmp(rdir, "/") == 0 ? "" : rdir, leaf);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* Scratch buffers for derived paths (stack-allocated in caller) */
typedef struct {
    char db_dir[SEATBELT_PATH_SIZE];
    char db_file[4][SEATBELT_PATH_SIZE];   /* db, -wal, -shm, -journal */
    char duck_file[4][SEATBELT_PATH_SIZE]; /* .wal, .wal.checkpoint,
                                              .wal.recovery, .tmp (audit 43) */
    char fs_read_keys[HL_MANIFEST_MAX_PATHS][16];    /* "FS_R_0" .. "FS_R_31" */
    char fs_write_keys[HL_MANIFEST_MAX_PATHS][16];   /* "FS_W_0" .. "FS_W_31" */
    char fs_read_real[HL_MANIFEST_MAX_PATHS][SEATBELT_PATH_SIZE];
    char fs_write_real[HL_MANIFEST_MAX_PATHS][SEATBELT_PATH_SIZE];
    char app_hull[SEATBELT_PATH_SIZE];     /* <app_dir>/.hull (audit 37) */
    char int_file[4][SEATBELT_PATH_SIZE];  /* internal db, -wal, -shm, -journal */
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
                                   const char *tls_cert_path,
                                   const char *tls_key_path,
                                   const char *const *ttys,
                                   int tty_count,
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

    /* stat()/lstat() on any path - kernel needs this to traverse parent
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
        /* The database and its WAL / SHM / journal siblings, by absolute
         * path, plus a read of the directory itself (SQLite fsyncs it). The
         * whole parent directory used to be writable: with -d
         * app_dir/data.db that was the entire app tree, and a relative path
         * whose file did not exist yet collapsed the grant to ".". */
        char abs[SEATBELT_PATH_SIZE];
        if (!realpath(db_path, abs)) {
            char dir[SEATBELT_PATH_SIZE];
            snprintf(dir, sizeof dir, "%s", db_path);
            char *sl = strrchr(dir, '/');
            const char *base = sl ? sl + 1 : db_path;
            if (sl) { if (sl == dir) sl[1] = '\0'; else *sl = '\0'; }
            char rdir[SEATBELT_PATH_SIZE];
            if (!realpath(sl ? dir : ".", rdir) ||
                (size_t)snprintf(abs, sizeof abs, "%s/%s",
                                 strcmp(rdir, "/") == 0 ? "" : rdir, base) >= sizeof abs)
                snprintf(abs, sizeof abs, "%s", db_path);
        }
        snprintf(scratch->db_dir, sizeof(scratch->db_dir), "%s", abs);
        char *slash = strrchr(scratch->db_dir, '/');
        if (slash && slash != scratch->db_dir) *slash = '\0';
        else snprintf(scratch->db_dir, sizeof(scratch->db_dir), "/");
        static const char *const suffix[4] = { "", "-wal", "-shm", "-journal" };
        static const char *const key[4] = { "DB_F0", "DB_F1", "DB_F2", "DB_F3" };
        for (int k = 0; k < 4; k++) {
            snprintf(scratch->db_file[k], sizeof(scratch->db_file[k]), "%s%s",
                     abs, suffix[k]);
            PARAM_ADD(key[k], scratch->db_file[k]);
        }
        PARAM_ADD("DB_DIR", scratch->db_dir);

        SBPL_LIT("; SQLite database: the file, its WAL / SHM / journal, and a\n"
                 "; read of its directory\n"
                 "(allow file-read* file-write*\n"
                 "    (literal (param \"DB_F0\")) (literal (param \"DB_F1\"))\n"
                 "    (literal (param \"DB_F2\")) (literal (param \"DB_F3\")))\n"
                 "(allow file-read* (literal (param \"DB_DIR\")))\n\n");

        /* A DuckDB database's own sidecars (audit 43): its .wal - unlinked
         * at every checkpoint and made again by the next commit -, the
         * .wal.checkpoint / .wal.recovery it makes on demand, and the .tmp/
         * spill directory. Seatbelt matches paths, so each is granted by
         * name, created or not, and the directory stays unwritable. */
        if (policy->db_duckdb) {
            static const char *const dsuf[4] = {
                ".wal", ".wal.checkpoint", ".wal.recovery", ".tmp"
            };
            static const char *const dkey[4] = {
                "DUCK_F0", "DUCK_F1", "DUCK_F2", "DUCK_TMP"
            };
            for (int k = 0; k < 4; k++) {
                snprintf(scratch->duck_file[k], sizeof(scratch->duck_file[k]),
                         "%s%s", abs, dsuf[k]);
                PARAM_ADD(dkey[k], scratch->duck_file[k]);
            }
            SBPL_LIT("; DuckDB database: its .wal, .wal.checkpoint, "
                     ".wal.recovery and .tmp/\n"
                     "(allow file-read* file-write*\n"
                     "    (literal (param \"DUCK_F0\")) (literal (param \"DUCK_F1\"))\n"
                     "    (literal (param \"DUCK_F2\")) (subpath (param \"DUCK_TMP\")))\n\n");
        }
    }

    /* ── Manifest fs_read[] paths ───────────────────────────── */

    for (int i = 0; i < policy->fs_read_count; i++) {
        snprintf(scratch->fs_read_keys[i],
                 sizeof(scratch->fs_read_keys[i]), "FS_R_%d", i);
        /* Against app_dir, as fs.write is: realpath() alone resolved a
         * relative grant against the cwd - `hull /srv/app` run from / with
         * fs.read={"etc"} gave Seatbelt read access to /etc. */
        if (hl_sandbox_resolve_grant(app_dir, policy->fs_read[i],
                                      scratch->fs_read_real[i],
                                      sizeof(scratch->fs_read_real[i]), 0) != 0) {
            log_warn("[sandbox] fs.read path '%s' rejected (absolute "
                     "or contains '..') - skipping. Fix the manifest.",
                     policy->fs_read[i]);
            continue;
        }
        const char *rpath = scratch->fs_read_real[i];
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
        if (hl_sandbox_resolve_grant(app_dir, wpath,
                                      scratch->fs_write_real[i],
                                      sizeof(scratch->fs_write_real[i]), 1) != 0) {
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

    /* ── TLS cert, TLS key ───────────────────────────────────── */

    /* No CA bundle grant (audit 29): every anchor is read into memory
     * before this profile applies (ca_trust.c), and the path the ladder
     * reported was sometimes a label ("(embedded)") that resolved against
     * the working directory - a planted symlink of that name widened the
     * read grant to whatever it pointed at. */
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
     * sandbox auto-allows them like the CA bundle - apps can't opt
     * out per-app, but the global HULL_NO_CACHE=1 env var disables
     * every consumer at runtime. See docs/blob.md §"Runtime-
     * infrastructure caches".
     *
     * hl_hull_cache_dir() creates the path as a side effect so the
     * subpath rule resolves to a real inode under seatbelt. If
     * $HOME is unset (rare - happens in some CI sandboxes), the
     * call fails and we skip the allow rule - the cache consumers
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

#ifdef HL_ENABLE_DB
    /* SQLite temp files (temp_store=FILE: sorter spills, temp b-trees,
     * VACUUM's copy) go to Hull's private, already-canonical temp dir. */
    {
        const char *tmp = sb_sqlite_temp_dir();
        if (tmp) {
            PARAM_ADD("SQLITE_TMP", tmp);
            SBPL_LIT("; SQLite temp files (auto-allowed, not in manifest)\n"
                     "(allow file-read* file-write*\n"
                     "    (subpath (param \"SQLITE_TMP\")))\n");
        }
    }
#endif

    SBPL_LIT("\n");

    /* ── Network ────────────────────────────────────────────── */

    /* Inbound + bind only if the app may accept connections (server
     * mode). CLI mode apps (app.main, no routes) get a strictly
     * tighter profile - no network-inbound, no network-bind. The
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

    /* /dev/tty plus the process's OWN terminal devices only, each by
     * literal path passed as a parameter (audit 25): a /dev/ttys[0-9]+
     * regex and a bare (allow file-ioctl) let a TUI app open, read, write
     * and ioctl (TIOCSTI) any pty the user owns - another Terminal tab.
     * Linux / OpenBSD likewise grant /dev/tty alone. */
    if (policy->tui) {
        static const char *const tty_keys[HL_SANDBOX_TTY_MAX] = {
            "TTY_0", "TTY_1", "TTY_2"
        };
        char tty_rules[256];
        size_t tl = 0;
        tty_rules[0] = '\0';
        for (int i = 0; i < tty_count && i < HL_SANDBOX_TTY_MAX; i++) {
            if (!ttys || !hl_sandbox_tty_path_ok(ttys[i]))
                continue;
            PARAM_ADD(tty_keys[i], ttys[i]);
            int n = snprintf(tty_rules + tl, sizeof tty_rules - tl,
                             " (literal (param \"%s\"))", tty_keys[i]);
            if (n < 0 || (size_t)n >= sizeof tty_rules - tl) return -1;
            tl += (size_t)n;
        }
        SBPL_LIT("; Terminal UI (tcsetattr, alt screen, TIOCGWINSZ)\n");
        SBPL_FMT("(allow file-read* file-write* (literal \"/dev/tty\")%s)\n",
                 tty_rules);
        /* ioctl limited to what cap/tui.c issues (audit 26): TIOCGWINSZ,
         * and tcgetattr / isatty (TIOCGETA) + tcsetattr TCSANOW / TCSADRAIN
         * / TCSAFLUSH (TIOCSETA / TIOCSETAW / TIOCSETAF). A path-only rule
         * allowed TIOCSTI, which pushes bytes into the terminal's input
         * queue - typed commands for the shell that gets the tty next. */
        SBPL_FMT("(allow file-ioctl (require-all\n"
                 "    (require-any (literal \"/dev/tty\")%s)\n"
                 "    (ioctl-command TIOCGWINSZ TIOCGETA TIOCSETA"
                 " TIOCSETAW TIOCSETAF)))\n\n",
                 tty_rules);
    }

    /* ── Signals, sysctl ────────────────────────────────────── */

    SBPL_LIT("; Signals and sysctl\n"
             "(allow signal (target self))\n"
             "(allow sysctl-read)\n\n");

    /* ── Block dangerous operations ─────────────────────────── */

    SBPL_LIT("; Block exec and fork\n"
             "(deny process-exec*)\n"
             "(deny process-fork)\n");

    /* ── <app_dir>/.hull: never the app's (audit 37) ─────────── */

    /* The `hull dev --agent` sidecars an agent reads as Hull's own output
     * live there. Last, so it wins over every allow above (SBPL: the last
     * matching rule decides) - the app-dir read, a manifest grant, a -d
     * database. Seatbelt matches the real path without case on a
     * case-insensitive volume, and an in-root symlink resolves to it. */
    int app_hull_param = 0;
    if (app_dir) {
        int hn = snprintf(scratch->app_hull, sizeof(scratch->app_hull),
                          "%s/.hull", app_dir);
        if (hn > 0 && (size_t)hn < sizeof(scratch->app_hull)) {
            PARAM_ADD("APP_HULL", scratch->app_hull);
            app_hull_param = 1;
            SBPL_LIT("\n; The app tree's .hull (Hull's own sidecars)\n"
                     "(deny file-read* file-write* (subpath (param \"APP_HULL\")))\n");
        }
    }

    /* ── The Hull-owned internal database (audit convergence §7.2) ── */

    /* After the .hull deny, so these four files (and nothing else in .hull)
     * win: Hull itself opens them; app code reaches them by no capability
     * (the fs resolver, db.open and databases.named refuse .hull). */
    if (policy->internal_db_path) {
        char abs[SEATBELT_PATH_SIZE];
        if (sb_canonical_file(policy->internal_db_path, abs, sizeof abs) == 0) {
            static const char *const suffix[4] = { "", "-wal", "-shm", "-journal" };
            static const char *const key[4] = { "INT_F0", "INT_F1", "INT_F2", "INT_F3" };
            int ok = 1;
            for (int k = 0; k < 4; k++) {
                int n = snprintf(scratch->int_file[k], sizeof(scratch->int_file[k]),
                                 "%s%s", abs, suffix[k]);
                if (n < 0 || (size_t)n >= sizeof(scratch->int_file[k])) ok = 0;
            }
            if (ok) {
                for (int k = 0; k < 4; k++) PARAM_ADD(key[k], scratch->int_file[k]);
                SBPL_LIT("\n; Hull's internal database: the file, its WAL / SHM / journal\n"
                         "(allow file-read* file-write*\n"
                         "    (literal (param \"INT_F0\")) (literal (param \"INT_F1\"))\n"
                         "    (literal (param \"INT_F2\")) (literal (param \"INT_F3\")))\n");
                /* The registry re-checks the path (no symbolic link below
                 * the app directory) with lstat: the directory's metadata
                 * only, never a listing or its other files. */
                if (app_hull_param)
                    SBPL_LIT("(allow file-read-metadata (literal (param \"APP_HULL\")))\n");
            }
        }
    }

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

#endif /* platform dispatch */

int hl_sandbox_kernel_available(void) { return sb_supported(); }

int hl_sandbox_tty_path_ok(const char *path)
{
    static const char prefix[] = "/dev/tty";
    if (!path || strncmp(path, prefix, sizeof prefix - 1) != 0)
        return 0;
    const char *rest = path + sizeof prefix - 1;
    size_t n = 0;
    for (; rest[n]; n++) {
        unsigned char c = (unsigned char)rest[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9')))
            return 0;
    }
    return n >= 1 && n <= 16;
}

int hl_sandbox_own_ttys(char out[HL_SANDBOX_TTY_MAX][HL_SANDBOX_TTY_PATH_MAX])
{
    static const int fds[HL_SANDBOX_TTY_MAX] = {
        STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO
    };
    int count = 0;
    for (int i = 0; i < HL_SANDBOX_TTY_MAX; i++) {
        char name[HL_SANDBOX_TTY_PATH_MAX];
        if (ttyname_r(fds[i], name, sizeof name) != 0)
            continue;
        if (!hl_sandbox_tty_path_ok(name))
            continue;
        int dup = 0;
        for (int j = 0; j < count; j++)
            if (strcmp(out[j], name) == 0) dup = 1;
        if (dup) continue;
        snprintf(out[count], HL_SANDBOX_TTY_PATH_MAX, "%s", name);
        count++;
    }
    return count;
}

int hl_sandbox_seatbelt_profile_for_test(const HlSandboxPolicy *policy,
                                         const char *const *ttys, int tty_count,
                                         char *buf, size_t buf_size,
                                         const char **params, size_t max_params)
{
#if defined(__APPLE__) && !defined(__COSMOPOLITAN__)
    /* Static: the parameters point into it until the next call. */
    static SeatbeltScratch scratch;
    memset(&scratch, 0, sizeof scratch);
    return seatbelt_build_profile(policy, NULL, NULL, NULL, NULL,
                                  ttys, tty_count, buf, buf_size,
                                  params, max_params, &scratch);
#else
    (void)policy; (void)ttys; (void)tty_count;
    (void)buf; (void)buf_size; (void)params; (void)max_params;
    return -1;
#endif
}

int hl_sandbox_seatbelt_profile_db_for_test(const HlSandboxPolicy *policy,
                                            const char *app_dir,
                                            const char *db_path,
                                            char *buf, size_t buf_size,
                                            const char **params,
                                            size_t max_params)
{
#if defined(__APPLE__) && !defined(__COSMOPOLITAN__)
    /* Static: the parameters point into it until the next call (freed, the
     * caller read them from a freed buffer). */
    static SeatbeltScratch scratch;
    memset(&scratch, 0, sizeof scratch);
    return seatbelt_build_profile(policy, app_dir, db_path, NULL, NULL,
                                  NULL, 0, buf, buf_size,
                                  params, max_params, &scratch);
#else
    (void)policy; (void)app_dir; (void)db_path;
    (void)buf; (void)buf_size; (void)params; (void)max_params;
    return -1;
#endif
}

int hl_sandbox_seatbelt_app_profile_for_test(const HlSandboxPolicy *policy,
                                             const char *app_dir,
                                             char *buf, size_t buf_size,
                                             const char **params,
                                             size_t max_params)
{
#if defined(__APPLE__) && !defined(__COSMOPOLITAN__)
    /* Static: the parameters point into it until the next call. */
    static SeatbeltScratch scratch;
    memset(&scratch, 0, sizeof scratch);
    return seatbelt_build_profile(policy, app_dir, NULL, NULL, NULL,
                                  NULL, 0, buf, buf_size,
                                  params, max_params, &scratch);
#else
    (void)policy; (void)app_dir;
    (void)buf; (void)buf_size; (void)params; (void)max_params;
    return -1;
#endif
}

void hl_sandbox_policy_add_default_dsn(HlSandboxPolicy *policy, const char *dsn)
{
    if (!policy || !dsn) return;
    if (hl_sandbox_dsn_is_network(dsn))
        policy->network_outbound = 1;
    if (strncasecmp(dsn, "duckdb://", 9) == 0 && hl_sandbox_db_path(dsn))
        policy->db_duckdb = 1;
}

int hl_sandbox_duckdb_dir_grant(const char *app_dir, const char *db_path,
                                char *out, size_t out_sz)
{
    if (!db_path || !*db_path || !out || out_sz == 0) return -1;
    char dir[SANDBOX_PATH_MAX];
    if ((size_t)snprintf(dir, sizeof dir, "%s", db_path) >= sizeof dir)
        return -1;
    char *sl = strrchr(dir, '/');
    if (!sl) snprintf(dir, sizeof dir, ".");
    else if (sl == dir) sl[1] = '\0';
    else *sl = '\0';

    char real[SANDBOX_PATH_MAX];
    struct stat st;
    if (!realpath(dir, real) || stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    if (hl_host_path_too_broad(real)) return 1;
    if (st.st_mode & S_IWOTH) return 1;     /* /tmp, a shared drop dir */

    if (app_dir) {
        char app[SANDBOX_PATH_MAX];
        if (!realpath(app_dir, app)) return 1;
        size_t rn = strlen(real), an = strlen(app);
        /* The app directory itself or an ancestor of it: the app tree. */
        if (rn <= an && strncmp(app, real, rn) == 0 &&
            (app[rn] == '\0' || app[rn] == '/' || strcmp(real, "/") == 0))
            return 1;
        /* Inside the app's reserved .hull (any letter case; audit 37). */
        if (rn > an + 5 && strncmp(real, app, an) == 0 && real[an] == '/' &&
            strncasecmp(real + an + 1, ".hull", 5) == 0 &&
            (real[an + 6] == '\0' || real[an + 6] == '/'))
            return 1;
    }
    if ((size_t)snprintf(out, out_sz, "%s", real) >= out_sz) return -1;
    return 0;
}

/* ── Phase 1: pre-load pledge ──────────────────────────────────────── */

/* Whether any of stdin/stdout/stderr is a controlling terminal, probed once
 * in phase 1 BEFORE any pledge is applied. Read by phase 2 (which cannot probe
 * itself: phase-1 pledge is already active there, and isatty()'s ioctl(TCGETS)
 * is not permitted by the `stdio` promise). See the probe site below. */
static int g_stdio_is_tty = 0;
/* Per-descriptor result of the same unsandboxed probe: -1 not probed yet. */
static int g_std_tty[3] = { -1, -1, -1 };

int hl_sandbox_std_tty(int fd)
{
    return (fd >= 0 && fd <= 2) ? g_std_tty[fd] : -1;
}

/* Set once a pledge is in force: phase 1 or phase 2 succeeded on a pledge
 * host. Phase 2 sets it too - an embedder may skip the optional phase 1
 * (include/hull/embed.h), and SQLite's fchown no-op keys off it (audit 30). */
static int g_pledged = 0;

int hl_sandbox_pledged(void)
{
    return g_pledged;
}

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
    for (int fd = 0; fd <= 2; fd++)
        g_std_tty[fd] = isatty(fd) ? 1 : 0;
    g_stdio_is_tty = (g_std_tty[0] || g_std_tty[1] || g_std_tty[2]) ? 1 : 0;

    if (!sb_supported()) {
        /* Terse here on purpose: this is a step note, and phase 2 carries the
         * one loud verdict about what is - and is not - enforcing. */
        log_info("[sandbox] phase 1 skipped (no kernel sandbox on this host)");
        return 0;
    }

#ifdef __APPLE__
    /* sandbox_init is irreversible - can't do two-phase on macOS.
     * Phase 2 applies the full Seatbelt profile after manifest extraction.
     * C-level runtime sandboxes (Lua: os removed, JS: no std/os) prevent
     * exec during load_app(). */
    log_info("[sandbox] phase 1 skipped (macOS seatbelt applied in phase 2)");
    return 0;
#endif

#ifdef __linux__
    if (linux_landlock_abi() < 1) {
        log_warn("[sandbox] Landlock unavailable in this Linux kernel; "
                 "phase 1 will apply seccomp only. Phase 2 will fail closed "
                 "unless --allow-degraded-sandbox is set");
    }
    __pledge_mode = 0x0001 | 0x0010; /* KILL_PROCESS | STDERR_LOGGING */
#endif

    /* Phase 1: permissive - needs to allow whatever phase 2 will allow,
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
    g_pledged = 1;

    log_info("[sandbox] phase 1 pledge applied (exec/proc/fork blocked)");
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────── */
/* hl_tool_sandbox_init (build-tool unveil) lives in sandbox_tool.c so the
 * app-runtime sandbox links without the build machinery - see the note in
 * that file. */

int hl_sandbox_apply(const HlSandboxPolicy *policy, const char *app_dir,
                      const char *db_path,
                      const char *tls_cert_path,
                      const char *tls_key_path)
{
    if (!policy)
        return 0;

    /* The app directory is unveiled read-only for the app's own files. A
     * built binary takes it from its working directory, so one started
     * from / (a systemd unit with no WorkingDirectory, a container whose
     * WORKDIR is /) unveiled the whole filesystem for reading - and its
     * relative grants resolved from /. Refused rather than granted. */
    if (app_dir) {
        char real[SANDBOX_PATH_MAX];
        const char *r = realpath(app_dir, real) ? real : app_dir;
        if (strcmp(r, "/") == 0) {
            log_error("[sandbox] the app directory is / - that would grant the "
                      "whole filesystem. Run the app from its own directory "
                      "(e.g. WorkingDirectory= / WORKDIR).");
            return -1;
        }
        /* Nor the user's home, or a directory above it: a built binary
         * started from ~ unveiled every file the user owns (~/.ssh among
         * them), and its relative grants resolved from there. The same rule
         * as the tool sandbox's (hl_host_path_too_broad): $USERPROFILE when
         * $HOME is unset, case-insensitive on Windows, and a bare drive
         * root is a root (audit 9). */
        if (r == real && hl_host_path_too_broad(r)) {
            log_error("[sandbox] the app directory is %s - a drive root, or "
                      "your home directory or above it, which would grant every "
                      "file in it. Run the app from its own directory (e.g. "
                      "WorkingDirectory= / WORKDIR).", r);
            return -1;
        }
    }

    /* ── W^X / no-runtime-dynamic-code fail-closed check ───────────
     *
     * When wx_enforced=1 (the default posture set by
     * hl_sandbox_policy_from_manifest), refuse any manifest that
     * opts into JIT / runtime codegen / dlopen, AND refuse to start
     * on platforms without a kernel sandbox.
     *
     * The escape hatch is `--no-sandbox`, which causes main.c to
     * skip this function entirely and log a warning. We do NOT add
     * a second downgrade flag - there is exactly one. */
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
        /* A host with NO kernel backend at all is handled below, not here.
         * Refusing to start would demand a flag on every run while offering
         * no decision the user could act on: there is no partial sandbox to
         * accept, and no setting that would produce one. A backend that
         * exists but is INCOMPLETE is the opposite case and still fails
         * closed - see the Landlock check further down. */
    }

    if (!sb_supported()) {
        /* Loud, and once. Everything this process does from here is bounded by
         * Hull's capability layer alone - the manifest's fs/env/hosts gates
         * checked in C - with nothing underneath it. Saying "applied" here,
         * which is what this code used to do on every cosmo host, told the
         * reader the opposite of the truth. */
        log_warn("[sandbox] NO kernel sandbox on this host: syscall and "
                 "filesystem confinement%s are not enforced. Hull's "
                 "capability layer (manifest fs / env / hosts) is the only "
                 "boundary. See docs/security.md.",
                 policy->wx_enforced ? ", and W^X," : "");
        return 0;
    }

#if defined(__COSMOPOLITAN__)
    /* Cosmopolitan's libc pledge() / unveil() install the seccomp filter and
     * the Landlock domain on the CALLING thread only (vendor/pledge's TSYNC
     * patches are for the native build; Hull does not patch cosmo's libc). A
     * thread that exists now would keep the phase-1 permissions for good, and
     * threads share one address space. The runners start no thread before
     * phase 2 (the worker pool on first submit, the watchdogs deferred /
     * stopped - audit 43), so more than one here is a thread something else
     * started: an incomplete backend, which fails closed like a kernel
     * without Landlock. A joined thread stays listed for a moment, hence the
     * short wait. */
    if (IsLinux()) {
        int nthreads = hl_host_wait_thread_count(1, 100);
        if (nthreads != 1) {
            char nthr[32];
            if (nthreads < 0)
                snprintf(nthr, sizeof nthr, "an unknown number of threads "
                         "(/proc/self/task unreadable)");
            else
                snprintf(nthr, sizeof nthr, "%d threads", nthreads);
            if (!policy->allow_degraded) {
                log_error("[sandbox] this process has %s when the sandbox "
                          "applies, and a Cosmopolitan build's pledge / unveil "
                          "confine only the calling thread - refusing to report "
                          "a full sandbox. Use a native Linux build of hull "
                          "(its sandbox reaches every thread), or "
                          "--allow-degraded-sandbox", nthr);
                return -1;
            }
            log_warn("[sandbox] DEGRADED: %s at sandbox time; only this one "
                     "is confined (Cosmopolitan pledge / unveil are "
                     "per-thread)", nthr);
        }
    }
#endif

#ifdef __linux__
    int landlock_abi = linux_landlock_abi();
    if (landlock_abi < 1 && !policy->allow_degraded) {
        log_error("[sandbox] Landlock filesystem confinement is unavailable "
                  "in this Linux kernel - refusing to report a full sandbox. "
                  "Enable CONFIG_SECURITY_LANDLOCK, or explicitly use "
                  "--allow-degraded-sandbox for seccomp + Hull capability "
                  "enforcement");
        return -1;
    }
    if (landlock_abi < 1) {
        log_warn("[sandbox] DEGRADED: Landlock unavailable; filesystem paths "
                 "are enforced by Hull's capability layer, not by the kernel");
    }
#endif

#ifdef __APPLE__
    /* ── macOS: Hardened Runtime check (release-only fail-closed) ─
     *
     * The signed gethull.dev release binary always has Hardened
     * Runtime active and is built with -DHL_RELEASE_BUILD. A locally
     * `make`-built binary is unsigned and lacks Hardened Runtime -
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

        /* Resolve symlinks - Seatbelt matches against real paths.
         * e.g. /tmp → /private/tmp on macOS.  Fall back to original
         * if realpath fails (path may not exist yet). */
        char real_app[SEATBELT_PATH_SIZE], real_db[SEATBELT_PATH_SIZE];
        const char *resolved_app = app_dir;
        const char *resolved_db = db_path;

        if (app_dir && realpath(app_dir, real_app))
            resolved_app = real_app;
        if (db_path && realpath(db_path, real_db))
            resolved_db = real_db;

        /* The TUI's own terminals, resolved before sandbox_init (macOS
         * phase 1 applies nothing, so ttyname() still works here). */
        char tty_buf[HL_SANDBOX_TTY_MAX][HL_SANDBOX_TTY_PATH_MAX];
        const char *ttys[HL_SANDBOX_TTY_MAX];
        int tty_count = 0;
        if (policy->tui) {
            tty_count = hl_sandbox_own_ttys(tty_buf);
            for (int i = 0; i < tty_count; i++) ttys[i] = tty_buf[i];
        }

        if (seatbelt_build_profile(policy, resolved_app, resolved_db,
                                    tls_cert_path,
                                    tls_key_path, ttys, tty_count,
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

#ifdef HL_ENABLE_DB
    /* Made (mkdir) before the first unveil: on OpenBSD that call already
     * hides everything not yet unveiled. */
    const char *sqlite_tmp = sb_sqlite_temp_dir();
#endif

    /* App directory: always readable (templates, static assets, source) */
    if (app_dir) {
        if (unveil(app_dir, "r") != 0)
            log_warn("[sandbox] unveil failed for app dir: %s", app_dir);
        /* ...except its .hull (the `hull dev --agent` sidecars; audit 37).
         * OpenBSD's unveil hides a path unveiled with no permissions under a
         * broader grant; Landlock only adds access, so on Linux this is a
         * no-op and the capability layer's refusal is the boundary there. */
        char hull_dir[SANDBOX_PATH_MAX];
        int hn = snprintf(hull_dir, sizeof hull_dir, "%s/.hull", app_dir);
        if (hn > 0 && (size_t)hn < sizeof hull_dir)
            (void)unveil(hull_dir, "");
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
     * so ask the selector whether a linked backend claims the duckdb scheme.
     * Any composed backend used to count (audit 29): --with=postgres or
     * --with=mysql fill the same hook and got DuckDB's /proc, /sys grants. */
    int duckdb_present = 0;
#ifdef HL_ENABLE_DUCKDB
    duckdb_present = 1;
#endif
#ifdef HL_ENABLE_DB
    if (!duckdb_present)
        duckdb_present = hl_db_backend_select("duckdb://", NULL) != NULL;
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
        if (hl_sandbox_resolve_grant(app_dir, policy->fs_read[i],
                                      abs, sizeof(abs), 0) != 0 ||
            unveil(abs, "r") != 0) {
            log_warn("[sandbox] unveil failed for read path: %s",
                     policy->fs_read[i]);
        }
    }

    for (int i = 0; i < policy->fs_write_count; i++) {
        char abs[SANDBOX_PATH_MAX];
        if (hl_sandbox_resolve_grant(app_dir, policy->fs_write[i],
                                      abs, sizeof(abs), 1) != 0 ||
            unveil(abs, "rwc") != 0) {
            log_warn("[sandbox] unveil failed for write path: %s",
                     policy->fs_write[i]);
        }
    }

    /* SQLite database always needs read + write + create */
    if (db_path) {
        if (unveil(db_path, "rwc") != 0)
            log_warn("[sandbox] unveil failed for database: %s", db_path);
        /* And its WAL / SHM / journal files, as the Seatbelt profile grants
         * them (audit 42). Landlock grants an existing inode only, so a file
         * that is not there now cannot be named; the connection that is
         * open at this point (migrations, the default handle) keeps -wal and
         * -shm in place for the process's life. Before, nothing but the
         * main thread's already-open descriptors reached them, and every
         * later connection - a db.async / worker.dispatch pool thread's -
         * worked only because those threads ran outside the sandbox. A link
         * is not followed into a grant. */
        static const char *const sidecar[] = { "-wal", "-shm", "-journal" };
        for (size_t k = 0; k < sizeof sidecar / sizeof sidecar[0]; k++) {
            char sc[SANDBOX_PATH_MAX];
            struct stat sst;
            if ((size_t)snprintf(sc, sizeof sc, "%s%s", db_path, sidecar[k])
                    >= sizeof sc)
                continue;
            if (lstat(sc, &sst) == 0 && S_ISREG(sst.st_mode) &&
                unveil(sc, "rw") != 0)
                log_warn("[sandbox] unveil failed for database file: %s", sc);
        }
        /* A DuckDB database's sidecars are not SQLite's (audit 43): the
         * database's .wal is unlinked at every checkpoint (each shutdown
         * included) and created again by the next commit, and .wal.checkpoint,
         * .wal.recovery and the .tmp/ spill directory are made on demand.
         * With only SQLite's names granted, a second start - the file now
         * there, its .wal gone - failed every commit with "Cannot open file
         * x.duckdb.wal: Permission denied"; and a CHECKPOINT failed the next
         * commit of the first. OpenBSD's unveil takes a name that does not
         * exist yet, so the four names are granted there; Landlock grants
         * existing inodes only and has no per-name rule, so on Linux the
         * database's directory is granted rwc - never the app directory, an
         * ancestor of it, $HOME or above, or a world-writable directory
         * (hl_sandbox_duckdb_dir_grant), which keeps the app tree read-only.
         * A database stored there gets a WARN naming the fix instead. */
        if (policy->db_duckdb) {
            int on_linux = 0;
#if defined(__COSMOPOLITAN__)
            on_linux = IsLinux();
#elif defined(__linux__)
            on_linux = 1;
#endif
            if (on_linux) {
                char ddir[SANDBOX_PATH_MAX];
                int g = hl_sandbox_duckdb_dir_grant(app_dir, db_path,
                                                    ddir, sizeof ddir);
                if (g == 0) {
                    if (unveil(ddir, "rwc") != 0)
                        log_warn("[sandbox] unveil failed for DuckDB "
                                 "database directory: %s", ddir);
                } else {
                    log_warn("[sandbox] DuckDB database %s: its directory is "
                             "not granted (the app directory or an ancestor, "
                             "too broad, or world-writable). DuckDB recreates "
                             "its .wal there after every checkpoint, which the "
                             "Linux sandbox can allow only by making that "
                             "directory writable, so commits will fail with "
                             "\"Permission denied\". Put the database in a "
                             "directory of its own, e.g. "
                             "-d duckdb://data/app.duckdb.", db_path);
                }
            } else {
                static const char *const dsc[] = {
                    ".wal", ".wal.checkpoint", ".wal.recovery", ".tmp"
                };
                for (size_t k = 0; k < sizeof dsc / sizeof dsc[0]; k++) {
                    char sc[SANDBOX_PATH_MAX];
                    if ((size_t)snprintf(sc, sizeof sc, "%s%s", db_path,
                                         dsc[k]) >= sizeof sc)
                        continue;
                    (void)unveil(sc, "rwc");
                }
            }
        }
    }

    /* Hull's internal database (§7.2) and its sidecars. Opened before this
     * runs, so the file and its WAL / SHM exist; a -journal may not (unveil
     * of a missing file fails under Landlock - ignored). On OpenBSD each
     * file is unveiled under the .hull that was hidden above, so the rest of
     * .hull stays hidden. Landlock cannot grant creating a sibling through a
     * file rule, so on Linux the directory itself is unveiled when it is
     * <app>/.hull - Landlock could not hide .hull anyway (the capability
     * layer is the boundary there, see above). */
    if (policy->internal_db_path) {
        static const char *const suffix[4] = { "", "-wal", "-shm", "-journal" };
        for (int k = 0; k < 4; k++) {
            char f[SANDBOX_PATH_MAX];
            int n = snprintf(f, sizeof f, "%s%s", policy->internal_db_path, suffix[k]);
            if (n > 0 && (size_t)n < sizeof f && unveil(f, "rwc") != 0 && k == 0)
                log_warn("[sandbox] unveil failed for the internal database: %s", f);
        }
#if !defined(__OpenBSD__)
        int on_openbsd = 0;
#if defined(__COSMOPOLITAN__)
        on_openbsd = IsOpenbsd();
#endif
        char app_real[SANDBOX_PATH_MAX];
        if (!on_openbsd && app_dir && realpath(app_dir, app_real)) {
            char hull_dir[SANDBOX_PATH_MAX];
            int hn = snprintf(hull_dir, sizeof hull_dir, "%s/.hull/", app_real);
            if (hn > 0 && (size_t)hn < sizeof hull_dir &&
                strncmp(policy->internal_db_path, hull_dir, (size_t)hn) == 0) {
                hull_dir[hn - 1] = '\0';
                (void)unveil(hull_dir, "rwc");
            }
        }
#endif
    }

    /* No CA bundle unveil (audit 29): the anchor is already in memory
     * (ca_trust.c reads it before phase 2), and the ladder's "(embedded)"
     * label, unveiled as a relative path, resolved against the working
     * directory - a planted "(embedded) -> /" symlink unveiled everything. */

    /* Hull runtime cache root ($HOME/.hull/cache/) - see SBPL
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

#ifdef HL_ENABLE_DB
    /* SQLite temp files: temp_store=FILE spills sorts, temp b-trees and
     * VACUUM's copy here (hl_hull_sqlite_temp_dir, a private 0700 dir that
     * sqlite3_temp_directory names too). */
    if (sqlite_tmp && unveil(sqlite_tmp, "rwc") != 0)
        log_warn("[sandbox] unveil failed for SQLite temp dir: %s", sqlite_tmp);
#endif

    /* DNS resolution: when manifest declares outbound hosts, glibc's
     * getaddrinfo() needs to read these files. The "dns" pledge promise
     * allows the syscalls; unveil controls which paths they may touch.
     * Errors are non-fatal - the file may not exist on minimal systems. */
    if (policy->network_outbound) {
        unveil("/etc/resolv.conf",   "r");
        unveil("/etc/hosts",         "r");
        unveil("/etc/nsswitch.conf", "r");
        unveil("/etc/services",      "r");
        unveil("/etc/gai.conf",      "r");   /* glibc getaddrinfo sorting */
        unveil("/etc/host.conf",     "r");   /* glibc resolver options */
        /* systemd-resolved IPC socket (modern Linux) and nscd socket */
        unveil("/run/systemd/resolve", "r");
        unveil("/var/run/nscd",        "r");
        /* Named files only (audit 24): this granted all of /etc as well -
         * /etc/shadow where readable, SSH host keys, every service's
         * config. No CA store needs it: hl_ca_trust_resolve reads the
         * bundle into memory before the sandbox applies, and an explicit
         * --ca-bundle is unveiled above. */
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
                int d = policy->gpu_devices[i];
                /* The manifest parsers keep only 0..HL_GPU_MAX_DEVICES-1;
                 * refuse anything else here too (audit 23: 128 + d was a
                 * signed overflow for a huge d). */
                if (d < 0 || d >= HL_GPU_MAX_DEVICES)
                    continue;
                char node[64];
                snprintf(node, sizeof(node), "/dev/dri/renderD%d", 128 + d);
                if (unveil(node, "rw") != 0)
                    log_warn("[sandbox] unveil failed for %s", node);
            }
        } else {
            /* No device restriction - unveil all of /dev/dri */
            if (unveil("/dev/dri", "rw") != 0)
                log_warn("[sandbox] unveil failed for /dev/dri");
        }
        if (unveil("/proc/self", "r") != 0)
            log_warn("[sandbox] unveil failed for /proc/self");
    }

    /* Terminal UI - unveil the controlling tty so tcsetattr /
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

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
    /* Every thread must carry the Landlock domain, not only this one (audit
     * 42): a thread that already existed - a library's (a GPU driver's,
     * DuckDB's), or a watchdog started at app load - and kept its full
     * filesystem reach voided the domain for the whole process, since threads
     * share one address space. The polyfill applies it with
     * LANDLOCK_RESTRICT_SELF_TSYNC where the kernel has it, else asks each
     * thread to restrict itself (patch 0004); a thread it could not reach
     * (its signal blocked, its call refused, /proc unreadable) is counted.
     * That is an incomplete backend, so it fails closed like a kernel without
     * Landlock. The seccomp filters go to every thread through TSYNC (patch
     * 0003), which needs no such check. */
    if (landlock_abi >= 1 && hull_unveil_unsynced_threads != 0) {
        if (!policy->allow_degraded) {
            char nthr[32];
            if (hull_unveil_unsynced_threads < 0)
                snprintf(nthr, sizeof nthr, "an unknown number of");
            else
                snprintf(nthr, sizeof nthr, "%d", hull_unveil_unsynced_threads);
            log_error("[sandbox] the Landlock filesystem domain could not be "
                      "applied to %s other thread(s) of this process (this "
                      "kernel's Landlock ABI %d has no TSYNC, and those threads "
                      "did not restrict themselves) - refusing to report a full "
                      "sandbox. Use a kernel with Landlock TSYNC (ABI 8+), or "
                      "--allow-degraded-sandbox", nthr, landlock_abi);
            return -1;
        }
        log_warn("[sandbox] DEGRADED: %d thread(s) lack the Landlock "
                 "filesystem domain (seccomp still applies to every thread)",
                 hull_unveil_unsynced_threads);
    }
#endif

    /* ── Pledge: restrict syscall families ──────────────────── */

    /*
     * Build promise string from manifest capabilities.
     *
     * Always needed:
     *   stdio  - basic I/O on open fds, epoll/kqueue, signals
     *   rpath  - SQLite reads, app file reads
     *   wpath  - SQLite WAL writes
     *   cpath  - SQLite journal/WAL creation
     *   flock  - SQLite locking
     *   fattr  - futimes() on owned blob-cache fds (LRU atime touch
     *            in hl_blob_store_reader_open when track_access=1).
     *            Glibc since 2.6.22 implements futimes() as utimensat(),
     *            which pledge categorises under fattr. Scope is narrow:
     *            owned fds we just opened from $HOME/.hull/blobs/runtime/,
     *            unveil prevents any other path from being touched. The
     *            bytecode and template caches need this to drive their
     *            LRU eviction policy.
     *
     * Conditional:
     *   inet   - socket-family syscalls. Needed when the app serves
     *            HTTP (network_inbound) OR makes outbound requests
     *            (network_outbound). CLI-mode apps with neither drop
     *            it entirely; their kernel-visible syscall surface
     *            shrinks accordingly.
     *   dns    - outbound HTTP name resolution (when hosts declared)
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
     *   dns      - UDP/IP queries to the resolver
     *   netlink  - Linux/glibc getaddrinfo() opens AF_NETLINK to enumerate
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
            /* Should never happen - promises[256] vs ~50 bytes total -
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
    /* Phase 1 is optional (hl_embed_seal callers may skip it): record the
     * pledge here too, or a root embedder's first WAL hit "pledge chown"
     * through SQLite's fchown (db_fchown in cap/db.c; audit 30). */
    g_pledged = 1;

#if defined(__linux__)
    if (landlock_abi < 1) {
        log_warn("[sandbox] applied DEGRADED (filesystem: capability layer; "
                 "syscalls: seccomp pledge: %s)", promises);
    } else
#endif
    {
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
        /* threads: how the Landlock domain reached the threads that already
         * existed - "single" (none), "tsync" (Landlock TSYNC) or "signal"
         * (each restricted itself). Seccomp always reaches all (TSYNC). */
        log_info("[sandbox] applied (unveil: %d read, %d write; pledge: %s; "
                 "threads: %s)",
                 policy->fs_read_count, policy->fs_write_count, promises,
                 hull_unveil_sync_mode == 1 ? "tsync"
                 : hull_unveil_sync_mode == 2 ? "signal" : "single");
#else
        log_info("[sandbox] applied (unveil: %d read, %d write; pledge: %s)",
                 policy->fs_read_count, policy->fs_write_count, promises);
#endif
    }

    return 0;
}

/* ── Policy builder ────────────────────────────────────────────────── */

/* True if a DSN names a NETWORK database backend (dials out), vs a local
 * sqlite/duckdb file. A "$VAR"/"${VAR}" env-ref has an unknown scheme at
 * manifest time, so it's treated as possibly-network (the common
 * `$DATABASE_URL` = postgres case must not be blocked). A scheme-less bare
 * path is SQLite (local). */
int hl_sandbox_dsn_is_network(const char *dsn)
{
    if (!dsn || !*dsn) return 0;
    if (dsn[0] == '$') return 1;                 /* env-ref: assume network */
    /* Case-insensitive to match the DSN backend selector (db_select.c) and
     * serve.c's db_dsn_is_network - else an uppercase-scheme named DSN would
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

/* Map a database DSN to the local filesystem path the kernel sandbox must gate,
 * or NULL when there is no local file to gate: an in-memory database
 * (":memory:") or a network backend (postgres:// / mysql:// / mariadb://).
 * Without this the sandbox would treat the raw DSN string as a path and try to
 * unveil e.g. "duckdb://:memory:", which fails (a harmless warning) or, for a
 * scheme-qualified file DSN, gates the wrong directory. A scheme-less DSN (a
 * bare path, ":memory:", or a "file:" URI) is returned unchanged so existing
 * SQLite behavior is untouched. Returns a pointer into @p dsn.
 *
 * Only the FILE schemes (sqlite://, file://, duckdb://) yield a path; every
 * other scheme yields NULL - not just the network backends Hull knows. A
 * mistyped or unknown network DSN ("postgress://u:pw@h", "redis://...")
 * otherwise came back as "u:pw@h/db", and its password reached unveil and
 * the sandbox's log lines (audit 11).
 *
 * Shared by both entry points (serve.c, and the app.main runner in
 * serve_cli.c, which passed the raw -d DSN - a network DSN with its password
 * then reached unveil and the sandbox's log lines; audit 10). */
const char *hl_sandbox_db_path(const char *dsn)
{
    if (!dsn) return NULL;
    const char *sep = strstr(dsn, "://");
    if (!sep) return dsn;   /* scheme-less: unchanged existing behavior */
    size_t n = (size_t)(sep - dsn);
    /* A "://" not preceded by a URI scheme (RFC 3986: a letter, then letters,
     * digits, '+', '-', '.') is part of a scheme-less path. */
    int scheme = n > 0 && isalpha((unsigned char)dsn[0]);
    for (size_t i = 1; scheme && i < n; i++) {
        unsigned char c = (unsigned char)dsn[i];
        scheme = isalnum(c) || c == '+' || c == '-' || c == '.';
    }
    if (!scheme) return dsn;
    /* Only file backends have a local file to sandbox. */
    if (!((n == 6 && strncasecmp(dsn, "sqlite", 6) == 0) ||
          (n == 4 && strncasecmp(dsn, "file",   4) == 0) ||
          (n == 6 && strncasecmp(dsn, "duckdb", 6) == 0)))
        return NULL;
    /* The target follows "://". An in-memory or empty target needs no file
     * gating. */
    const char *path = sep + 3;
    if (*path == '\0' || strcmp(path, ":memory:") == 0)
        return NULL;
    return path;
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
 * for a DB-only app that has no http `hosts` - without it the kernel sandbox
 * blocks the connect (pledge SIGKILL on Linux). The `-d` default DSN is not in
 * the manifest (it lives in cfg); both entry points OR it in through
 * hl_sandbox_policy_add_default_dsn. */
int hl_sandbox_manifest_has_network_db(const HlManifest *m)
{
    if (!m) return 0;
    for (int i = 0; i < m->databases.named_count; i++)
        if (hl_sandbox_dsn_is_network(m->databases.named[i].dsn)) return 1;
    if (hl_sandbox_dsn_is_network(m->databases.internal)) return 1;
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

    /* W^X is the platform's default posture - independent of manifest.
     * Only `--no-sandbox` disables it (by skipping hl_sandbox_apply
     * entirely in main.c). */
    policy->wx_enforced = 1;

    if (!manifest)
        return;

    /* Borrow array pointers - manifest must outlive the policy. */
    policy->fs_read         = manifest->fs_read;
    policy->fs_read_count   = manifest->fs_read_count;
    policy->fs_write        = manifest->fs_write;
    policy->fs_write_count  = manifest->fs_write_count;

    /* Sandbox only needs the boolean "any outbound network?" decision. The
     * actual host allowlist is enforced in cap/http.c (http.fetch), for DB by
     * the backend + databases.dynamic policy, and for SSH by cap/ssh_policy.c.
     * Granted for http `hosts`, a declared network database connection, OR a
     * declared ssh.connect - each of which dials out, and each of which was
     * SIGKILLed on connect before it was listed here.
     *
     * ssh belongs here because hull/ssh deliberately does NOT require `hosts`
     * (see module_registry.c): ssh.connect is its own grant, so a fleet tool
     * that declares only `ssh` declares no hosts, and would otherwise have
     * been denied the socket its manifest plainly asks for. */
    policy->network_outbound = (manifest->hosts_count > 0)
                             || hl_sandbox_manifest_has_network_db(manifest)
                             || manifest->ssh.connect.declared;

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
    policy->allow_degraded = 0;
}
