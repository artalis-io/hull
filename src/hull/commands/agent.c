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

#ifdef HL_ENABLE_DB
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
#endif /* HL_ENABLE_DB */

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
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            /* L2: strict port parse — silent strtol(s, NULL, 10) lets
             * "abc" become 0 and the request would fail downstream with
             * a confusing error. */
            char *e;
            long v = strtol(argv[++i], &e, 10);
            if (*e != '\0' || v < 1 || v > 65535) {
                fprintf(stderr, "hull agent: invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)v;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
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
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char *e;
            long v = strtol(argv[++i], &e, 10);
            if (*e != '\0' || v < 1 || v > 65535) {
                fprintf(stderr, "hull agent status: invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)v;
        } else if (argv[i][0] != '-')
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

#ifdef HL_ENABLE_DB
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
#endif

/* ── Phase 6 (2026-05-15): Extended introspection subcommands ─────── */

#include "hull/app_context.h"

/* Open a warm app context for the duration of a single subcommand. */
static HlAppContext *open_warm_ctx(const char *app_dir)
{
    HlAppContextOpts opts = { .app_dir = app_dir };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0) return NULL;
    return ctx;
}

static int cmd_manifest(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_manifest(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_vfs(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_vfs(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_compute_sub(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_compute(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_gpu_sub(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_gpu(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_capabilities(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_capabilities(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_validate(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "hull agent validate: file path required\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_validate(argv[0], &out);
    return output_result(&out, rc);
}

static int cmd_logs(int argc, char **argv)
{
    const char *app_dir = ".";
    int tail_n = 100;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            char *e;
            long v = strtol(argv[++i], &e, 10);
            if (*e == '\0' && v > 0 && v <= 10000) tail_n = (int)v;
        } else if (argv[i][0] != '-') {
            app_dir = argv[i];
        }
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_logs(app_dir, tail_n, &out);
    return output_result(&out, rc);
}

static int cmd_endpoint(int argc, char **argv)
{
    const char *app_dir = ".";
    const char *method = NULL, *path = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (!method)      method = argv[i];
        else if (!path)   path   = argv[i];
        else              app_dir = argv[i];
    }
    if (!method || !path) {
        fprintf(stderr, "hull agent endpoint: METHOD and PATH required\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_endpoint(app_dir, method, path, &out);
    return output_result(&out, rc);
}

static int cmd_middleware(int argc, char **argv)
{
    const char *app_dir = ".";
    const char *method = NULL, *path = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (!method)      method = argv[i];
        else if (!path)   path   = argv[i];
        else              app_dir = argv[i];
    }
    if (!method || !path) {
        fprintf(stderr, "hull agent middleware: METHOD and PATH required\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_middleware(app_dir, method, path, &out);
    return output_result(&out, rc);
}

static int cmd_eval(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "hull agent eval: code argument required\n");
        return 1;
    }
    const char *code = argv[0];
    const char *app_dir = ".";
    if (argc >= 2 && argv[1][0] != '-') app_dir = argv[1];

    HlAppContext *ctx = open_warm_ctx(app_dir);
    if (!ctx) {
        fprintf(stderr, "hull agent eval: failed to load app\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_eval_ctx(ctx, code, &out);
    hl_app_context_free(ctx);
    return output_result(&out, rc);
}

static int cmd_perf(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    HlAppContext *ctx = open_warm_ctx(app_dir);
    if (!ctx) {
        fprintf(stderr, "hull agent perf: failed to load app\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_perf_ctx(ctx, &out);
    hl_app_context_free(ctx);
    return output_result(&out, rc);
}

static int cmd_template(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "hull agent template: template name required\n");
        return 1;
    }
    const char *name = argv[0];
    const char *data_path = NULL;
    const char *app_dir = ".";
    if (argc >= 2 && argv[1][0] != '-') data_path = argv[1];
    if (argc >= 3 && argv[2][0] != '-') app_dir = argv[2];

    char *data_json = NULL;
    if (data_path) {
        FILE *f = fopen(data_path, "rb");
        if (!f) { fprintf(stderr, "hull agent template: cannot open data file\n"); return 1; }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
        long n = ftell(f);
        if (n < 0 || n > 1024 * 1024) { fclose(f); return 1; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }
        data_json = malloc((size_t)n + 1);
        if (!data_json) { fclose(f); return 1; }
        size_t got = fread(data_json, 1, (size_t)n, f);
        int err = ferror(f);
        fclose(f);
        if (err || got != (size_t)n) { free(data_json); return 1; }
        data_json[got] = '\0';
    }

    HlAppContext *ctx = open_warm_ctx(app_dir);
    if (!ctx) {
        free(data_json);
        fprintf(stderr, "hull agent template: failed to load app\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_template_ctx(ctx, name, data_json ? data_json : "{}", &out);
    hl_app_context_free(ctx);
    free(data_json);
    return output_result(&out, rc);
}

static int cmd_compute_call(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "hull agent compute-call: module-name and input-file required\n");
        return 1;
    }
    const char *module = argv[0];
    const char *input  = argv[1];
    const char *app_dir = ".";
    if (argc >= 3 && argv[2][0] != '-') app_dir = argv[2];

    HlAppContext *ctx = open_warm_ctx(app_dir);
    if (!ctx) {
        fprintf(stderr, "hull agent compute-call: failed to load app\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_compute_call_ctx(ctx, module, input, &out);
    hl_app_context_free(ctx);
    return output_result(&out, rc);
}

#ifdef HL_ENABLE_DB
static int cmd_schema_diff(int argc, char **argv)
{
    const char *app_dir = ".";
    const char *db_path = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) db_path = argv[++i];
        else if (argv[i][0] != '-') app_dir = argv[i];
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_schema_diff(app_dir, db_path, &out);
    return output_result(&out, rc);
}

static int cmd_sql(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[0], "named") != 0) {
        fprintf(stderr, "hull agent sql: expected 'sql named <query-name>'\n");
        return 1;
    }
    const char *qname = argv[1];
    const char *params_json = NULL;
    const char *app_dir = ".";
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--params") == 0 && i + 1 < argc) params_json = argv[++i];
        else if (argv[i][0] != '-') app_dir = argv[i];
    }
    HlAppContext *ctx = open_warm_ctx(app_dir);
    if (!ctx) {
        fprintf(stderr, "hull agent sql: failed to load app\n");
        return 1;
    }
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_sql_named_ctx(ctx, qname, params_json, &out);
    hl_app_context_free(ctx);
    return output_result(&out, rc);
}
#endif

/* ── Usage ────────────────────────────────────────────────────────── */

static void agent_usage(void)
{
    fprintf(stderr,
        "Usage: hull agent <subcommand> [args]\n"
        "\n"
        "Subcommands:\n"
        "  routes [app_dir]               List registered routes as JSON\n"
        "  db schema [app_dir] [-d path]  Introspect database schema\n"
        "  db query \"SQL\" [app_dir]       Run read-only SQL query\n"
        "  request METHOD PATH [opts]     HTTP request to dev server\n"
        "  status [app_dir] [-p port]     Check dev server status\n"
        "  errors [app_dir]               Show structured errors\n"
        "  test [app_dir]                 Run tests\n"
        "  context --task=T [--level=L]   Task-relevant documentation\n"
        "  migrate [app_dir] [-d path]    Migration status\n"
        "  deploy [app_dir]               Deployment readiness analysis\n"
        "\n"
        "Extended introspection:\n"
        "  manifest [app_dir]             Effective manifest JSON\n"
        "  endpoint METHOD PATH [dir]     Preview which handler+middleware would fire\n"
        "  middleware METHOD PATH [dir]   Just the matching middleware stack\n"
        "  capabilities [app_dir]         Source-walk vs manifest analysis\n"
        "  vfs [app_dir]                  List embedded files (path,size,bucket)\n"
        "  compute [app_dir]              List WASM compute modules\n"
        "  gpu [app_dir]                  List WGSL shaders + GPU availability\n"
        "  perf [app_dir]                 Runtime stats snapshot\n"
        "  logs [app_dir] [--tail N]      Last N log lines from .hull/dev.log\n"
        "  validate <file>                Parse+sandbox-check a Lua/JS file\n"
        "  eval <code> [app_dir]          Run a one-shot snippet, return JSON\n"
        "  template <name> [data.json] [dir]  Render a template with data\n"
        "  compute-call <mod> <in> [dir]  Invoke a WASM module against a file\n"
        "  schema-diff [app_dir] [-d p]   DB schema drift analysis\n"
        "  sql named <qname> [--params J] [dir]  Run a named query from queries.json\n"
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
#ifdef HL_ENABLE_DB
    if (strcmp(sub, "db") == 0)
        return cmd_db(sub_argc, sub_argv);
#endif
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
#ifdef HL_ENABLE_DB
    if (strcmp(sub, "migrate") == 0)
        return cmd_migrate(sub_argc, sub_argv);
#endif
    if (strcmp(sub, "deploy") == 0)
        return cmd_deploy(sub_argc, sub_argv);
    /* Phase 6 extended introspection */
    if (strcmp(sub, "manifest") == 0)     return cmd_manifest(sub_argc, sub_argv);
    if (strcmp(sub, "vfs") == 0)          return cmd_vfs(sub_argc, sub_argv);
    if (strcmp(sub, "compute") == 0)      return cmd_compute_sub(sub_argc, sub_argv);
    if (strcmp(sub, "gpu") == 0)          return cmd_gpu_sub(sub_argc, sub_argv);
    if (strcmp(sub, "capabilities") == 0) return cmd_capabilities(sub_argc, sub_argv);
    if (strcmp(sub, "validate") == 0)     return cmd_validate(sub_argc, sub_argv);
    if (strcmp(sub, "logs") == 0)         return cmd_logs(sub_argc, sub_argv);
    if (strcmp(sub, "endpoint") == 0)     return cmd_endpoint(sub_argc, sub_argv);
    if (strcmp(sub, "middleware") == 0)   return cmd_middleware(sub_argc, sub_argv);
    if (strcmp(sub, "eval") == 0)         return cmd_eval(sub_argc, sub_argv);
    if (strcmp(sub, "perf") == 0)         return cmd_perf(sub_argc, sub_argv);
    if (strcmp(sub, "template") == 0)     return cmd_template(sub_argc, sub_argv);
    if (strcmp(sub, "compute-call") == 0) return cmd_compute_call(sub_argc, sub_argv);
#ifdef HL_ENABLE_DB
    if (strcmp(sub, "schema-diff") == 0)  return cmd_schema_diff(sub_argc, sub_argv);
    if (strcmp(sub, "sql") == 0)          return cmd_sql(sub_argc, sub_argv);
#endif
    if (strcmp(sub, "help") == 0 || strcmp(sub, "--help") == 0 ||
        strcmp(sub, "-h") == 0) {
        agent_usage();
        return 0;
    }

    fprintf(stderr, "hull agent: unknown subcommand '%s'\n", sub);
    agent_usage();
    return 1;
}
