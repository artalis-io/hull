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

int hl_ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        if (errno == 0) errno = ENOTDIR;
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
