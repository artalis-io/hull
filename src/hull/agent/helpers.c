/*
 * agent/helpers.c — Shared agent_lib helpers (error JSON + entry detection).
 *
 * Backend-agnostic, so it stays in the base even when SQLite is composed as a
 * feature (docs/sqlite_feature.md). The SQLite app-DB opener moved to
 * agent/db_open.c so it can travel into libhull_feature-sqlite.a.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include <sh_json.h>

#include <stdio.h>
#include <string.h>
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
