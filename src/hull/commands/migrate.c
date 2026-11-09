/*
 * commands/migrate.c - hull migrate subcommand
 *
 * Dispatch:
 *   hull migrate [app_dir]          → run pending migrations (C)
 *   hull migrate status [app_dir]   → show migration status (C)
 *   hull migrate new <name>         → scaffold new .sql file (Lua)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/commands/migrate.h"
#include "hull/migrate.h"
#include "hull/vfs.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"   /* hl_db_sqlite_wrap/_unwrap + raw */
#include "hull/cap/db_internal.h" /* hl_db_internal_startup / _configure */
#include "hull/cap/db_registry.h"
#include "hull/app_context.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h" /* hl_module_set_uses_internal_db */
#include "hull/runtime.h"
#include "hull/tool.h"
#include "hull/shared/fs_util.h"   /* hl_default_db_path_check */
#include "hull/shared/cli_log.h"   /* hl_log_fputs_escaped */
#include <limits.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
        "  --keep-internal-in-app-db\n"
        "                      Move the stdlib's _hull_* tables back into the\n"
        "                      app database instead of the Hull-owned internal\n"
        "                      database (the pre-0.18 layout)\n"
        "  [app_dir]           App directory (default: .)\n");
}

/* ── Run subcommand ───────────────────────────────────────────────── */

static int cmd_run(const char *app_dir, const char *db_path)
{
    /* Open via the backend vtable so the handle.ctx is the proper
     * HlDbSqliteCtx - required for hl_db_sqlite_raw / hl_migrate_run. */
    const char *db_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(db_path, &db_err);
    if (!be) {
        fprintf(stderr, "hull migrate: %s\n",
                db_err ? db_err : "no database backend");
        return 1;
    }
    HlDbHandle handle = { .backend = be, .ctx = NULL };
    if (be->open(&handle.ctx, db_path, NULL) != 0) {
        char shown[256];
        hl_db_dsn_redact(db_path, shown, sizeof shown);   /* no password (audit 9 L4) */
        fprintf(stderr, "hull migrate: cannot open database %s\n", shown);
        return 1;
    }

    extern const HlEntry hl_app_entries[];
    HlVfs vfs;
    hl_vfs_init(&vfs, hl_app_entries, app_dir);

    int result = hl_migrate_run(&handle, &vfs);

    be->close(&handle);

    if (result == HL_MIGRATE_ERR) {
        fprintf(stderr, "hull migrate: migration failed\n");
        return 1;
    }
    if (result == HL_MIGRATE_NO_DIR) {
        printf("hull migrate: no migrations found\n");
    } else if (result == 0) {
        printf("hull migrate: already up to date\n");
    } else {
        printf("hull migrate: applied %d migration(s)\n", result);
    }
    return 0;
}

/* ── The internal-database move ───────────────────────────────────── */

/* 1 when @p app_dir holds an app entry (app.lua / app.js). */
static int has_entry(const char *app_dir)
{
    static const char *const names[] = { "app.lua", "app.js" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[PATH_MAX];
        int n = snprintf(p, sizeof p, "%s/%s", app_dir, names[i]);
        if (n > 0 && (size_t)n < sizeof p && access(p, F_OK) == 0) return 1;
    }
    return 0;
}

/* Move the stdlib's _hull_* tables into the Hull-owned internal database (or
 * back, under --keep-internal-in-app-db), exactly as a start without
 * --no-migrate does (hl_db_internal_startup): the documented "run `hull
 * migrate` separately, start with --no-migrate" deployment otherwise never
 * moved them, and a start refuses while they are not moved. The app is
 * loaded (as `hull test` / `hull agent` load it) to learn whether its
 * manifest declares databases.internal, in which case nothing moves; a
 * directory with no app entry has no manifest. */
static int cmd_move_internal(const char *app_dir, const char *db_path, int keep,
                             int from_manifest)
{
    int rc = 1;
    if (!has_entry(app_dir)) {
        HlDbRegistry *reg = hl_db_registry_create(NULL, db_path, NULL);
        const char *derr = NULL;
        if (!reg || hl_db_registry_set_app_dir(reg, app_dir) != 0 ||
            !hl_db_registry_get(reg, "default", &derr) ||
            hl_db_internal_configure(reg, app_dir, db_path, keep) != 0) {
            fprintf(stderr, "hull migrate: cannot open the app database for "
                            "the internal-table move\n");
        } else if (hl_db_internal_startup(reg, keep, 0) == 0) {
            rc = 0;
        }
        hl_db_registry_destroy(reg);
        return rc;
    }

    /* A default the manifest names (databases.named.default, "@state/") is
     * resolved exactly as the runners resolve it - re-pointed, with the
     * state directory - so the internal database is the one they read
     * (<state>/.hull/internal.db). Handed over as a -d it was a database
     * outside the app tree, given <file>.hull-internal beside it: the move
     * went there, and a later --no-migrate start passed its check with an
     * empty internal database (audit 44). */
    HlAppContextOpts opts = {
        .app_dir = app_dir,
        .db_path = from_manifest ? NULL : db_path,
        .db_path_explicit = !from_manifest,
        .manifest_default_db = from_manifest,
        .no_migrate = 1,               /* cmd_run applied them */
        .gate_modules = 1,
        .keep_internal_in_app_db = keep,
    };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0) {
        fprintf(stderr, "hull migrate: cannot load the app to read its "
                        "manifest (databases.internal); the stdlib's _hull_* "
                        "tables were not moved\n");
        return 1;
    }
    HlRuntime *rt = hl_app_context_runtime(ctx);
    HlManifest m;
    memset(&m, 0, sizeof m);
    int have_m = rt && rt->vt->extract_manifest &&
                 rt->vt->extract_manifest(rt, &m) == 0;
    if (rt && rt->db_registry) {
        if (have_m) hl_db_registry_set_manifest(rt->db_registry, &m);
        if (hl_db_internal_startup(rt->db_registry, keep,
                                   hl_module_set_uses_internal_db(rt->module_set)) == 0)
            rc = 0;
        hl_db_registry_set_manifest(rt->db_registry, NULL);
    } else {
        rc = 0;   /* no database: nothing to move */
    }
    if (have_m) hl_manifest_free(&m);
    hl_app_context_free(ctx);
    if (rc != 0)
        fprintf(stderr, "hull migrate: the internal-table move failed\n");
    return rc;
}

/* ── Status subcommand ────────────────────────────────────────────── */

static int cmd_status(const char *app_dir, const char *db_path)
{
    /* Open via the backend vtable (same reason as cmd_run). */
    const char *db_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(db_path, &db_err);
    if (!be) {
        fprintf(stderr, "hull migrate: %s\n",
                db_err ? db_err : "no database backend");
        return 1;
    }
    HlDbHandle handle = { .backend = be, .ctx = NULL };
    if (be->open(&handle.ctx, db_path, NULL) != 0) {
        char shown[256];
        hl_db_dsn_redact(db_path, shown, sizeof shown);   /* no password (audit 9 L4) */
        fprintf(stderr, "hull migrate: cannot open database %s\n", shown);
        return 1;
    }

    extern const HlEntry hl_app_entries[];
    HlVfs vfs;
    hl_vfs_init(&vfs, hl_app_entries, app_dir);

    HlMigrationStatus *entries = NULL;
    int count = 0;
    int rc = hl_migrate_status(&handle, &vfs, &entries, &count);

    be->close(&handle);

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
        /* A migration's name comes from the app's tree (or its database's
         * _hull_migrations), so it is printed escaped (audit 31). */
        fputs(entries[i].applied ? "  [x] " : "  [ ] ", stdout);
        hl_log_fputs_escaped(stdout, entries[i].name);
        if (entries[i].applied) {
            fputs("  (", stdout);
            hl_log_fputs_escaped(stdout, entries[i].applied_at
                                         ? entries[i].applied_at : "?");
            fputs(")\n", stdout);
        } else {
            fputs("  (pending)\n", stdout);
        }
    }

    hl_migrate_status_free(entries, count);
    return 0;
}

/* ── Command entry point ──────────────────────────────────────────── */

int hl_cmd_migrate(int argc, char **argv, const HlCommandEnv *env)
{
    const char *app_dir = env->app_dir;
    const char *db_path = NULL;   /* default: <app_dir>/data.db (below) */
    const char *subcmd = NULL;
    int tui = 0;
    int keep_internal = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            migrate_usage();
            return 0;
        } else if (strcmp(argv[i], "--tui") == 0) {
            tui = 1;
        } else if (strcmp(argv[i], "--keep-internal-in-app-db") == 0) {
            keep_internal = 1;
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

    /* The app's database, as `hull agent migrate` / `agent db` open it: the
     * default was "data.db" in the WORKING directory, so `hull migrate
     * myapp` migrated (or created) ./data.db rather than myapp/data.db. An
     * app whose manifest names its default (databases.named.default - a
     * "@state/" database, gap 3) is migrated there: the app is loaded to read
     * it, as the internal-table move loads it. */
    char default_db[4096];
    int db_from_manifest = 0;
    if (!db_path && has_entry(app_dir ? app_dir : ".")) {
        HlAppContextOpts opts = {
            .app_dir = app_dir ? app_dir : ".",
            .no_migrate = 1,
            .gate_modules = 1,
            .manifest_default_db = 1,
            .keep_internal_in_app_db = keep_internal,
        };
        HlAppContext *ctx = NULL;
        if (hl_app_context_init(&ctx, &opts) != 0) {
            fprintf(stderr, "hull migrate: cannot load the app to read its manifest\n");
            return 1;
        }
        const char *d = hl_app_context_default_db_path(ctx);
        if (d) {
            int n = snprintf(default_db, sizeof default_db, "%s", d);
            if (n > 0 && (size_t)n < sizeof default_db) {
                db_path = default_db;
                db_from_manifest = 1;
            }
        }
        hl_app_context_free(ctx);
    }
    if (!db_path) {
        int n = snprintf(default_db, sizeof default_db, "%s/data.db",
                         app_dir ? app_dir : ".");
        if (n < 0 || (size_t)n >= sizeof default_db) {
            fprintf(stderr, "hull migrate: app directory path too long\n");
            return 1;
        }
        db_path = default_db;
        /* Only the implicit default: a committed data.db symlink pointed
         * this (unsandboxed) command at another database (audit 25). */
        char why[PATH_MAX + 160];
        if (hl_default_db_path_check(db_path, why, sizeof why) != 0) {
            fprintf(stderr, "hull migrate: %s\n", why);
            return 1;
        }
    }

    /* Status subcommand */
    if (subcmd && strcmp(subcmd, "status") == 0) {
#ifdef HL_TUI_LINKED
        if (tui) {
            if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
                fprintf(stderr,
                    "hull migrate status --tui: not attached to a "
                    "terminal. Run without --tui for plain text.\n");
                return 1;
            }
            return hull_tool("hull.migrate_status_tui", argc, argv,
                             env->hull_exe);
        }
#else
        if (tui) {
            fprintf(stderr, "hull migrate status --tui: this build was "
                            "compiled without HL_ENABLE_TUI.\n");
            return 1;
        }
#endif
        return cmd_status(app_dir, db_path);
    }

    /* Default: run migrations, then the internal-table move a start
     * without --no-migrate would make. */
    int rc = cmd_run(app_dir, db_path);
    if (rc != 0) return rc;
    return cmd_move_internal(app_dir ? app_dir : ".", db_path, keep_internal,
                             db_from_manifest);
}

#endif /* HL_ENABLE_DB */
