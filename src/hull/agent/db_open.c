/*
 * agent/db_open.c - SQLite app-DB opener for agent introspection.
 *
 * Split out of agent/helpers.c so it moves into libhull_feature-sqlite.a with
 * the rest of the SQLite agent code (SQLite as a composable feature,
 * docs/sqlite_feature.md). hl_agent_open_app_db is the raw sqlite3
 * opener the SQLite agent impls (agent/db.c, sql.c, schema_diff.c) call; keeping
 * it here lets a SQLite-less base drop it (the composed feature supplies it)
 * while helpers.c retains the backend-agnostic hl_agent_write_error /
 * hl_agent_detect_entry.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_SQLITE

#include "internal.h"
#include "hull/app_context.h"
#include "hull/entry.h"
#include "hull/vfs.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"   /* hl_db_sqlite_wrap/_unwrap */
#include "hull/migrate.h"
#include <sqlite3.h>

#include <limits.h>
#include <stdio.h>
#include <unistd.h>

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
