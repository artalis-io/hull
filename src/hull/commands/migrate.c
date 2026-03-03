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
#include "hull/cap/db.h"
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
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "hull migrate: cannot open database %s: %s\n",
                db_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    hl_cap_db_init(db);

    int result = hl_migrate_run(db, app_dir);

    hl_cap_db_shutdown(db);
    sqlite3_close(db);

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
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "hull migrate: cannot open database %s: %s\n",
                db_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    hl_cap_db_init(db);

    HlMigrationStatus *entries = NULL;
    int count = 0;

    int rc = hl_migrate_status(db, app_dir, &entries, &count);

    hl_cap_db_shutdown(db);
    sqlite3_close(db);

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

int hl_cmd_migrate(int argc, char **argv, const char *hull_exe)
{
    const char *app_dir = ".";
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
        return hull_tool("hull.migrate", argc, argv, hull_exe);

    /* Status subcommand */
    if (subcmd && strcmp(subcmd, "status") == 0)
        return cmd_status(app_dir, db_path);

    /* Default: run migrations */
    return cmd_run(app_dir, db_path);
}
