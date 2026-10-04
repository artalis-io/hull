/*
 * migrate.c - SQL migration runner
 *
 * Discovers .sql files from embedded entries or filesystem,
 * tracks applied migrations in _hull_migrations table,
 * executes pending ones in filename order.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/migrate.h"
#include "hull/utils/alloc.h"
#include "hull/vfs.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/crypto.h"   /* hl_cap_crypto_sha256 */
#include "utils/hex.h"        /* hl_hex_encode */

#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

/* ── Tracking table ───────────────────────────────────────────────── */

/* Portable across SQLite, Postgres, and MySQL: `name` is the primary key (the
 * runner dedupes by name and orders by filename, so no surrogate id is
 * needed), and `applied_at` is written explicitly by the runner instead of
 * via a backend-specific DEFAULT. `name` is VARCHAR(255), not TEXT: MySQL
 * cannot index a TEXT/BLOB primary key without a prefix length, and a
 * migration name is a short filename. VARCHAR carries TEXT affinity on SQLite
 * and is directly comparable to text on Postgres, so the `WHERE name = ?`
 * lookup is unaffected. Existing databases created with the older id/
 * AUTOINCREMENT (or TEXT-PK) schema keep working: CREATE IF NOT EXISTS is a
 * no-op and the INSERT below names its columns. */
static const char *CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS _hull_migrations ("
    "  name VARCHAR(255) NOT NULL PRIMARY KEY,"
    "  applied_at TEXT NOT NULL,"
    "  checksum VARCHAR(64)"
    ")";

static int noop_row_cb(void *ctx, HlColumn *cols, int ncols)
{
    (void)ctx; (void)cols; (void)ncols;
    return 1;
}

static int ensure_tracking_table(HlDbHandle *h)
{
    if (hl_db_exec(h, CREATE_TABLE_SQL, NULL, 0) < 0) {
        log_error("[hull:migrate] cannot create tracking table: %s",
                  hl_db_errmsg(h));
        return -1;
    }
    /* A table made before the checksum column existed gets it (nullable;
     * such rows are backfilled from the SQL as it is now). */
    static const char probe[] = "SELECT checksum FROM _hull_migrations WHERE 1 = 0";
    if (hl_db_query(h, probe, NULL, 0, noop_row_cb, NULL, NULL) != 0 &&
        hl_db_exec(h, "ALTER TABLE _hull_migrations ADD COLUMN checksum VARCHAR(64)",
                   NULL, 0) < 0) {
        /* Two processes on one database at their first start after an
         * upgrade (a server and a jobs worker) both try the ALTER; the
         * loser's fails with "duplicate column". The column exists either
         * way - probe again before refusing to start. */
        if (hl_db_query(h, probe, NULL, 0, noop_row_cb, NULL, NULL) == 0)
            return 0;
        log_error("[hull:migrate] cannot add the checksum column: %s",
                  hl_db_errmsg(h));
        return -1;
    }
    return 0;
}

/* sha256 hex of a migration's SQL. */
static void sql_checksum(const char *sql, size_t len, char hex[65])
{
    uint8_t d[32];
    if (hl_cap_crypto_sha256(sql, len, d) != 0) { hex[0] = '\0'; return; }
    hl_hex_encode(d, 32, hex, 65);
}

/* ── Check if a migration has been applied ────────────────────────── */

typedef struct {
    int  found;
    int  has_sum;
    char sum[65];
} AppliedRow;

static int migrate_found_cb(void *ctx, HlColumn *cols, int ncols)
{
    AppliedRow *r = (AppliedRow *)ctx;
    r->found = 1;
    if (ncols > 0 && (cols[0].value.type == HL_TYPE_TEXT ||
                      cols[0].value.type == HL_TYPE_BLOB) &&
        cols[0].value.len == 64) {
        memcpy(r->sum, cols[0].value.s, 64);
        r->sum[64] = '\0';
        r->has_sum = 1;
    }
    return 1;   /* one row is enough; stop the scan */
}

/* 1 applied, 0 not, -1 when the lookup itself failed. A failed lookup read
 * as "not applied" and ran the migration again - on a dropped connection, a
 * locked database, a timeout - against a schema that already had it. */
/* An applied migration's SQL is compared with what was applied: an edit
 * used to be skipped without a word, so the schema the code expects and
 * the one the database has drifted apart silently. Not re-run (that is
 * what a new migration is for) - reported. A row from before checksums is
 * backfilled. */
static int is_applied(HlDbHandle *h, const char *name, const char *sql, size_t len)
{
    HlValue p = { .type = HL_TYPE_TEXT, .s = name, .len = strlen(name) };
    AppliedRow r = { 0, 0, { 0 } };
    if (hl_db_query(h, "SELECT checksum FROM _hull_migrations WHERE name = ?",
                    &p, 1, migrate_found_cb, &r, NULL) != 0) {
        log_error("[migrate] could not check whether %s is applied: %s",
                  name, hl_db_errmsg(h));
        return -1;
    }
    if (!r.found) return 0;
    char now[65];
    sql_checksum(sql, len, now);
    if (!r.has_sum) {
        HlValue up[2] = {
            { .type = HL_TYPE_TEXT, .s = now,  .len = strlen(now) },
            { .type = HL_TYPE_TEXT, .s = name, .len = strlen(name) },
        };
        (void)hl_db_exec(h, "UPDATE _hull_migrations SET checksum = ? "
                            "WHERE name = ? AND checksum IS NULL", up, 2);
    } else if (strcmp(now, r.sum) != 0) {
        log_warn("[hull:migrate] %s was CHANGED after it was applied - the "
                 "change is NOT applied (applied migrations never re-run). "
                 "Put schema changes in a new migration.", name);
    }
    return 1;
}

/* ── Record a migration as applied ────────────────────────────────── */

/* ISO-8601 UTC, generated host-side so the stored value is identical on
 * every backend (no datetime('now') / now() dialect split). */
static void iso_now(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    if (strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0 && len > 0)
        buf[0] = '\0';
}

static int record_migration(HlDbHandle *h, const char *name,
                            const char *sql, size_t len)
{
    char ts[32], sum[65];
    iso_now(ts, sizeof ts);
    sql_checksum(sql, len, sum);
    HlValue params[3] = {
        { .type = HL_TYPE_TEXT, .s = name, .len = strlen(name) },
        { .type = HL_TYPE_TEXT, .s = ts,   .len = strlen(ts) },
        { .type = HL_TYPE_TEXT, .s = sum,  .len = strlen(sum) },
    };
    if (hl_db_exec(h, "INSERT INTO _hull_migrations (name, applied_at, checksum) "
                      "VALUES (?, ?, ?)", params, 3) < 0)
        return -1;
    return 0;
}

/* ── Execute a single migration ───────────────────────────────────── */

static int execute_migration(HlDbHandle *h, const char *name,
                             const char *sql, size_t sql_len)
{
    /* Skip empty migrations */
    if (sql_len == 0 || sql[0] == '\0') {
        log_warn("[hull:migrate] skipping empty migration: %s", name);
        return 0;
    }

    if (hl_db_begin(h) != 0) {
        log_error("[hull:migrate] %s: cannot begin transaction: %s",
                  name, hl_db_errmsg(h));
        return -1;
    }

    /* Applied by another process since the caller looked? Two instances
     * starting together both saw the migration as pending; the second then
     * re-ran it once the first committed and failed ("table already
     * exists"), so it did not start. Checked again under the transaction:
     * SQLite's BEGIN IMMEDIATE holds the write lock; on Postgres the table
     * lock serialises the two. */
    if (h->backend && h->backend->name && strcmp(h->backend->name, "postgres") == 0 &&
        hl_db_exec(h, "LOCK TABLE _hull_migrations IN SHARE ROW EXCLUSIVE MODE",
                   NULL, 0) != 0) {
        log_error("[hull:migrate] %s: cannot lock _hull_migrations: %s",
                  name, hl_db_errmsg(h));
        hl_db_rollback(h);
        return -1;
    }
    {
        HlValue p = { .type = HL_TYPE_TEXT, .s = name, .len = strlen(name) };
        AppliedRow r = { 0, 0, { 0 } };
        if (hl_db_query(h, "SELECT checksum FROM _hull_migrations WHERE name = ?",
                        &p, 1, migrate_found_cb, &r, NULL) != 0) {
            log_error("[hull:migrate] %s: cannot re-check: %s",
                      name, hl_db_errmsg(h));
            hl_db_rollback(h);
            return -1;
        }
        if (r.found) {
            hl_db_rollback(h);
            log_info("[hull:migrate] %s: applied by another process", name);
            return 0;
        }
    }

    /* Always copy to ensure NUL-termination (avoids OOB read if VFS
     * entry isn't NUL-terminated).  Migration SQL is small and runs
     * once at startup, so the copy cost is negligible. */
    if (sql_len > SIZE_MAX / 2) { hl_db_rollback(h); return -1; }
    char *sql_copy = malloc(sql_len + 1);
    if (!sql_copy) { hl_db_rollback(h); return -1; }
    memcpy(sql_copy, sql, sql_len);
    sql_copy[sql_len] = '\0';

    /* Migration files can bundle several ;-separated statements, so use the
     * script path (sqlite3_exec / PG simple Query), not the single-statement
     * exec. */
    int rc = hl_db_exec_script(h, sql_copy);
    free(sql_copy);
    if (rc < 0) {
        log_error("[hull:migrate] %s: SQL error: %s", name, hl_db_errmsg(h));
        hl_db_rollback(h);
        return -1;
    }

    if (record_migration(h, name, sql, sql_len) != 0) {
        log_error("[hull:migrate] %s: failed to record migration", name);
        hl_db_rollback(h);
        return -1;
    }

    if (hl_db_commit(h) != 0) {
        log_error("[hull:migrate] %s: commit failed: %s",
                  name, hl_db_errmsg(h));
        hl_db_rollback(h);
        return -1;
    }

    log_info("[hull:migrate] applied: %s", name);
    return 1; /* 1 = applied */
}

/* ── String comparison for qsort ──────────────────────────────────── */

static int cmp_strings(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
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

static int discover_fs_migrations(const char *root_dir, MigrationList *ml)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/migrations", root_dir);

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
            if ((size_t)capacity > SIZE_MAX / (2 * sizeof(char *))) {
                migration_list_free(ml);
                closedir(dir);
                return -1;
            }
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
                 root_dir, ml->names[i]);

        /* A regular file only, opened without blocking: a FIFO named
         * NNN_x.sql hung startup in fopen. */
        int mfd = open(filepath, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        struct stat mst;
        FILE *f = NULL;
        if (mfd >= 0 && fstat(mfd, &mst) == 0 && S_ISREG(mst.st_mode))
            f = fdopen(mfd, "r");
        if (!f) {
            if (mfd >= 0) close(mfd);
            log_error("[hull:migrate] cannot read %s", filepath);
            migration_list_free(ml);
            return -1;
        }

        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            migration_list_free(ml);
            return -1;
        }
        long flen = ftell(f);
        if (fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            migration_list_free(ml);
            return -1;
        }

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
            size_t nread = fread(sql, 1, (size_t)flen, f);
            /* L3: distinguish short read from underlying read error. */
            int read_err = ferror(f);
            if (read_err || nread != (size_t)flen) {
                free(sql);
                fclose(f);
                migration_list_free(ml);
                log_error("[migrate] %s on %s (%zu/%ld bytes)",
                          read_err ? "read error" : "short read",
                          ml->names[i], nread, flen);
                return -1;
            }
            sql[flen] = '\0';
        } else {
            sql[0] = '\0';
        }
        fclose(f);

        /* --verify-sig: only a signed migration, as signed (the startup
         * check hashed it; this is the copy that will run). */
        char rel[4096];
        int rn = snprintf(rel, sizeof(rel), "migrations/%s", ml->names[i]);
        if (rn < 0 || (size_t)rn >= sizeof(rel) ||
            hl_vfs_disk_gate_check(rel, sql, (size_t)flen) != 0) {
            log_error("[hull:migrate] %s is not a signed file (--verify-sig)",
                      ml->names[i]);
            free(sql);
            migration_list_free(ml);
            return -1;
        }

        ml->sqls[i] = sql;
    }

    return 0;
}

/* ── Cross-process migration lock ─────────────────────────────────── */

/* Two instances starting together against one network database both saw a
 * migration pending. SQLite's BEGIN IMMEDIATE and Postgres' LOCK TABLE (in
 * execute_migration) serialise the per-migration re-check, but not on MySQL,
 * where the re-check is a non-locking consistent read and DDL commits
 * implicitly: the second instance re-ran the migration and failed to start
 * ("table exists", a duplicate key in _hull_migrations). Nor did anything
 * cover the first CREATE TABLE IF NOT EXISTS _hull_migrations, which two
 * Postgres sessions racing fail with a pg_type unique violation. So the whole
 * run holds a session-level named lock on the network backends (audit 5 L3):
 * Postgres pg_advisory_lock, MySQL GET_LOCK. Both are released with the
 * session if the process dies. */
#define HL_MIGRATE_PG_LOCK_KEY  "5216458419349063506"   /* "HULLMIGR" */
#define HL_MIGRATE_MY_LOCK_NAME "hull_migrate"
#define HL_MIGRATE_MY_LOCK_WAIT "300"                    /* seconds */

typedef enum { MIG_LOCK_NONE = 0, MIG_LOCK_PG, MIG_LOCK_MY } MigLockKind;

static MigLockKind mig_lock_kind(const HlDbHandle *h)
{
    const char *n = h->backend ? h->backend->name : NULL;
    if (!n) return MIG_LOCK_NONE;
    if (strcmp(n, "postgres") == 0) return MIG_LOCK_PG;
    if (strcmp(n, "mysql") == 0)    return MIG_LOCK_MY;
    return MIG_LOCK_NONE;
}

/* GET_LOCK answers 1 (acquired), 0 (timed out) or NULL (error). */
static int mig_first_int_cb(void *ctx, HlColumn *cols, int ncols)
{
    int *out = (int *)ctx;
    if (ncols > 0) {
        if (cols[0].value.type == HL_TYPE_INT)
            *out = (int)cols[0].value.i;
        else if (cols[0].value.type == HL_TYPE_TEXT && cols[0].value.len == 1)
            *out = cols[0].value.s[0] == '1' ? 1 : 0;
    }
    return 1;
}

static int mig_lock(HlDbHandle *h, MigLockKind kind)
{
    if (kind == MIG_LOCK_PG) {
        if (hl_db_query(h, "SELECT pg_advisory_lock(" HL_MIGRATE_PG_LOCK_KEY ")",
                        NULL, 0, noop_row_cb, NULL, NULL) != 0) {
            log_error("[hull:migrate] cannot take the migration lock: %s",
                      hl_db_errmsg(h));
            return -1;
        }
    } else if (kind == MIG_LOCK_MY) {
        int got = -1;
        if (hl_db_query(h, "SELECT GET_LOCK('" HL_MIGRATE_MY_LOCK_NAME "', "
                           HL_MIGRATE_MY_LOCK_WAIT ")",
                        NULL, 0, mig_first_int_cb, &got, NULL) != 0 || got != 1) {
            log_error("[hull:migrate] cannot take the migration lock%s%s",
                      got == 0 ? ": another instance held it for "
                                 HL_MIGRATE_MY_LOCK_WAIT "s" : ": ",
                      got == 0 ? "" : hl_db_errmsg(h));
            return -1;
        }
    }
    return 0;
}

static void mig_unlock(HlDbHandle *h, MigLockKind kind)
{
    if (kind == MIG_LOCK_PG)
        (void)hl_db_query(h, "SELECT pg_advisory_unlock(" HL_MIGRATE_PG_LOCK_KEY ")",
                          NULL, 0, noop_row_cb, NULL, NULL);
    else if (kind == MIG_LOCK_MY)
        (void)hl_db_query(h, "SELECT RELEASE_LOCK('" HL_MIGRATE_MY_LOCK_NAME "')",
                          NULL, 0, noop_row_cb, NULL, NULL);
}

/* ── Public API: run migrations ───────────────────────────────────── */

static int migrate_run_locked(HlDbHandle *handle, const HlVfs *vfs);

int hl_migrate_run(HlDbHandle *handle, const HlVfs *vfs)
{
    /* The tracking table + SQL all flow through the HlDbBackend vtable, so
     * the runner works on any wired backend (SQLite file or Postgres). No
     * backend at all (compute-only build / --no-db) means nothing to do. */
    if (!handle || !handle->backend)
        return 0;
    MigLockKind kind = mig_lock_kind(handle);
    if (mig_lock(handle, kind) != 0)
        return HL_MIGRATE_ERR;
    int rc = migrate_run_locked(handle, vfs);
    mig_unlock(handle, kind);
    return rc;
}

static int migrate_run_locked(HlDbHandle *handle, const HlVfs *vfs)
{
    if (ensure_tracking_table(handle) != 0)
        return HL_MIGRATE_ERR;

    /* Check for embedded migration entries via VFS prefix query */
    const HlEntry *first = NULL;
    size_t mig_count = hl_vfs_prefix(vfs, "migrations/", &first);

    if (mig_count > 0) {
        int applied = 0;
        for (size_t i = 0; i < mig_count; i++) {
            const char *mig_name = first[i].name + 11; /* strip "migrations/" */
            int ia = is_applied(handle, mig_name, (const char *)first[i].data,
                                first[i].len);
            if (ia < 0)
                return HL_MIGRATE_ERR;
            if (ia)
                continue;

            int rc = execute_migration(handle, mig_name,
                                       (const char *)first[i].data, first[i].len);
            if (rc < 0)
                return HL_MIGRATE_ERR;
            applied += rc;
        }
        return applied;
    }

    /* Fall back to filesystem discovery - in development only. A built binary
     * (its app files embedded) has exactly the migrations it was built with,
     * none included: reading <cwd>/migrations ran whatever unsigned SQL lay
     * beside the binary when it started, past --verify-sig. */
    if (!vfs->root_dir || vfs->count > 0)
        return HL_MIGRATE_NO_DIR;

    MigrationList ml = {0};
    int rc = discover_fs_migrations(vfs->root_dir, &ml);
    if (rc < 0)
        return HL_MIGRATE_NO_DIR;

    if (ml.count == 0)
        return HL_MIGRATE_NO_DIR;

    int applied = 0;
    for (int i = 0; i < ml.count; i++) {
        int ia = is_applied(handle, ml.names[i], ml.sqls[i], strlen(ml.sqls[i]));
        if (ia < 0) {
            migration_list_free(&ml);
            return HL_MIGRATE_ERR;
        }
        if (ia)
            continue;

        rc = execute_migration(handle, ml.names[i],
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

/* Probe one migration's applied state: sets applied=1 and copies the
 * (length-tracked, possibly non-NUL-terminated) applied_at wire value. */
typedef struct { int applied; char *applied_at; } StatusProbe;

static int status_probe_cb(void *ctx, HlColumn *cols, int ncols)
{
    StatusProbe *p = (StatusProbe *)ctx;
    p->applied = 1;
    if (ncols > 0 && cols[0].value.type == HL_TYPE_TEXT && cols[0].value.s) {
        size_t l = cols[0].value.len;
        char *s = malloc(l + 1);
        if (s) { memcpy(s, cols[0].value.s, l); s[l] = '\0'; p->applied_at = s; }
    }
    return 1;   /* one row is enough */
}

int hl_migrate_status(HlDbHandle *handle, const HlVfs *vfs,
                      HlMigrationStatus **out, int *out_count)
{
    if (!handle || !handle->backend) {
        /* Absent backend (compute-only / --no-db): report zero migrations. */
        if (out) *out = NULL;
        if (out_count) *out_count = 0;
        return 0;
    }
    if (ensure_tracking_table(handle) != 0)
        return -1;

    /* Check for embedded migration entries via VFS prefix query */
    const HlEntry *first = NULL;
    size_t mig_count = hl_vfs_prefix(vfs, "migrations/", &first);

    /* Build list of migration names */
    int count = 0;
    char **names = NULL;

    if (mig_count > 0) {
        count = (int)mig_count;

        names = calloc((size_t)count, sizeof(char *));
        if (!names)
            return -1;

        for (int i = 0; i < count; i++) {
            names[i] = strdup(first[i].name + 11); /* strip "migrations/" */
            if (!names[i]) {
                for (int j = 0; j < i; j++) free(names[j]);
                free(names);
                return -1;
            }
        }
    } else {
        /* Discover from filesystem */
        if (!vfs->root_dir) {
            *out = NULL;
            *out_count = 0;
            return 0;
        }

        MigrationList ml = {0};
        if (discover_fs_migrations(vfs->root_dir, &ml) < 0 || ml.count == 0) {
            *out = NULL;
            *out_count = 0;
            migration_list_free(&ml);
            return 0;
        }

        count = ml.count;
        names = calloc((size_t)count, sizeof(char *));
        if (!names) {
            migration_list_free(&ml);
            return -1;
        }

        for (int i = 0; i < count; i++) {
            names[i] = strdup(ml.names[i]);
            if (!names[i]) {
                for (int j = 0; j < i; j++) free(names[j]);
                free(names);
                migration_list_free(&ml);
                return -1;
            }
        }

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

        /* Check if applied and capture the timestamp, via the vtable. */
        HlValue namep = { .type = HL_TYPE_TEXT,
                          .s = names[i], .len = strlen(names[i]) };
        StatusProbe probe = { 0, NULL };
        hl_db_query(handle,
            "SELECT applied_at FROM _hull_migrations WHERE name = ?",
            &namep, 1, status_probe_cb, &probe, NULL);
        entries[i].applied = probe.applied;
        entries[i].applied_at = probe.applied_at;
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
        hl_free_const(entries[i].name);
        hl_free_const(entries[i].applied_at);
    }
    free(entries);
}

#endif /* HL_ENABLE_DB */
