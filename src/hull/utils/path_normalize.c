/*
 * path_normalize.c - shared normalize_path implementation.
 *
 * Lifted out of runtime/lua/mod_fs.c so the JS module loader can use
 * the same logic. See path_normalize.h for the contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/utils/path_normalize.h"

#include <string.h>

int hl_path_normalize(char *path)
{
    if (!path) return -1;

    /* Split into segments, process left-to-right */
    char *segments[128];
    int depth = 0;
    int absolute = (path[0] == '/');

    char *p = path;
    while (*p) {
        /* Skip slashes */
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        /* Find end of segment */
        char *seg = p;
        while (*p && *p != '/')
            p++;
        if (*p == '/') {
            *p = '\0';
            p++;
        }

        if (strcmp(seg, ".") == 0) {
            continue; /* skip */
        } else if (strcmp(seg, "..") == 0) {
            if (depth > 0)
                depth--;
            else
                return -1; /* escapes past root */
        } else {
            if (depth >= 128)
                return -1;
            segments[depth++] = seg;
        }
    }

    /* Rebuild path */
    char *out = path;
    if (absolute)
        *out++ = '/';
    for (int i = 0; i < depth; i++) {
        if (i > 0)
            *out++ = '/';
        size_t len = strlen(segments[i]);
        memmove(out, segments[i], len);
        out += len;
    }
    *out = '\0';

    return 0;
}
