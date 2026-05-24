/*
 * agent/schema_diff.c — `hull agent schema-diff`: compare live DB
 * schema against what the migrations would produce.
 *
 * Reports schema drift in three buckets:
 *   - applied:    migrations recorded in _hull_migrations
 *   - pending:    migration files not yet applied
 *   - drift:      tables/indexes in the DB that aren't created by any migration
 *
 * Best-effort comparison: we tokenise migration SQL for "CREATE TABLE"
 * / "CREATE INDEX" statements and check if each name shows up in
 * sqlite_master. False positives possible if a migration drops then
 * recreates an object with a different name; those are reported as
 * drift and the developer can disregard.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/entry.h"
#include "hull/migrate.h"
#include "hull/vfs.h"

#include <sh_json.h>
#include <sqlite3.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Collect "CREATE TABLE / INDEX <name>" identifiers from a SQL chunk. */
typedef struct {
    char **names;
    int    count;
    int    cap;
} NameList;

static void name_list_push(NameList *l, const char *name)
{
    if (!name || !*name) return;
    /* Dedup */
    for (int i = 0; i < l->count; i++)
        if (strcmp(l->names[i], name) == 0) return;
    if (l->count >= l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        char **g = realloc(l->names, (size_t)ncap * sizeof(char *));
        if (!g) return;
        l->names = g;
        l->cap = ncap;
    }
    /* L-1 fix: don't increment count if strdup returned NULL — otherwise
     * downstream name_present / emit_list would crash on the slot. */
    char *dup = strdup(name);
    if (!dup) return;
    l->names[l->count++] = dup;
}

static void name_list_free(NameList *l)
{
    for (int i = 0; i < l->count; i++) free(l->names[i]);
    free(l->names);
    l->names = NULL; l->count = l->cap = 0;
}

/* Walk over SQL text, find every `CREATE TABLE [IF NOT EXISTS] <name>`
 * and `CREATE INDEX [...] <name>`. Identifier may be quoted or bare.
 *
 * H-1 fix: `sql` is the .data pointer from an HlEntry which is NOT
 * NUL-terminated (the registry stores raw bytes + a separate `len`).
 * Walk against `sql + sql_len` rather than against `*p`. */
static void collect_creates(const char *sql, size_t sql_len,
                             NameList *tables, NameList *indexes)
{
    const char *p = sql;
    const char *end = sql + sql_len;
    while (p < end) {
        /* Skip whitespace/comments crudely */
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p + 1 < end && p[0] == '-' && p[1] == '-') {
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < end) p += 2;
            continue;
        }
        if (end - p >= 6 && strncasecmp(p, "CREATE", 6) == 0) {
            const char *cursor = p + 6;
            while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
            int is_table = (end - cursor >= 5) && strncasecmp(cursor, "TABLE",  5) == 0;
            int is_index = (end - cursor >= 5) && strncasecmp(cursor, "INDEX",  5) == 0;
            int is_unique_index = (end - cursor >= 6) && strncasecmp(cursor, "UNIQUE", 6) == 0;
            if (is_unique_index) {
                cursor += 6;
                while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
                if ((end - cursor >= 5) && strncasecmp(cursor, "INDEX", 5) == 0) is_index = 1;
            }
            if (is_table) cursor += 5;
            else if (is_index) cursor += 5;
            else { p++; continue; }

            while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
            if ((end - cursor >= 2) && strncasecmp(cursor, "IF", 2) == 0) {
                while (cursor < end && !isspace((unsigned char)*cursor)) cursor++;
                while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
                if ((end - cursor >= 3) && strncasecmp(cursor, "NOT", 3) == 0) {
                    while (cursor < end && !isspace((unsigned char)*cursor)) cursor++;
                    while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
                    if ((end - cursor >= 6) && strncasecmp(cursor, "EXISTS", 6) == 0) {
                        cursor += 6;
                        while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
                    }
                }
            }
            /* Now parse name: identifier or "quoted" or `backticked`. */
            char name[HL_AGENT_IDENT_MED];
            int ni = 0;
            char quote = 0;
            if (cursor < end && (*cursor == '"' || *cursor == '`' || *cursor == '\'')) {
                quote = *cursor++;
            }
            while (cursor < end && ni < (int)sizeof(name) - 1) {
                if (quote) {
                    if (*cursor == quote) { cursor++; break; }
                } else {
                    if (isspace((unsigned char)*cursor) ||
                        *cursor == '(' || *cursor == ';') break;
                }
                name[ni++] = *cursor++;
            }
            name[ni] = '\0';
            if (ni > 0) {
                if (is_table) name_list_push(tables, name);
                else          name_list_push(indexes, name);
            }
            p = cursor;
            continue;
        }
        p++;
    }
}

static void emit_list(ShJsonWriter *w, const char *key, const NameList *l)
{
    sh_json_write_key(w, key);
    sh_json_write_array_start(w);
    for (int i = 0; i < l->count; i++)
        sh_json_write_string(w, l->names[i]);
    sh_json_write_array_end(w);
}

/* Find a name in a NameList; returns 1 if present. */
static int name_present(const NameList *l, const char *name)
{
    for (int i = 0; i < l->count; i++)
        if (strcmp(l->names[i], name) == 0) return 1;
    return 0;
}

static int schema_diff_impl(sqlite3 *db, const HlVfs *vfs, ShJsonBuf *out)
{
    /* Step 1: collect expected objects from migration files */
    NameList expected_tables = {0}, expected_indexes = {0};
    const HlEntry *first = NULL;
    int mig_count = hl_vfs_prefix(vfs, "migrations/", &first);
    for (int i = 0; i < mig_count; i++) {
        /* HlEntry::data is NOT NUL-terminated; use the entry's len. */
        collect_creates((const char *)first[i].data, first[i].len,
                        &expected_tables, &expected_indexes);
    }

    /* Step 2: collect actual objects from sqlite_master */
    NameList actual_tables = {0}, actual_indexes = {0};
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT type, name FROM sqlite_master "
            "WHERE name NOT LIKE 'sqlite_%' AND name NOT LIKE '_hull_%'",
            -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *type = (const char *)sqlite3_column_text(stmt, 0);
            const char *name = (const char *)sqlite3_column_text(stmt, 1);
            if (!type || !name) continue;
            if (strcmp(type, "table") == 0) name_list_push(&actual_tables, name);
            else if (strcmp(type, "index") == 0) name_list_push(&actual_indexes, name);
        }
        sqlite3_finalize(stmt);
    }

    /* Step 3: write diff */
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    emit_list(&w, "expected_tables",  &expected_tables);
    emit_list(&w, "actual_tables",    &actual_tables);
    emit_list(&w, "expected_indexes", &expected_indexes);
    emit_list(&w, "actual_indexes",   &actual_indexes);

    /* Drift = in actual but not expected. */
    sh_json_write_key(&w, "drift_tables");
    sh_json_write_array_start(&w);
    int drift_tables = 0;
    for (int i = 0; i < actual_tables.count; i++) {
        if (!name_present(&expected_tables, actual_tables.names[i])) {
            sh_json_write_string(&w, actual_tables.names[i]);
            drift_tables++;
        }
    }
    sh_json_write_array_end(&w);

    sh_json_write_key(&w, "drift_indexes");
    sh_json_write_array_start(&w);
    int drift_indexes = 0;
    for (int i = 0; i < actual_indexes.count; i++) {
        /* SQLite auto-creates an index named sqlite_autoindex_* for
         * PRIMARY KEY / UNIQUE constraints — those are not drift. */
        if (strncmp(actual_indexes.names[i], "sqlite_", 7) == 0) continue;
        if (!name_present(&expected_indexes, actual_indexes.names[i])) {
            sh_json_write_string(&w, actual_indexes.names[i]);
            drift_indexes++;
        }
    }
    sh_json_write_array_end(&w);

    /* Missing = in expected but not actual (i.e. migration not applied). */
    sh_json_write_key(&w, "missing_tables");
    sh_json_write_array_start(&w);
    int missing_tables = 0;
    for (int i = 0; i < expected_tables.count; i++) {
        if (!name_present(&actual_tables, expected_tables.names[i])) {
            sh_json_write_string(&w, expected_tables.names[i]);
            missing_tables++;
        }
    }
    sh_json_write_array_end(&w);

    sh_json_write_kv_int(&w, "drift_table_count",    drift_tables);
    sh_json_write_kv_int(&w, "drift_index_count",    drift_indexes);
    sh_json_write_kv_int(&w, "missing_table_count",  missing_tables);
    sh_json_write_kv_bool(&w, "in_sync", (drift_tables == 0 &&
                                           drift_indexes == 0 &&
                                           missing_tables == 0));

    sh_json_write_object_end(&w);

    name_list_free(&expected_tables);
    name_list_free(&expected_indexes);
    name_list_free(&actual_tables);
    name_list_free(&actual_indexes);
    return 0;
}

int hl_agent_schema_diff_ctx(HlAppContext *ctx, const char *db_path,
                              ShJsonBuf *out)
{
    const HlVfs *vfs = hl_app_context_app_vfs(ctx);
    if (!vfs) return hl_agent_write_error(out, "no vfs");

    if (db_path) {
        sqlite3 *db = hl_agent_open_app_db(hl_app_context_app_dir(ctx), db_path);
        if (!db) return hl_agent_write_error(out, "cannot open database");
        int rc = schema_diff_impl(db, vfs, out);
        sqlite3_close(db);
        return rc;
    }
    return schema_diff_impl(hl_app_context_db(ctx), vfs, out);
}

int hl_agent_schema_diff(const char *app_dir, const char *db_path,
                         ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";
    sqlite3 *db = hl_agent_open_app_db(app_dir, db_path);
    if (!db) return hl_agent_write_error(out, "cannot open database");

    extern const HlEntry hl_app_entries[];
    HlVfs vfs;
    hl_vfs_init(&vfs, hl_app_entries, app_dir);

    int rc = schema_diff_impl(db, &vfs, out);
    sqlite3_close(db);
    return rc;
}

#else /* !HL_ENABLE_DB */
/* ISO C requires a translation unit to contain at least one declaration. */
typedef int hl_agent_schema_diff_disabled;
#endif /* HL_ENABLE_DB */
