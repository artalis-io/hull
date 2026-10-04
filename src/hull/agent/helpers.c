/*
 * agent/helpers.c - Shared agent_lib helpers (error JSON + entry detection).
 *
 * Backend-agnostic, so it stays in the base even when SQLite is composed as a
 * feature (docs/sqlite_feature.md). The SQLite app-DB opener moved to
 * agent/db_open.c so it can travel into libhull_feature-sqlite.a.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include <sh_json.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int hl_agent_write_error(ShJsonBuf *out, const char *msg)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "error", msg);
    sh_json_write_object_end(&w);
    return -1;
}

const char *hl_agent_detect_entry(const char *app_dir, const char *ext,
                                  char *buf, size_t buf_size)
{
    size_t dir_len = strlen(app_dir);
    while (dir_len > 1 && app_dir[dir_len - 1] == '/')
        dir_len--;
    snprintf(buf, buf_size, "%.*s/app.%s", (int)dir_len, app_dir, ext);
    if (access(buf, F_OK) == 0) return buf;
    return NULL;
}

FILE *hl_agent_open_sidecar(const char *path)
{
    /* A sidecar under <app>/.hull is in a cloned repo's tree: never through a
     * link, never blocking on a planted FIFO, a regular file only. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f) close(fd);
    return f;
}

int hl_agent_text_is_terminal_safe(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7f)
            return 0;
        /* U+0080..U+009F (UTF-8 C2 80..C2 9F): C1 controls, CSI among them. */
        if (c == 0xc2 && i + 1 < len &&
            (unsigned char)buf[i + 1] >= 0x80 && (unsigned char)buf[i + 1] <= 0x9f)
            return 0;
    }
    return 1;
}
