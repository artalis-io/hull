/**
 * @file cache_dir.c
 * @brief Hull runtime cache directory helpers ($HOME/.hull/cache/).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cache_dir.h"
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

int hl_hull_cache_dir(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;
    const char *home = getenv("HOME");
    if (!home || !*home) { errno = ENOENT; return -1; }

    /* Runtime caches share the same on-disk pool as app blobs but
     * live under a `runtime/` subtree. Apps' blob stores live
     * elsewhere (manifest-declared); runtime caches partition under
     * blobs/runtime/<kind>/ so a single `hull cache list` walk
     * (future §1.5.b-X-5) can enumerate them cleanly. The env-var
     * surface (HULL_NO_CACHE etc.) keeps "cache" nomenclature
     * because that's the user-facing intent — these directories ARE
     * caches even though the on-disk layout is the blob shape. */
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

int hl_hull_cache_disabled(const char *kind)
{
    if (env_truthy("HULL_NO_CACHE")) return 1;
    if (!kind) return 0;

    /* Build `HULL_NO_<KIND>_CACHE`, uppercasing `kind`. */
    char env_name[64];
    size_t prefix_len = strlen("HULL_NO_");
    size_t suffix_len = strlen("_CACHE");
    size_t kind_len   = strlen(kind);
    if (prefix_len + kind_len + suffix_len + 1 > sizeof(env_name)) return 0;
    memcpy(env_name, "HULL_NO_", prefix_len);
    for (size_t j = 0; j < kind_len; j++) {
        char c = kind[j];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        env_name[prefix_len + j] = c;
    }
    memcpy(env_name + prefix_len + kind_len, "_CACHE", suffix_len);
    env_name[prefix_len + kind_len + suffix_len] = '\0';
    return env_truthy(env_name);
}
