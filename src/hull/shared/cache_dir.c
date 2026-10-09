/**
 * @file cache_dir.c
 * @brief Hull runtime cache directory helpers ($HOME/.hull/cache/).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/cache_dir.h"
#include "hull/shared/host.h"
#include "hull/shared/fs_util.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

/* Cache subdir names are filesystem path components - restrict to a
 * conservative charset so a misconfigured caller can't path-traverse
 * out of `$HOME/.hull/cache/`. Allowed: [A-Za-z0-9_-]+ */
static int name_valid(const char *name)
{
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

/* The caches hold compiled Lua / QuickJS bytecode and AOT code that Hull
 * loads without re-verifying it - Lua bytecode in particular is not safe
 * to load from an untrusted source. So a cache directory is used only if
 * nobody else can write into it: a real directory (not a symlink), owned
 * by this user, and not group- or world-writable. A shared HULL_CACHE_DIR
 * (or a ~/.hull another account can write to) was a way to run code in
 * every app that used it. Failing this disables the cache - the source is
 * compiled afresh - with one WARN naming the directory.
 *
 * Not on Windows: Cosmopolitan reports no meaningful owner or mode bits
 * there, and the cache lives under the user's own profile. */
static int cache_dir_trusted(const char *path)
{
    if (hl_host_is_windows()) return 1;
    struct stat st;
    const char *why = NULL;
    if (lstat(path, &st) != 0)              why = "cannot be inspected";
    else if (S_ISLNK(st.st_mode))           why = "is a symlink";
    else if (!S_ISDIR(st.st_mode))          why = "is not a directory";
    else if (st.st_uid != geteuid())        why = "is owned by another user";
    else if (st.st_mode & (S_IWGRP | S_IWOTH)) why = "is writable by other users";
    /* And every directory above it: a parent another user can write (with
     * no sticky bit) lets them rename this one away and put their own in
     * its place after the check. Each must be the user's or root's, and
     * shared-writable only when sticky (/tmp). */
    if (!why) {
        char up[4096];
        int n = snprintf(up, sizeof up, "%s", path);
        if (n < 0 || (size_t)n >= sizeof up) why = "path is too long";
        while (!why) {
            char *sl = strrchr(up, '/');
            if (!sl) break;
            if (sl == up) { up[1] = '\0'; } else { *sl = '\0'; }
            struct stat ps;
            if (stat(up, &ps) != 0) { why = "has a parent that cannot be inspected"; break; }
            if (ps.st_uid != geteuid() && ps.st_uid != 0)
                why = "has a parent owned by another user";
            else if ((ps.st_mode & (S_IWGRP | S_IWOTH)) && !(ps.st_mode & S_ISVTX))
                why = "has a parent writable by other users";
            if (sl == up) break;   /* checked "/" */
        }
    }
    if (!why) return 1;
    static int warned;
    if (!warned) {
        warned = 1;
        log_warn("[cache] %s %s; the bytecode / template / AOT caches are "
                 "off (fix its ownership or permissions, or set "
                 "HULL_CACHE_DIR to a private directory)", path, why);
    }
    errno = EPERM;
    return 0;
}

int hl_hull_cache_dir(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;

    /* HULL_CACHE_DIR override - used for per-app cache isolation in
     * shared / multi-tenant environments where the default
     * $HOME/.hull/blobs/runtime/ pool would expose cache content to
     * any process running under the same user. Set per-deployment
     * (systemd EnvironmentFile, k8s env, Docker -e):
     *
     *   HULL_CACHE_DIR=/var/lib/myapp/cache hull myapp.lua
     *
     * Must be an absolute path. The directory tree is created on
     * demand; the sandbox auto-allows it (via this function's
     * return value).
     *
     * Tools storage ($HOME/.hull/blobs/tools/) is NOT affected by
     * this override - tools are durable signed downloads with a
     * stable system home, not per-app caches. See cache_registry.c
     * for the registry-wide path-resolution rules. */
    const char *override = getenv("HULL_CACHE_DIR");
    if (override && *override) {
        /* A Git Bash / MSYS2 user exports this as "C:/hull-cache", the mixed
         * form that shell hands a native program. That is absolute, so accept
         * it by rewriting to the rooted form the rest of this function (and
         * blob_store, which also tests for a leading '/') requires. No-op off
         * Windows and on an already-rooted path. */
        char ovr[HL_HOST_PATH_MAX];
        if (hl_host_normalize_path(override, ovr, sizeof ovr) < 0) {
            errno = ENAMETOOLONG;
            return -1;
        }
        override = ovr;

        if (override[0] != '/') { errno = EINVAL; return -1; }
        size_t olen = strlen(override);
        /* Strip trailing slashes for canonical form. */
        while (olen > 1 && override[olen - 1] == '/') olen--;
        if (olen + 2 > out_sz) { errno = ENAMETOOLONG; return -1; }
        memcpy(out, override, olen);
        out[olen]     = '/';
        out[olen + 1] = '\0';

        /* mkdir -p the override path. Use a private buffer to avoid
         * mutating `out`. */
        char abspath[PATH_MAX];
        if (olen >= sizeof(abspath)) { errno = ENAMETOOLONG; return -1; }
        memcpy(abspath, override, olen);
        abspath[olen] = '\0';
        if (hl_mkdir_p(abspath, 0700) != 0) return -1;
        /* The cache directory is granted read-write-create to the app and
         * tool sandboxes, so HULL_CACHE_DIR=/ or =$HOME made the filesystem
         * or every file the user owns writable, and =~/.hull the cache and
         * tool-cache keys (audit 9). Refused with the caches off, as for an
         * untrusted directory. A directory INSIDE ~/.hull is refused too
         * (audit 10): =~/.hull/blobs/tools or =~/.hull/keys handed the app
         * the signed tool store or the signing keys. */
        if (hl_host_path_too_broad(abspath) ||
            hl_host_path_covers_home(abspath, ".hull") ||
            hl_host_path_under_home(abspath, ".hull")) {
            static int warned_broad;
            if (!warned_broad) {
                warned_broad = 1;
                log_warn("[cache] HULL_CACHE_DIR %s is the filesystem root, "
                         "your home directory or above, or ~/.hull, inside it "
                         "or above; "
                         "the bytecode / template / AOT caches are off (set "
                         "it to a directory of its own)", abspath);
            }
            errno = EPERM;
            return -1;
        }
        if (!cache_dir_trusted(abspath)) return -1;
        return 0;
    }

    /* Default: $HOME/.hull/blobs/runtime/. Runtime caches share the
     * on-disk blob layout (sha-keyed, sharded shards) with apps'
     * blob stores but partition under blobs/runtime/<kind>/ so
     * `hull cache list` can enumerate them in one tree walk. The
     * env-var surface (HULL_NO_CACHE etc.) keeps "cache"
     * nomenclature - these directories ARE caches even though the
     * disk layout is the blob shape. */
    const char *home = getenv("HOME");
    if (!home || !*home) { errno = ENOENT; return -1; }

    char hull_dir[PATH_MAX];
    int n = snprintf(hull_dir, sizeof(hull_dir), "%s/.hull", home);
    if (n < 0 || (size_t)n >= sizeof(hull_dir)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (hl_ensure_dir(hull_dir, 0755) != 0) return -1;

    char blobs_dir[PATH_MAX];
    n = snprintf(blobs_dir, sizeof(blobs_dir), "%s/blobs", hull_dir);
    if (n < 0 || (size_t)n >= sizeof(blobs_dir)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (hl_ensure_dir(blobs_dir, 0755) != 0) return -1;

    char runtime_dir[PATH_MAX];
    n = snprintf(runtime_dir, sizeof(runtime_dir), "%s/runtime", blobs_dir);
    if (n < 0 || (size_t)n >= sizeof(runtime_dir)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (hl_ensure_dir(runtime_dir, 0700) != 0) return -1;
    if (!cache_dir_trusted(runtime_dir)) return -1;

    /* Trailing slash for easy concatenation. */
    n = snprintf(out, out_sz, "%s/", runtime_dir);
    if (n < 0 || (size_t)n >= out_sz) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}

int hl_hull_cache_subdir(const char *name, char *out, size_t out_sz)
{
    if (!name_valid(name)) { errno = EINVAL; return -1; }
    if (!out || out_sz < 2) return -1;

    char cache_dir[PATH_MAX];
    if (hl_hull_cache_dir(cache_dir, sizeof(cache_dir)) != 0) return -1;
    /* hl_hull_cache_dir returns with trailing slash. */

    char sub[PATH_MAX];
    int n = snprintf(sub, sizeof(sub), "%s%s", cache_dir, name);
    if (n < 0 || (size_t)n >= sizeof(sub)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (hl_ensure_dir(sub, 0700) != 0) return -1;
    if (!cache_dir_trusted(sub)) return -1;

    n = snprintf(out, out_sz, "%s/", sub);
    if (n < 0 || (size_t)n >= out_sz) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}

static int env_truthy(const char *name)
{
    const char *v = getenv(name);
    if (!v) return 0;
    if (*v == '\0' || *v == '0') return 0;
    if ((v[0] == 'f' || v[0] == 'F') &&
        (v[1] == 'a' || v[1] == 'A')) return 0;        /* "false"/"FALSE" */
    return 1;
}

int hl_hull_cache_env_name(const char *kind, char *out, size_t out_sz)
{
    if (!kind || !out) return -1;
    size_t prefix_len = strlen("HULL_NO_");
    size_t suffix_len = strlen("_CACHE");
    size_t kind_len   = strlen(kind);
    if (kind_len == 0) return -1;
    if (prefix_len + kind_len + suffix_len + 1 > out_sz) return -1;

    memcpy(out, "HULL_NO_", prefix_len);
    for (size_t j = 0; j < kind_len; j++) {
        char c = kind[j];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[prefix_len + j] = c;
    }
    memcpy(out + prefix_len + kind_len, "_CACHE", suffix_len);
    out[prefix_len + kind_len + suffix_len] = '\0';
    return 0;
}

int hl_hull_cache_disabled(const char *kind)
{
    if (env_truthy("HULL_NO_CACHE")) return 1;
    if (!kind) return 0;

    char env_name[64];
    if (hl_hull_cache_env_name(kind, env_name, sizeof(env_name)) != 0) {
        /* Kind name too long to fit the buffer - fail open (cache
         * stays active). Mirrors the previous inline behaviour. */
        return 0;
    }
    return env_truthy(env_name);
}

/* ── SQLite temp directory (audit 11) ────────────────────────────────
 *
 * Hull's SQLite connections run with temp_store=FILE so a big sort, CREATE
 * INDEX, GROUP BY / DISTINCT / UNION or VACUUM spills to disk instead of
 * growing in the process-wide, hard-limited heap. Those temp files must land
 * in a directory the kernel sandbox grants, so Hull picks it rather than
 * SQLite: a private "hull-sqlite-<euid>" under the first existing absolute
 * directory in SQLite's own order (SQLITE_TMPDIR, TMPDIR, /var/tmp, /usr/tmp,
 * /tmp). It is used only if it is a real directory (not a symlink) owned by
 * this user with no group / other access, so another account cannot plant or
 * read the temp files. Resolved once, canonicalised (Seatbelt matches real
 * paths), and created on the first call - which the sandbox makes before its
 * first grant. Not on Windows: SQLite's unix VFS under Cosmopolitan finds no
 * usable temp path there (SQLITE_IOERR_GETTEMPPATH, even with one named), so
 * the connections keep temp_store=MEMORY (hl_cap_db_temp_on_disk). */
static char           g_sqlite_tmp[PATH_MAX];
static int            g_sqlite_tmp_ok;
static pthread_once_t g_sqlite_tmp_once = PTHREAD_ONCE_INIT;

static void sqlite_tmp_resolve(void)
{
    if (hl_host_is_windows()) return;
    const char *cand[5] = {
        getenv("SQLITE_TMPDIR"), getenv("TMPDIR"), "/var/tmp", "/usr/tmp", "/tmp",
    };
    const char *base = NULL;
    for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        struct stat st;
        if (cand[i] && cand[i][0] == '/' && stat(cand[i], &st) == 0 &&
            S_ISDIR(st.st_mode)) {
            base = cand[i];
            break;
        }
    }
    if (!base) return;
    size_t bl = strlen(base);
    while (bl > 1 && base[bl - 1] == '/') bl--;
    if (bl == 1) bl = 0;   /* "/" itself: no doubled slash */
    char raw[PATH_MAX];
    int n = snprintf(raw, sizeof raw, "%.*s/hull-sqlite-%lu", (int)bl, base,
                     (unsigned long)geteuid());
    if (n < 0 || (size_t)n >= sizeof raw) return;
    if (mkdir(raw, 0700) != 0 && errno != EEXIST) return;
    struct stat st;
    if (lstat(raw, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid())
        return;
    if ((st.st_mode & 077) && chmod(raw, 0700) != 0) return;
    if (!realpath(raw, g_sqlite_tmp)) return;
    g_sqlite_tmp_ok = 1;
}

const char *hl_hull_sqlite_temp_dir(void)
{
    (void)pthread_once(&g_sqlite_tmp_once, sqlite_tmp_resolve);
    return g_sqlite_tmp_ok ? g_sqlite_tmp : NULL;
}
