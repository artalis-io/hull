/*
 * cap/exec_child.c - a declared command's child manifest: which app fs
 * grants a command may take, and the exact kernel path each one names.
 * See hull/cap/exec_child.h and docs/design/exec_broker.md §3.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/exec_child.h"
#include "hull/cap/exec.h"
#include "hull/cap/fs_policy.h"
#include "hull/shared/host.h"     /* hl_host_path_too_broad */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const HL_EXEC_CHILD_MANIFEST_KEYS[] = { "fs", NULL };

const char *const HL_EXEC_APP_ONLY_KEYS[] = {
    "modules", "env", "secrets", "hosts", "http", "csp", "cors", "databases",
    "kv", "ssh", "state", "tui", "exec", "compute", "gpu", "wasm", "limits",
    "allow_dynamic_code", "allowDynamicCode",
    "allow_dynamic_libraries", "allowDynamicLibraries", NULL };

const char *const HL_EXEC_ENTRY_ONLY_KEYS[] = {
    "path", "argv", "sha256", "host", "manifest", "timeout_ms", "timeoutMs",
    "stdout_max", "stdoutMax", "stderr_max", "stderrMax", "concurrency",
    "stdin_max", "stdinMax", "unix_sockets", "unixSockets", NULL };

static int key_in(const char *k, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (strcmp(k, list[i]) == 0) return 1;
    return 0;
}

int hl_exec_child_key(HlExecBuf *b, const char *cmd, const char *key)
{
    if (key_in(key, HL_EXEC_CHILD_MANIFEST_KEYS)) return 0;
    if (strcmp(key, "env") == 0)
        hl_exec_b_fail(b, "exec.%s.manifest.env: an app's env allowlist is not "
                       "something a command can be given (a command gets only "
                       "the literal values of exec.%s.env)", cmd, cmd);
    else if (key_in(key, HL_EXEC_ENTRY_ONLY_KEYS))
        hl_exec_b_fail(b, "exec.%s.manifest.%s: not a manifest field; it belongs on "
                       "the command entry (exec.%s.%s)", cmd, key, cmd, key);
    else if (key_in(key, HL_EXEC_APP_ONLY_KEYS))
        hl_exec_b_fail(b, "exec.%s.manifest.%s: an app capability a command cannot "
                       "be given (a command's manifest takes only: fs)", cmd, key);
    else
        hl_exec_b_fail(b, "exec.%s.manifest: unknown key '%s'", cmd, key);
    return -1;
}

int hl_exec_child_grant_entry(HlExecBuf *b, const char *cmd, const char *list,
                              unsigned idx, const char *raw, size_t n)
{
    char why[160];
    if (hl_exec_child_grant_check(raw, n, why, sizeof why) == 0) return 0;
    hl_exec_b_fail(b, "exec.%s.manifest.fs.%s[%u] '%.*s' %s", cmd, list, idx,
                   (int)(n > 64 ? 64 : n), raw, why);
    return -1;
}

void hl_exec_legacy_note(const char *cmd, const char *legacy, const char *replacement)
{
    static int warned;
    if (warned) return;
    warned = 1;
    log_warn("[manifest] exec.%s.%s is the v0.26.0 spelling, taken as exec.%s.%s; "
             "it is removed in 0.28.0 (`hull check` lists every use)",
             cmd, legacy, cmd, replacement);
}

int hl_exec_child_grant_check(const char *raw, size_t n, char *why, size_t why_size)
{
    HlFsGrant g = HL_FS_GRANT_INIT;
    const char *e = NULL;
    if (hl_fs_grant_parse(raw, n, NULL, &g, &e) != 0) {
        if (e && strcmp(e, "reserved_path") == 0)
            snprintf(why, why_size, "is reserved (.hull, or an '@' root other "
                     "than @state)");
        else if (e && strcmp(e, "unsupported_pattern") == 0)
            snprintf(why, why_size, "uses a pattern an fs grant does not take");
        else
            snprintf(why, why_size, "is not an fs grant: a path under the app "
                     "directory or \"@state/\", with no '..'");
        return -1;
    }
    int rc = 0;
    if (g.n == 0) {
        snprintf(why, why_size, "grants the whole %s directory, which a command "
                 "cannot be given: its .hull would be included (name a "
                 "subdirectory)", g.state ? "state" : "app");
        rc = -1;
    } else if (g.first_pattern < g.n) {
        snprintf(why, why_size, "is a pattern, which a command cannot be given "
                 "(the kernel grants files and directories; name one)");
        rc = -1;
    }
    hl_fs_grant_free(&g);
    return rc;
}

/* ── Opening a grant ───────────────────────────────────────────────── */

/* A descriptor for walking and for the Landlock rule: Linux O_PATH needs no
 * read permission; macOS has no O_PATH (O_EVTONLY opens a file for events
 * only). */
#ifdef O_PATH
#define HX_DIR_FLAGS  (O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#define HX_FILE_FLAGS (O_PATH | O_NOFOLLOW | O_CLOEXEC)
#elif defined(O_EVTONLY)
#define HX_DIR_FLAGS  (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#define HX_FILE_FLAGS (O_EVTONLY | O_NOFOLLOW | O_CLOEXEC)
#else
#define HX_DIR_FLAGS  (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#define HX_FILE_FLAGS (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
#endif

typedef struct { dev_t dev; ino_t ino; int set; } Id;

static int id_eq(const Id *a, const struct stat *st)
{
    return a->set && a->dev == st->st_dev && a->ino == st->st_ino;
}

/* Is the directory @p dir the same as, or an ancestor of, @p target? Walks up
 * from @p target by "..", comparing inodes, so no name alias (a case
 * variant, an HFS+ ignorable spelling, a nested root) can hide it. */
static int dir_contains(const struct stat *dir, const char *target)
{
    int fd = open(target, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return 0;
    int hit = 0;
    for (int depth = 0; depth < 256; depth++) {
        struct stat st;
        if (fstat(fd, &st) != 0) break;
        if (st.st_dev == dir->st_dev && st.st_ino == dir->st_ino) { hit = 1; break; }
        int up = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        struct stat ust;
        if (up < 0 || fstat(up, &ust) != 0 ||
            (ust.st_dev == st.st_dev && ust.st_ino == st.st_ino)) {
            if (up >= 0) close(up);
            break;                                    /* reached "/" */
        }
        close(fd);
        fd = up;
    }
    close(fd);
    return hit;
}

/* The .hull of a root, by inode (unset when it does not exist). */
static void hull_id(const char *root, Id *out)
{
    out->set = 0;
    if (!root || !root[0]) return;
    char p[PATH_MAX];
    struct stat st;
    if (snprintf(p, sizeof p, "%s/.hull", root) < (int)sizeof p && lstat(p, &st) == 0) {
        out->dev = st.st_dev;
        out->ino = st.st_ino;
        out->set = 1;
    }
}

/* A root directory, by inode (unset when absent). */
static void dir_id(const char *root, Id *out)
{
    struct stat st;
    out->set = 0;
    if (root && root[0] && stat(root, &st) == 0) {
        out->dev = st.st_dev;
        out->ino = st.st_ino;
        out->set = 1;
    }
}

static int same_file(const struct stat *a, const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && st.st_dev == a->st_dev && st.st_ino == a->st_ino;
}

/* A write grant may not reach a protected database or its sidecars: a
 * command truncating the live -shm kills the app with SIGBUS. The app's own
 * fs layer refuses such a write (`database_file_in_use`); a command has no
 * such layer. */
static int reaches_db(const struct stat *g, const HlExecChildRoots *r, char *why,
                      size_t why_size)
{
    static const char *const SIDE[] = { "", "-wal", "-shm", "-journal" };
    for (int i = 0; r->protect && i < r->n_protect; i++) {
        const char *db = r->protect[i];
        if (!db || !db[0]) continue;
        /* The server sends each file's real path (hl_db_registry_file_paths):
         * a symbolic link or a relative -d is resolved there, against the
         * server's own working directory. A relative one here (a test) is
         * taken against the app directory. */
        char abs[PATH_MAX];
        if (db[0] == '/') snprintf(abs, sizeof abs, "%s", db);
        else if (snprintf(abs, sizeof abs, "%s/%s", r->app_dir ? r->app_dir : ".", db)
                 >= (int)sizeof abs) continue;
        if (S_ISDIR(g->st_mode)) {
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s", abs);
            char *sl = strrchr(dir, '/');
            if (sl == dir) dir[1] = '\0';
            else if (sl) *sl = '\0';
            if (dir_contains(g, dir)) {
                snprintf(why, why_size, "contains the app's database (%s): a command "
                         "may not write it or its -wal / -shm / -journal", db);
                return 1;
            }
            continue;
        }
        for (int k = 0; k < 4; k++) {
            char side[PATH_MAX + 16];
            snprintf(side, sizeof side, "%s%s", abs, SIDE[k]);
            if (same_file(g, side)) {
                snprintf(why, why_size, "is the app's database%s (%s): a command may "
                         "not write it", k ? " sidecar" : "", db);
                return 1;
            }
        }
    }
    return 0;
}

int hl_exec_child_grant_open(const HlExecChildRoots *r, const char *raw, int write,
                             HlExecChildGrant *out, char *why, size_t why_size)
{
    out->fd = -1;
    out->path[0] = '\0';
    size_t rn = strlen(raw);
    if (hl_exec_child_grant_check(raw, rn, why, why_size) != 0) return -1;
    HlFsGrant g = HL_FS_GRANT_INIT;
    const char *e = NULL;
    if (hl_fs_grant_parse(raw, rn, NULL, &g, &e) != 0) {
        snprintf(why, why_size, "is not an fs grant");
        return -1;
    }
    int rc = -1, fd = -1;
    const char *root = g.state ? r->state_dir : r->app_dir;
    char rroot[PATH_MAX], path[PATH_MAX];
    struct stat st;
    if (!root || !root[0]) {
        snprintf(why, why_size, g.state ? "names @state/, but the app declares no "
                 "state directory" : "has no app directory to resolve against");
        goto done;
    }
    if (!realpath(root, rroot)) {
        snprintf(why, why_size, "cannot resolve its root %s: %s", root, strerror(errno));
        goto done;
    }
    /* "/" or the home is not an app directory (a built tool run from there):
     * a relative grant would name the user's own files. Refused before
     * anything is created. */
    if (!g.state && (strcmp(rroot, "/") == 0 || hl_host_path_too_broad(rroot))) {
        snprintf(why, why_size, "would be resolved against %s, which is not an app "
                 "directory (/ or the home); a tool run from anywhere uses "
                 "\"@state/\"", rroot);
        goto done;
    }
    Id hull_app, hull_state, root_app, root_state;
    hull_id(r->app_dir, &hull_app);
    hull_id(r->state_dir, &hull_state);
    dir_id(r->app_dir, &root_app);
    dir_id(r->state_dir, &root_state);

    fd = open(rroot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0) {
        snprintf(why, why_size, "cannot open its root %s: %s", rroot, strerror(errno));
        goto done;
    }
    size_t at = (size_t)snprintf(path, sizeof path, "%s", strcmp(rroot, "/") ? rroot : "");
    /* Component by component from the root's descriptor: each checked with
     * fstatat(NOFOLLOW), opened NOFOLLOW, and the opened object must be the
     * one checked. Nothing is looked up by path afterwards: the caller grants
     * the final descriptor, so a component swapped for a symbolic link cannot
     * redirect the grant. */
    for (size_t i = 0; i < g.n; i++) {
        const char *c = g.comp[i];
        int last = i + 1 == g.n, made = 0;
        struct stat pst;
        int at_root = fstat(fd, &pst) == 0 &&
                      (id_eq(&root_app, &pst) || id_eq(&root_state, &pst));
        int w = snprintf(path + at, sizeof path - at, "/%s", c);
        if (w < 0 || (size_t)w >= sizeof path - at) {
            snprintf(why, why_size, "is too long");
            goto done;
        }
        at += (size_t)w;
        if (fstatat(fd, c, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT || !g.directory_intent) {
                snprintf(why, why_size, "does not exist; a command's grant names an "
                         "existing file or directory (end it with '/' for a "
                         "directory to create)");
                goto done;
            }
            /* A "dir/" grant: created, as the app's sandbox creates it, then
             * checked like any other component (it may have been replaced). */
            if (mkdirat(fd, c, 0755) != 0) {
                if (errno != EEXIST) {
                    snprintf(why, why_size, "cannot be created: %s", strerror(errno));
                    goto done;
                }
            } else {
                made = 1;
            }
            if (fstatat(fd, c, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                snprintf(why, why_size, "vanished while it was created");
                goto done;
            }
        }
        if (S_ISLNK(st.st_mode)) {
            snprintf(why, why_size, "runs through a symbolic link");
            goto done;
        }
        /* Directly under a root, compare with that root's .hull as it is
         * NOW, after any mkdirat: on HFS+ a name with an ignorable code point
         * IS ".hull", so the directory just made may be the root's .hull
         * (unset at load if it did not exist). A directory made here that
         * turns out to be it is removed again. */
        struct stat hst;
        int is_hull = id_eq(&hull_app, &st) || id_eq(&hull_state, &st) ||
                      (at_root && fstatat(fd, ".hull", &hst, AT_SYMLINK_NOFOLLOW) == 0 &&
                       hst.st_dev == st.st_dev && hst.st_ino == st.st_ino);
        if (is_hull) {
            if (made) (void)unlinkat(fd, c, AT_REMOVEDIR);
            snprintf(why, why_size, "reaches a .hull directory, which no command "
                     "may be given");
            goto done;
        }
        if (!last && !S_ISDIR(st.st_mode)) {
            snprintf(why, why_size, "runs through a non-directory");
            goto done;
        }
        if (last && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            snprintf(why, why_size, "is neither a regular file nor a directory");
            goto done;
        }
        if (last && g.directory_intent && !S_ISDIR(st.st_mode)) {
            snprintf(why, why_size, "ends in '/' but is not a directory");
            goto done;
        }
        int nfd = openat(fd, c, S_ISDIR(st.st_mode) ? HX_DIR_FLAGS : HX_FILE_FLAGS);
        struct stat ost;
        if (nfd < 0 || fstat(nfd, &ost) != 0 ||
            ost.st_dev != st.st_dev || ost.st_ino != st.st_ino) {
            if (nfd >= 0) close(nfd);
            snprintf(why, why_size, "changed while it was opened");
            goto done;
        }
        close(fd);
        fd = nfd;
    }
    /* A directory grant may not be either root, or an ancestor of one: that
     * root's .hull would be under it (a state directory inside the app
     * directory, or the reverse). */
    if (S_ISDIR(st.st_mode)) {
        const char *roots[2] = { r->app_dir, r->state_dir };
        for (int k = 0; k < 2; k++) {
            if (roots[k] && roots[k][0] && dir_contains(&st, roots[k])) {
                snprintf(why, why_size, "holds the %s directory, whose .hull no "
                         "command may be given", k ? "state" : "app");
                goto done;
            }
        }
    }
    if (write && reaches_db(&st, r, why, why_size)) goto done;
#ifdef F_GETPATH
    {
        char real[PATH_MAX];
        if (fcntl(fd, F_GETPATH, real) == 0) snprintf(path, sizeof path, "%s", real);
    }
#endif
    out->fd = fd;
    fd = -1;
    snprintf(out->path, sizeof out->path, "%s", path);
    rc = 0;
done:
    if (fd >= 0) close(fd);
    hl_fs_grant_free(&g);
    return rc;
}

int hl_exec_child_grant_resolve(const char *app_dir, const char *state_dir,
                                const char *raw, char *out, size_t out_size,
                                char *why, size_t why_size)
{
    HlExecChildRoots r = { app_dir, state_dir, NULL, 0 };
    HlExecChildGrant gr;
    if (hl_exec_child_grant_open(&r, raw, 0, &gr, why, why_size) != 0) return -1;
    close(gr.fd);
    if (strlen(gr.path) >= out_size) { snprintf(why, why_size, "is too long"); return -1; }
    memcpy(out, gr.path, strlen(gr.path) + 1);
    return 0;
}
