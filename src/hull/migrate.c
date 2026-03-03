/*
 * migrate.c — SQL migration runner
 *
 * Discovers .sql files from embedded entries or filesystem,
 * tracks applied migrations in _hull_migrations table,
 * executes pending ones in filename order.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/migrate.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* ── Embedded entries (weak — overridden by hull build) ───────────── */

typedef struct {
    const char         *name;
    const unsigned char *data;
    unsigned int         len;
} HlStdlibEntry;

extern const HlStdlibEntry hl_app_migration_entries[];

/* ── Tracking table ───────────────────────────────────────────────── */

static const char *CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS _hull_migrations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL UNIQUE,"
    "  applied_at TEXT NOT NULL DEFAULT (datetime('now'))"
    ")";

static int ensure_tracking_table(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, CREATE_TABLE_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        log_error("[hull:migrate] cannot create tracking table: %s",
                  err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* ── Check if a migration has been applied ────────────────────────── */

static int is_applied(sqlite3 *db, const char *name)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM _hull_migrations WHERE name = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

/* ── Record a migration as applied ────────────────────────────────── */

static int record_migration(sqlite3 *db, const char *name)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO _hull_migrations (name) VALUES (?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── Execute a single migration ───────────────────────────────────── */

static int execute_migration(sqlite3 *db, const char *name,
                             const char *sql, size_t sql_len)
{
    /* Skip empty migrations */
    if (sql_len == 0 || sql[0] == '\0') {
        log_warn("[hull:migrate] skipping empty migration: %s", name);
        return 0;
    }

    char *err = NULL;

    /* BEGIN IMMEDIATE for exclusive write access */
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        log_error("[hull:migrate] %s: cannot begin transaction: %s",
                  name, err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }

    /* Make a NUL-terminated copy if needed */
    char *sql_copy = NULL;
    const char *sql_ptr = sql;
    if (sql[sql_len] != '\0') {
        if (sql_len > SIZE_MAX / 2) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        sql_copy = malloc(sql_len + 1);
        if (!sql_copy) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        memcpy(sql_copy, sql, sql_len);
        sql_copy[sql_len] = '\0';
        sql_ptr = sql_copy;
    }

    /* Execute the migration SQL */
    rc = sqlite3_exec(db, sql_ptr, NULL, NULL, &err);
    free(sql_copy);

    if (rc != SQLITE_OK) {
        log_error("[hull:migrate] %s: SQL error: %s", name, err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    /* Record the migration */
    if (record_migration(db, name) != 0) {
        log_error("[hull:migrate] %s: failed to record migration", name);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    /* Commit */
    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        log_error("[hull:migrate] %s: commit failed: %s",
                  name, err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    log_info("[hull:migrate] applied: %s", name);
    return 1; /* 1 = applied */
}

/* ── String comparison for qsort ──────────────────────────────────── */

static int cmp_strings(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ── Discover filesystem migrations ───────────────────────────────── */

typedef struct {
    char **names;   /* sorted filenames (allocated) */
    char **sqls;    /* SQL content for each (allocated) */
    int    count;
} MigrationList;

static void migration_list_free(MigrationList *ml)
{
    for (int i = 0; i < ml->count; i++) {
        free(ml->names[i]);
        if (ml->sqls)
            free(ml->sqls[i]);
    }
    free(ml->names);
    free(ml->sqls);
    ml->names = NULL;
    ml->sqls = NULL;
    ml->count = 0;
}

static int discover_fs_migrations(const char *app_dir, MigrationList *ml)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/migrations", app_dir);

    DIR *dir = opendir(path);
    if (!dir)
        return -1; /* no directory */

    /* Collect .sql filenames */
    int capacity = 16;
    ml->names = malloc((size_t)capacity * sizeof(char *));
    ml->sqls  = NULL;
    ml->count = 0;
    if (!ml->names) {
        closedir(dir);
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".sql") != 0)
            continue;

        if (ml->count >= capacity) {
            capacity *= 2;
            char **tmp = realloc(ml->names, (size_t)capacity * sizeof(char *));
            if (!tmp) {
                migration_list_free(ml);
                closedir(dir);
                return -1;
            }
            ml->names = tmp;
        }
        ml->names[ml->count] = strdup(name);
        if (!ml->names[ml->count]) {
            migration_list_free(ml);
            closedir(dir);
            return -1;
        }
        ml->count++;
    }
    closedir(dir);

    if (ml->count == 0) {
        free(ml->names);
        ml->names = NULL;
        return 0;
    }

    /* Sort by filename */
    qsort(ml->names, (size_t)ml->count, sizeof(char *), cmp_strings);

    /* Read SQL content for each */
    ml->sqls = calloc((size_t)ml->count, sizeof(char *));
    if (!ml->sqls) {
        migration_list_free(ml);
        return -1;
    }

    for (int i = 0; i < ml->count; i++) {
        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "%s/migrations/%s",
                 app_dir, ml->names[i]);

        FILE *f = fopen(filepath, "r");
        if (!f) {
            log_error("[hull:migrate] cannot read %s", filepath);
            migration_list_free(ml);
            return -1;
        }

        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (flen < 0) {
            fclose(f);
            migration_list_free(ml);
            return -1;
        }

        char *sql = malloc((size_t)flen + 1);
        if (!sql) {
            fclose(f);
            migration_list_free(ml);
            return -1;
        }

        if (flen > 0) {
            size_t read = fread(sql, 1, (size_t)flen, f);
            sql[read] = '\0';
        } else {
            sql[0] = '\0';
        }
        fclose(f);

        ml->sqls[i] = sql;
    }

    return 0;
}

/* ── Public API: run migrations ───────────────────────────────────── */

int hl_migrate_run(sqlite3 *db, const char *app_dir)
{
    if (ensure_tracking_table(db) != 0)
        return HL_MIGRATE_ERR;

    /* Check embedded entries first */
    int has_embedded = (hl_app_migration_entries[0].name != NULL);

    if (has_embedded) {
        int applied = 0;
        for (const HlStdlibEntry *e = hl_app_migration_entries; e->name; e++) {
            if (is_applied(db, e->name))
                continue;

            int rc = execute_migration(db, e->name,
                                       (const char *)e->data, e->len);
            if (rc < 0)
                return HL_MIGRATE_ERR;
            applied += rc;
        }
        return applied;
    }

    /* Fall back to filesystem discovery */
    MigrationList ml = {0};
    int rc = discover_fs_migrations(app_dir, &ml);
    if (rc < 0)
        return HL_MIGRATE_NO_DIR;

    if (ml.count == 0)
        return HL_MIGRATE_NO_DIR;

    int applied = 0;
    for (int i = 0; i < ml.count; i++) {
        if (is_applied(db, ml.names[i]))
            continue;

        rc = execute_migration(db, ml.names[i],
                               ml.sqls[i], strlen(ml.sqls[i]));
        if (rc < 0) {
            migration_list_free(&ml);
            return HL_MIGRATE_ERR;
        }
        applied += rc;
    }

    migration_list_free(&ml);
    return applied;
}

/* ── Public API: query status ─────────────────────────────────────── */

int hl_migrate_status(sqlite3 *db, const char *app_dir,
                      HlMigrationStatus **out, int *out_count)
{
    if (ensure_tracking_table(db) != 0)
        return -1;

    /* Check embedded entries first */
    int has_embedded = (hl_app_migration_entries[0].name != NULL);

    /* Build list of migration names */
    int count = 0;
    char **names = NULL;

    if (has_embedded) {
        /* Count embedded entries */
        for (const HlStdlibEntry *e = hl_app_migration_entries; e->name; e++)
            count++;

        names = malloc((size_t)count * sizeof(char *));
        if (!names)
            return -1;

        int i = 0;
        for (const HlStdlibEntry *e = hl_app_migration_entries; e->name; e++)
            names[i++] = strdup(e->name);
    } else {
        /* Discover from filesystem */
        MigrationList ml = {0};
        if (discover_fs_migrations(app_dir, &ml) < 0 || ml.count == 0) {
            *out = NULL;
            *out_count = 0;
            migration_list_free(&ml);
            return 0;
        }

        count = ml.count;
        names = malloc((size_t)count * sizeof(char *));
        if (!names) {
            migration_list_free(&ml);
            return -1;
        }

        for (int i = 0; i < count; i++)
            names[i] = strdup(ml.names[i]);

        migration_list_free(&ml);
    }

    /* Build status entries */
    HlMigrationStatus *entries = calloc((size_t)count, sizeof(HlMigrationStatus));
    if (!entries) {
        for (int i = 0; i < count; i++)
            free(names[i]);
        free(names);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        entries[i].name = names[i]; /* ownership transferred */

        /* Check if applied and get timestamp */
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db,
            "SELECT applied_at FROM _hull_migrations WHERE name = ?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, names[i], -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                entries[i].applied = 1;
                const char *ts = (const char *)sqlite3_column_text(stmt, 0);
                entries[i].applied_at = ts ? strdup(ts) : NULL;
            }
            sqlite3_finalize(stmt);
        }
    }

    free(names); /* individual strings now owned by entries */

    *out = entries;
    *out_count = count;
    return 0;
}

void hl_migrate_status_free(HlMigrationStatus *entries, int count)
{
    if (!entries)
        return;
    for (int i = 0; i < count; i++) {
        free((void *)entries[i].name);
        free((void *)entries[i].applied_at);
    }
    free(entries);
}
