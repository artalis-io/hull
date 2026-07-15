/*
 * agent/helpers.c — Shared agent_lib helpers (DB opener + error JSON).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/app_context.h"
#include "hull/entry.h"
#include "hull/vfs.h"

#ifdef HL_ENABLE_SQLITE
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"   /* hl_db_sqlite_wrap/_unwrap */
#include "hull/migrate.h"
#include <sqlite3.h>
#endif

#include <sh_json.h>

#include <limits.h>
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

#ifdef HL_ENABLE_SQLITE
sqlite3 *hl_agent_open_app_db(const char *app_dir, const char *db_path)
{
    sqlite3 *db = NULL;
    char default_path[PATH_MAX];

    if (!db_path) {
        snprintf(default_path, sizeof(default_path), "%s/data.db", app_dir);
        if (access(default_path, F_OK) == 0)
            db_path = default_path;
    }

    if (db_path) {
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return NULL;
        }
    } else {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK)
            return NULL;
        hl_cap_db_init(db);

        extern const HlEntry hl_app_entries[];
        HlVfs app_vfs;
        hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
        HlDbHandle tmp_handle;
        if (hl_db_sqlite_wrap(&tmp_handle, db) == 0) {
            hl_migrate_run(&tmp_handle, &app_vfs);
            hl_db_sqlite_unwrap(&tmp_handle);
        }
    }

    return db;
}
#endif /* HL_ENABLE_SQLITE */

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
