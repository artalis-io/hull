/*
 * agent/db.c — `hull agent db schema`, `hull agent db query`,
 *              `hull agent migrate status` — all DB-side introspection.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_SQLITE

#include "internal.h"
#include "hull/app_context.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/tool.h"
#include "hull/migrate.h"
#include "hull/vfs.h"

#include <sh_json.h>
#include <sqlite3.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int db_schema_impl(sqlite3 *db, int close_db, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "tables");
    sh_json_write_array_start(&w);

    sqlite3_stmt *tables_stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name",
        -1, &tables_stmt, NULL);

    if (rc == SQLITE_OK) {
        while (sqlite3_step(tables_stmt) == SQLITE_ROW) {
            const char *table_name = (const char *)sqlite3_column_text(tables_stmt, 0);

            sh_json_write_object_start(&w);
            sh_json_write_kv_string(&w, "name", table_name);
            sh_json_write_key(&w, "columns");
            sh_json_write_array_start(&w);

            /* L-6: escape `"` in the table name (SQLite identifier
             * quoting doubles the quote character). Trust boundary is
             * the developer's own DB but a literal `"` in a table name
             * would otherwise produce a malformed PRAGMA. Also reject
             * pathological lengths so we never overflow the 512-byte
             * scratch buffer. */
            char quoted[256];
            size_t qi = 0;
            int truncated = 0;
            for (const char *p = table_name; p && *p; p++) {
                if (qi + 2 >= sizeof(quoted)) { truncated = 1; break; }
                quoted[qi++] = *p;
                if (*p == '"') {
                    if (qi + 1 >= sizeof(quoted)) { truncated = 1; break; }
                    quoted[qi++] = '"';
                }
            }
            quoted[qi] = '\0';
            if (truncated) {
                sh_json_write_array_end(&w);
                sh_json_write_object_end(&w);
                continue;
            }
            char pragma[512];
            snprintf(pragma, sizeof(pragma), "PRAGMA table_info(\"%s\")", quoted);

            sqlite3_stmt *col_stmt = NULL;
            int rc2 = sqlite3_prepare_v2(db, pragma, -1, &col_stmt, NULL);
            if (rc2 == SQLITE_OK) {
                while (sqlite3_step(col_stmt) == SQLITE_ROW) {
                    const char *col_name = (const char *)sqlite3_column_text(col_stmt, 1);
                    const char *col_type = (const char *)sqlite3_column_text(col_stmt, 2);
                    int notnull = sqlite3_column_int(col_stmt, 3);
                    int pk = sqlite3_column_int(col_stmt, 5);

                    sh_json_write_object_start(&w);
                    sh_json_write_kv_string(&w, "name", col_name);
                    sh_json_write_kv_string(&w, "type", col_type ? col_type : "");
                    if (notnull) sh_json_write_kv_bool(&w, "notnull", true);
                    if (pk) sh_json_write_kv_bool(&w, "pk", true);

                    if (sqlite3_column_type(col_stmt, 4) != SQLITE_NULL) {
                        const char *def = (const char *)sqlite3_column_text(col_stmt, 4);
                        sh_json_write_kv_string(&w, "default", def);
                    }

                    sh_json_write_object_end(&w);
                }
                sqlite3_finalize(col_stmt);
            }

            sh_json_write_array_end(&w);
            sh_json_write_object_end(&w);
        }
        sqlite3_finalize(tables_stmt);
    }

    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    if (close_db) sqlite3_close(db);
    return 0;
}

int hl_agent_db_schema_ctx(HlAppContext *ctx, const char *db_path, ShJsonBuf *out)
{
    if (db_path) {
        /* Explicit db_path: open a separate read-only connection */
        sqlite3 *db = hl_agent_open_app_db(hl_app_context_app_dir(ctx), db_path);
        if (!db)
            return hl_agent_write_error(out, "cannot open database");
        return db_schema_impl(db, 1, out);
    }
    /* No db_path: use the context's :memory: database */
    return db_schema_impl(hl_app_context_db(ctx), 0, out);
}

int hl_agent_db_schema(const char *app_dir, const char *db_path, ShJsonBuf *out)
{
    sqlite3 *db = hl_agent_open_app_db(app_dir, db_path);
    if (!db)
        return hl_agent_write_error(out, "cannot open database");
    return db_schema_impl(db, 1, out);
}

/* ── hl_agent_db_query ─────────────────────────────────────────────── */

static int db_query_impl(sqlite3 *db, int close_db, const char *sql,
                          ShJsonBuf *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        int ret = hl_agent_write_error(out, sqlite3_errmsg(db));
        if (close_db) sqlite3_close(db);
        return ret;
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    /* Column names */
    int ncols = sqlite3_column_count(stmt);
    sh_json_write_key(&w, "columns");
    sh_json_write_array_start(&w);
    for (int i = 0; i < ncols; i++)
        sh_json_write_string(&w, sqlite3_column_name(stmt, i));
    sh_json_write_array_end(&w);

    /* Rows */
    sh_json_write_key(&w, "rows");
    sh_json_write_array_start(&w);

    int row_count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sh_json_write_array_start(&w);
        for (int i = 0; i < ncols; i++) {
            switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:
                sh_json_write_null(&w);
                break;
            case SQLITE_INTEGER:
                sh_json_write_int(&w, sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                sh_json_write_double(&w, sqlite3_column_double(stmt, i));
                break;
            case SQLITE_TEXT:
                sh_json_write_string(&w, (const char *)sqlite3_column_text(stmt, i));
                break;
            case SQLITE_BLOB: {
                char blob_str[64];
                snprintf(blob_str, sizeof(blob_str), "<blob:%d>",
                         sqlite3_column_bytes(stmt, i));
                sh_json_write_string(&w, blob_str);
                break;
            }
            }
        }
        sh_json_write_array_end(&w);
        row_count++;
    }

    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "count", row_count);

    if (rc != SQLITE_DONE)
        sh_json_write_kv_string(&w, "error", sqlite3_errmsg(db));

    sh_json_write_object_end(&w);
    sqlite3_finalize(stmt);
    if (close_db) sqlite3_close(db);
    return 0;
}

int hl_agent_db_query_ctx(HlAppContext *ctx, const char *db_path,
                          const char *sql, ShJsonBuf *out)
{
    if (!sql)
        return hl_agent_write_error(out, "SQL argument required");

    if (db_path) {
        sqlite3 *db = hl_agent_open_app_db(hl_app_context_app_dir(ctx), db_path);
        if (!db)
            return hl_agent_write_error(out, "cannot open database");
        return db_query_impl(db, 1, sql, out);
    }
    return db_query_impl(hl_app_context_db(ctx), 0, sql, out);
}

int hl_agent_db_query(const char *app_dir, const char *db_path,
                      const char *sql, ShJsonBuf *out)
{
    if (!sql)
        return hl_agent_write_error(out, "SQL argument required");

    sqlite3 *db = hl_agent_open_app_db(app_dir, db_path);
    if (!db)
        return hl_agent_write_error(out, "cannot open database");
    return db_query_impl(db, 1, sql, out);
}

/* ── hl_agent_request ──────────────────────────────────────────────── */

static int migrate_status_impl(sqlite3 *db, int close_db, const HlVfs *vfs,
                                ShJsonBuf *out)
{
    HlMigrationStatus *entries = NULL;
    int count = 0;
    HlDbHandle tmp_handle = {0};
    int wrap_ok = (hl_db_sqlite_wrap(&tmp_handle, db) == 0);
    int rc = wrap_ok ? hl_migrate_status(&tmp_handle, vfs, &entries, &count) : -1;
    if (wrap_ok) hl_db_sqlite_unwrap(&tmp_handle);
    if (rc != 0) {
        if (close_db) sqlite3_close(db);
        return hl_agent_write_error(out, "failed to query migration status");
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    sh_json_write_key(&w, "applied");
    sh_json_write_array_start(&w);
    for (int i = 0; i < count; i++) {
        if (entries[i].applied)
            sh_json_write_string(&w, entries[i].name);
    }
    sh_json_write_array_end(&w);

    sh_json_write_key(&w, "pending");
    sh_json_write_array_start(&w);
    for (int i = 0; i < count; i++) {
        if (!entries[i].applied)
            sh_json_write_string(&w, entries[i].name);
    }
    sh_json_write_array_end(&w);

    sh_json_write_kv_int(&w, "total", count);
    sh_json_write_object_end(&w);

    hl_migrate_status_free(entries, count);
    if (close_db) sqlite3_close(db);
    return 0;
}

int hl_agent_migrate_status_ctx(HlAppContext *ctx, const char *db_path,
                                ShJsonBuf *out)
{
    if (db_path) {
        sqlite3 *db = hl_agent_open_app_db(hl_app_context_app_dir(ctx), db_path);
        if (!db)
            return hl_agent_write_error(out, "cannot open database");
        return migrate_status_impl(db, 1, hl_app_context_app_vfs(ctx), out);
    }
    return migrate_status_impl(hl_app_context_db(ctx), 0,
                               hl_app_context_app_vfs(ctx), out);
}

int hl_agent_migrate_status(const char *app_dir, const char *db_path,
                            ShJsonBuf *out)
{
    sqlite3 *db = hl_agent_open_app_db(app_dir, db_path);
    if (!db)
        return hl_agent_write_error(out, "cannot open database");

    extern const HlEntry hl_app_entries[];
    HlVfs app_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);

    return migrate_status_impl(db, 1, &app_vfs, out);
}

/* (Removed: an earlier draft had a `count_files_in_dir` helper here.
 * The current `hull agent deploy` path uses `hl_tool_find_files`
 * directly in `agent/deploy.c` and doesn't need it. Triggered
 * -Wunused-function under the static-analysis CI job.) */

#endif /* HL_ENABLE_SQLITE */
