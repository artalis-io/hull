/**
 * @file state_dir.c
 * @brief The per-user state directory - see shared/state_dir.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/shared/state_dir.h"
#include "hull/shared/cache_dir.h"   /* hl_hull_dir_trusted */
#include "hull/shared/fs_util.h"     /* hl_ensure_dir */
#include "hull/shared/host.h"

#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

int hl_state_dir_name_valid(const char *name)
{
    if (!name || !*name) return 0;
    size_t n = strlen(name);
    if (n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (i == 0) { if (!alnum) return 0; continue; }
        if (!(alnum || c == '.' || c == '_' || c == '-')) return 0;
    }
    if (strstr(name, "..")) return 0;
    return 1;
}

/* Make and check @p path through the one validator every operator-given
 * private directory goes through (cache_dir.h): the ~/.hull, root / home and
 * trust rules, mkdir -p 0700. Logged at ERROR: the app does not start
 * without the state it declared. */
static int finish(const char *path, char *out, size_t out_sz)
{
    const char *why = NULL;
    if (hl_hull_private_dir_prepare(path, 0700, out, out_sz, &why) != 0) {
        log_error("[state] %s %s; refusing to use it as the state directory "
                  "(it is granted read-write to the app: give it a private "
                  "directory of its own, or set HULL_STATE_DIR)",
                  path, why ? why : "is refused");
        return -1;
    }
    return 0;
}

/* "<base>/<name>" with any trailing separators of @p base dropped; 0 / -1
 * (too long). */
static int join(const char *base, const char *name, char *buf, size_t sz)
{
    size_t bl = strlen(base);
    while (bl > 1 && (base[bl - 1] == '/' || base[bl - 1] == '\\')) bl--;
    int n = snprintf(buf, sz, "%.*s/%s", (int)bl, base, name);
    return (n < 0 || (size_t)n >= sz) ? -1 : 0;
}

/* The state directory's path for @p name into @p path (not made, not
 * checked): HULL_STATE_DIR, else %LOCALAPPDATA%\<name> on Windows, else
 * $XDG_STATE_HOME/<name>, else <home>/.local/state/<name>. With
 * @p make_parents the XDG ~/.local and ~/.local/state are made 0755 when
 * absent. 0 / -1 (logged). */
static int state_dir_path(const char *name, char *path, size_t path_sz,
                          int make_parents)
{
    if (!hl_state_dir_name_valid(name)) {
        log_error("[state] state.name %s is not a directory name Hull takes "
                  "([a-z0-9] then up to 63 of [a-z0-9._-], no \"..\")",
                  name ? "(given)" : "(missing)");
        return -1;
    }

    const char *override = getenv("HULL_STATE_DIR");
    if (override && *override) {
        size_t n = strlen(override);
        if (n >= path_sz) { log_error("[state] state directory path is too long"); return -1; }
        memcpy(path, override, n + 1);
        return 0;
    }

    if (hl_host_is_windows()) {
        const char *local = getenv("LOCALAPPDATA");
        char base[HL_HOST_PATH_MAX];
        if (local && *local && hl_host_normalize_path(local, base, sizeof base) == 0
            && base[0] == '/') {
            if (join(base, name, path, path_sz) != 0) {
                log_error("[state] state directory path is too long");
                return -1;
            }
            return 0;
        }
        /* No LOCALAPPDATA: fall through to the POSIX layout under the home. */
    }

    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] == '/') {          /* a '.' / '..' in it is refused by finish */
        if (join(xdg, name, path, path_sz) != 0) {
            log_error("[state] state directory path is too long");
            return -1;
        }
        return 0;
    }

    const char *home = hl_host_home();
    if (!home) {
        log_error("[state] no home directory: set HULL_STATE_DIR (or HOME)");
        return -1;
    }
    char local_state[PATH_MAX];
    int n = snprintf(local_state, sizeof local_state, "%s/.local/state", home);
    if (n < 0 || (size_t)n >= sizeof local_state) {
        log_error("[state] state directory path is too long");
        return -1;
    }
    /* ~/.local and ~/.local/state are the user's, made 0755 if absent (the
     * XDG layout); only <name> below them is private. */
    if (make_parents) {
        char local_dir[PATH_MAX];
        n = snprintf(local_dir, sizeof local_dir, "%s/.local", home);
        if (n < 0 || (size_t)n >= sizeof local_dir) return -1;
        if (hl_ensure_dir(local_dir, 0755) != 0 || hl_ensure_dir(local_state, 0755) != 0) {
            log_error("[state] cannot create %s: %s", local_state, strerror(errno));
            return -1;
        }
    }
    if (join(local_state, name, path, path_sz) != 0) {
        log_error("[state] state directory path is too long");
        return -1;
    }
    return 0;
}

int hl_state_dir_resolve(const char *name, char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;
    char path[PATH_MAX];
    if (state_dir_path(name, path, sizeof path, 1) != 0) return -1;
    return finish(path, out, out_sz);
}

int hl_state_dir_peek(const char *name, char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;
    out[0] = '\0';
    char path[PATH_MAX];
    if (state_dir_path(name, path, sizeof path, 0) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;   /* not made yet */
    return finish(path, out, out_sz) == 0 ? 1 : -1;
}
