/*
 * commands/agent.c — hull agent: AI-native development tooling (CLI wrapper)
 *
 * Thin CLI dispatcher that parses arguments and delegates to the
 * transport-agnostic agent library (agent_lib.c). All JSON generation
 * happens in the library; this file only handles argv parsing and
 * writing the result to stdout.
 *
 * Subcommands:
 *   hull agent routes [app_dir]            — list registered routes
 *   hull agent db schema [app_dir]         — introspect DB schema
 *   hull agent db query "SQL" [app_dir]    — run read-only query
 *   hull agent request METHOD PATH [opts]  — HTTP request to dev server
 *   hull agent status                      — dev server status
 *   hull agent errors                      — structured errors from last reload
 *   hull agent test [app_dir]              — run tests
 *   hull agent context --task=T --level=L  — task-relevant documentation
 *
 * All output is JSON to stdout. Errors go to stderr.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/agent.h"
#include "hull/agent_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Output helper ─────────────────────────────────────────────────── */

static int output_result(ShJsonBuf *buf, int rc)
{
    if (buf->buf) {
        fputs(buf->buf, stdout);
        putchar('\n');
    }
    sh_json_buf_free(buf);
    return rc < 0 ? 1 : rc;
}

/* ── Subcommand handlers ───────────────────────────────────────────── */

static int cmd_routes(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-')
        app_dir = argv[0];

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_routes(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_db_schema(int argc, char **argv)
{
    const char *app_dir = ".";
    const char *db_path = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            db_path = argv[++i];
        else if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_db_schema(app_dir, db_path, &out);
    return output_result(&out, rc);
}

static int cmd_db_query(int argc, char **argv)
{
    const char *sql = NULL;
    const char *app_dir = ".";
    const char *db_path = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            db_path = argv[++i];
        else if (argv[i][0] != '-' && !sql)
            sql = argv[i];
        else if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    if (!sql) {
        fprintf(stderr, "hull agent db query: SQL argument required\n");
        return 1;
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_db_query(app_dir, db_path, sql, &out);
    return output_result(&out, rc);
}

static int cmd_db(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: hull agent db <schema|query> [args]\n");
        return 1;
    }

    if (strcmp(argv[0], "schema") == 0)
        return cmd_db_schema(argc - 1, argv + 1);
    if (strcmp(argv[0], "query") == 0)
        return cmd_db_query(argc - 1, argv + 1);

    fprintf(stderr, "hull agent db: unknown subcommand '%s'\n", argv[0]);
    return 1;
}

static int cmd_request(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: hull agent request METHOD PATH "
                "[-p port] [-d body] [-H header]\n");
        return 1;
    }

    const char *method = argv[0];
    const char *path = argv[1];
    int port = 3000;
    const char *body = NULL;
    const char *headers[32];
    int header_count = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            body = argv[++i];
        else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            if (header_count < 32)
                headers[header_count++] = argv[++i];
            else {
                ++i;
                fprintf(stderr, "hull agent: max 32 headers, ignoring: %s\n", argv[i]);
            }
        }
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_request(method, path, port, body, headers, header_count, &out);
    return output_result(&out, rc);
}

static int cmd_status(int argc, char **argv)
{
    int port = 3000;
    const char *app_dir = ".";

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = (int)strtol(argv[++i], NULL, 10);
        else if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_status(app_dir, port, &out);
    return output_result(&out, rc);
}

static int cmd_errors(int argc, char **argv)
{
    const char *app_dir = ".";
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_errors(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_test(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-')
        app_dir = argv[0];

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_test(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_context(int argc, char **argv)
{
    const char *task = NULL;
    const char *level = "compact";

    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--task=", 7) == 0)
            task = argv[i] + 7;
        else if (strncmp(argv[i], "--level=", 8) == 0)
            level = argv[i] + 8;
        else if (strcmp(argv[i], "--task") == 0 && i + 1 < argc)
            task = argv[++i];
        else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc)
            level = argv[++i];
        else if (argv[i][0] != '-' && !task)
            task = argv[i];
    }

    if (!task) {
        fprintf(stderr, "Usage: hull agent context --task=TASK [--level=LEVEL]\n"
                "  Tasks: auth, db, middleware, templates, routing, testing,\n"
                "         build, deploy, search, i18n, webhooks, validation\n"
                "  Levels: minimal, compact (default), full\n");
        return 1;
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_context(task, level, &out);
    return output_result(&out, rc);
}

static int cmd_deploy(int argc, char **argv)
{
    const char *app_dir = ".";
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_deploy(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_migrate(int argc, char **argv)
{
    const char *app_dir = ".";
    const char *db_path = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            db_path = argv[++i];
        else if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_migrate_status(app_dir, db_path, &out);
    return output_result(&out, rc);
}

/* ── Usage ────────────────────────────────────────────────────────── */

static void agent_usage(void)
{
    fprintf(stderr,
        "Usage: hull agent <subcommand> [args]\n"
        "\n"
        "Subcommands:\n"
        "  routes [app_dir]              List registered routes as JSON\n"
        "  db schema [app_dir] [-d path] Introspect database schema\n"
        "  db query \"SQL\" [app_dir]      Run read-only SQL query\n"
        "  request METHOD PATH [opts]    HTTP request to dev server\n"
        "  status [app_dir] [-p port]    Check dev server status\n"
        "  errors [app_dir]              Show structured errors\n"
        "  test [app_dir]                Run tests\n"
        "  context --task=T [--level=L]  Task-relevant documentation\n"
        "  migrate [app_dir] [-d path]   Migration status\n"
        "  deploy [app_dir]              Deployment readiness analysis\n"
        "\n"
        "All output is JSON to stdout.\n");
}

/* ── Command entry point ──────────────────────────────────────────── */

int hl_cmd_agent(int argc, char **argv, const HlCommandEnv *env)
{
    if (argc < 2) {
        agent_usage();
        return 1;
    }

    const char *sub = argv[1];
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;

    if (strcmp(sub, "routes") == 0)
        return cmd_routes(sub_argc, sub_argv);
    if (strcmp(sub, "db") == 0)
        return cmd_db(sub_argc, sub_argv);
    if (strcmp(sub, "request") == 0)
        return cmd_request(sub_argc, sub_argv);
    if (strcmp(sub, "status") == 0)
        return cmd_status(sub_argc, sub_argv);
    if (strcmp(sub, "errors") == 0)
        return cmd_errors(sub_argc, sub_argv);
    if (strcmp(sub, "test") == 0)
        return cmd_test(sub_argc, sub_argv, env);
    if (strcmp(sub, "context") == 0)
        return cmd_context(sub_argc, sub_argv);
    if (strcmp(sub, "migrate") == 0)
        return cmd_migrate(sub_argc, sub_argv);
    if (strcmp(sub, "deploy") == 0)
        return cmd_deploy(sub_argc, sub_argv);
    if (strcmp(sub, "help") == 0 || strcmp(sub, "--help") == 0 ||
        strcmp(sub, "-h") == 0) {
        agent_usage();
        return 0;
    }

    fprintf(stderr, "hull agent: unknown subcommand '%s'\n", sub);
    agent_usage();
    return 1;
}
