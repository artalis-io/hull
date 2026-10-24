/*
 * fs_util.c - shared directory-creation helpers (see fs_util.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/fs_util.h"

#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

int hl_ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0) return 0;

    /* mkdir failed. On POSIX an already-existing directory yields EEXIST, but
     * some platforms report a pre-existing path with a DIFFERENT errno: notably
     * Cosmopolitan on Windows returns EIO (errno 5) for a drive root ("/C",
     * "/D") - which every absolute Windows path starts with - and can return
     * EACCES for some existing system directories. So do NOT special-case
     * EEXIST: if the path already IS a directory, treat it as created regardless
     * of the mkdir errno. Without this, hl_mkdir_p fails on the very first
     * (drive-root) component of any absolute path on Windows, which broke
     * hull/blob@1 there for ALL paths (nexogen PLATFORM_GAPS 2026-07-14, which
     * mis-attributed it to spaces in the path). A real failure - the path exists
     * but is not a directory, or is unreadable - still returns -1 below. */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (errno == 0) errno = ENOTDIR;
    return -1;
}

int hl_mkdir_p(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, len + 1);

    /* Create each "/"-terminated prefix, then the whole path. Starts at index 1
     * so a leading "/" (absolute path root) is skipped. */
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (hl_ensure_dir(buf, mode) != 0) return -1;
            buf[i] = saved;
        }
    }
    return 0;
}

int hl_default_db_path_check(const char *path, char *err, size_t err_sz)
{
    static const char *const suffixes[] = { "", "-wal", "-shm", "-journal" };
    if (err && err_sz) err[0] = '\0';
    if (!path || !*path) {
        if (err && err_sz) snprintf(err, err_sz, "no database path");
        return -1;
    }
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char p[PATH_MAX];
        int n = snprintf(p, sizeof p, "%s%s", path, suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof p) {
            if (err && err_sz) snprintf(err, err_sz, "database path too long");
            return -1;
        }
        struct stat st;
        if (lstat(p, &st) != 0) {
            if (errno == ENOENT) continue;   /* absent: SQLite creates it */
            if (err && err_sz)
                snprintf(err, err_sz, "cannot inspect %s: %s", p,
                         strerror(errno));
            return -1;
        }
        if (S_ISLNK(st.st_mode)) {
            if (err && err_sz)
                snprintf(err, err_sz, "%s is a symbolic link; the default "
                         "database must be the app's own file (pass -d to "
                         "name another database explicitly)", p);
            return -1;
        }
        if (!S_ISREG(st.st_mode)) {
            if (err && err_sz)
                snprintf(err, err_sz, "%s is not a regular file", p);
            return -1;
        }
    }
    return 0;
}
