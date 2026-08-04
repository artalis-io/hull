/*
 * fs_util.c — shared directory-creation helpers (see fs_util.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/fs_util.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>    /* TEMP DIAG */
#include <stdlib.h>   /* TEMP DIAG (getenv) */

int hl_ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0) return 0;
    int mkerr = errno;
    if (mkerr == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        /* TEMP DIAG: EEXIST but stat failed / not a dir (Windows spaces gap). */
        if (getenv("HULL_FS_DEBUG"))
            fprintf(stderr, "[FSDBG] ensure_dir '%s': EEXIST but stat-fail/not-dir (errno=%d)\n",
                    path, errno);
        if (errno == 0) errno = ENOTDIR;
    } else if (getenv("HULL_FS_DEBUG")) {
        /* TEMP DIAG: mkdir failed with something other than EEXIST. */
        fprintf(stderr, "[FSDBG] ensure_dir '%s': mkdir FAILED errno=%d\n", path, mkerr);
    }
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
