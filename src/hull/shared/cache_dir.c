/**
 * @file cache_dir.c
 * @brief Hull runtime cache directory helpers ($HOME/.hull/cache/).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/cache_dir.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Create a directory if absent; tolerate concurrent creation. */
static int ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
}

/* Cache subdir names are filesystem path components — restrict to a
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

/* Walk path components and mkdir each missing piece. */
static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (ensure_dir(buf, 0755) != 0) return -1;
            buf[i] = saved;
        }
    }
    return 0;
}

int hl_hull_cache_dir(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;

    /* HULL_CACHE_DIR override — used for per-app cache isolation in
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
     * this override — tools are durable signed downloads with a
     * stable system home, not per-app caches. See cache_registry.c
     * for the registry-wide path-resolution rules. */
    const char *override = getenv("HULL_CACHE_DIR");
    if (override && *override) {
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
        if (mkdir_p(abspath) != 0) return -1;
        return 0;
    }

    /* Default: $HOME/.hull/blobs/runtime/. Runtime caches share the
     * on-disk blob layout (sha-keyed, sharded shards) with apps'
     * blob stores but partition under blobs/runtime/<kind>/ so
     * `hull cache list` can enumerate them in one tree walk. The
     * env-var surface (HULL_NO_CACHE etc.) keeps "cache"
     * nomenclature — these directories ARE caches even though the
     * disk layout is the blob shape. */
    const char *home = getenv("HOME");
    if (!home || !*home) { errno = ENOENT; return -1; }

    char hull_dir[PATH_MAX];
    int n = snprintf(hull_dir, sizeof(hull_dir), "%s/.hull", home);
    if (n < 0 || (size_t)n >= sizeof(hull_dir)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (ensure_dir(hull_dir, 0755) != 0) return -1;

    char blobs_dir[PATH_MAX];
    n = snprintf(blobs_dir, sizeof(blobs_dir), "%s/blobs", hull_dir);
    if (n < 0 || (size_t)n >= sizeof(blobs_dir)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (ensure_dir(blobs_dir, 0755) != 0) return -1;

    char runtime_dir[PATH_MAX];
    n = snprintf(runtime_dir, sizeof(runtime_dir), "%s/runtime", blobs_dir);
    if (n < 0 || (size_t)n >= sizeof(runtime_dir)) {
        errno = ENAMETOOLONG; return -1;
    }
    if (ensure_dir(runtime_dir, 0755) != 0) return -1;

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
    if (ensure_dir(sub, 0755) != 0) return -1;

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
        /* Kind name too long to fit the buffer — fail open (cache
         * stays active). Mirrors the previous inline behaviour. */
        return 0;
    }
    return env_truthy(env_name);
}
