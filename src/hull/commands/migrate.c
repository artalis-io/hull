/*
 * commands/migrate.c — hull migrate subcommand
 *
 * Dispatch:
 *   hull migrate [app_dir]          → run pending migrations (C)
 *   hull migrate status [app_dir]   → show migration status (C)
 *   hull migrate new <name>         → scaffold new .sql file (Lua)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/migrate.h"
#include "hull/migrate.h"
#include "hull/vfs.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/tool.h"

#include <sqlite3.h>

#include <stdio.h>
#include <string.h>

/* ── Usage ─────────────────────────────────────────────────────────── */

static void migrate_usage(void)
{
    fprintf(stderr,
        "Usage: hull migrate [subcommand] [options]\n"
        "\n"
        "Subcommands:\n"
        "  (none)              Run pending migrations\n"
        "  status              Show migration status\n"
        "  new <name>          Create a new migration file\n"
        "\n"
        "Options:\n"
        "  -d FILE             Database file (default: data.db)\n"
        "  [app_dir]           App directory (default: .)\n");
}

/* ── Run subcommand ───────────────────────────────────────────────── */

static int cmd_run(const char *app_dir, const char *db_path)
{
    /* Open via the backend vtable so the handle.ctx is the proper
     * HlDbSqliteCtx — required for hl_db_sqlite_raw / hl_migrate_run. */
    HlDbHandle handle = { .backend = &hl_db_backend_sqlite, .ctx = NULL };
    if (hl_db_backend_sqlite.open(&handle.ctx, db_path, NULL) != 0) {
        fprintf(stderr, "hull migrate: cannot open database %s\n", db_path);
        return 1;
    }

    extern const HlEntry hl_app_entries[];
    HlVfs vfs;
    hl_vfs_init(&vfs, hl_app_entries, app_dir);

    int result = hl_migrate_run(&handle, &vfs);

    hl_db_backend_sqlite.close(handle.ctx);

    if (result == HL_MIGRATE_ERR) {
        fprintf(stderr, "hull migrate: migration failed\n");
        return 1;
    }
    if (result == HL_MIGRATE_NO_DIR) {
        printf("hull migrate: no migrations found\n");
        return 0;
    }
    if (result == 0) {
        printf("hull migrate: already up to date\n");
    } else {
        printf("hull migrate: applied %d migration(s)\n", result);
    }
    return 0;
}

/* ── Status subcommand ────────────────────────────────────────────── */

static int cmd_status(const char *app_dir, const char *db_path)
{
    /* Open via the backend vtable (same reason as cmd_run). */
    HlDbHandle handle = { .backend = &hl_db_backend_sqlite, .ctx = NULL };
    if (hl_db_backend_sqlite.open(&handle.ctx, db_path, NULL) != 0) {
        fprintf(stderr, "hull migrate: cannot open database %s\n", db_path);
        return 1;
    }

    extern const HlEntry hl_app_entries[];
    HlVfs vfs;
    hl_vfs_init(&vfs, hl_app_entries, app_dir);

    HlMigrationStatus *entries = NULL;
    int count = 0;
    int rc = hl_migrate_status(&handle, &vfs, &entries, &count);

    hl_db_backend_sqlite.close(handle.ctx);

    if (rc != 0) {
        fprintf(stderr, "hull migrate: failed to query status\n");
        return 1;
    }

    if (count == 0) {
        printf("hull migrate: no migrations found\n");
        return 0;
    }

    printf("Migrations:\n");
    for (int i = 0; i < count; i++) {
        if (entries[i].applied) {
            printf("  [x] %s  (%s)\n", entries[i].name,
                   entries[i].applied_at ? entries[i].applied_at : "?");
        } else {
            printf("  [ ] %s  (pending)\n", entries[i].name);
        }
    }

    hl_migrate_status_free(entries, count);
    return 0;
}

/* ── Command entry point ──────────────────────────────────────────── */

int hl_cmd_migrate(int argc, char **argv, const HlCommandEnv *env)
{
    const char *app_dir = env->app_dir;
    const char *db_path = "data.db";
    const char *subcmd = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            migrate_usage();
            return 0;
        } else if (!subcmd && argv[i][0] != '-') {
            /* First positional arg: could be subcommand or app_dir */
            if (strcmp(argv[i], "status") == 0 ||
                strcmp(argv[i], "new") == 0) {
                subcmd = argv[i];
            } else {
                app_dir = argv[i];
            }
        } else if (subcmd && argv[i][0] != '-') {
            /* Second positional arg: app_dir (or name for 'new') */
            app_dir = argv[i];
        }
    }

    /* Dispatch to Lua for 'new' subcommand */
    if (subcmd && strcmp(subcmd, "new") == 0)
        return hull_tool("hull.migrate", argc, argv, env->hull_exe);

    /* Status subcommand */
    if (subcmd && strcmp(subcmd, "status") == 0)
        return cmd_status(app_dir, db_path);

    /* Default: run migrations */
    return cmd_run(app_dir, db_path);
}
