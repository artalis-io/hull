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
 *   hull agent auth-status [--path PATH]   — auth-stack health probe
 *                                            (proxies hull/web/auth-health)
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
#include "hull/sbom.h"
#include "hull/tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <sh_json.h>
#include <sh_arena.h>

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

/* hull agent auth-status [-p PORT] [--path PATH]
 *
 * Thin wrapper over `hull agent request GET <path>`. Default path
 * is /admin/auth-status, matching hull/web/auth-health's default
 * routes() mount point. Apps wire that handler in their startup;
 * this command just fetches + pretty-prints the JSON for AI/ops use. */
static int cmd_auth_status(int argc, char **argv)
{
    int port = 3000;
    const char *path = "/admin/auth-status";

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char *e;
            long v = strtol(argv[++i], &e, 10);
            if (*e != '\0' || v < 1 || v > 65535) {
                fprintf(stderr, "hull agent auth-status: "
                                "invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)v;
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            path = argv[++i];
        }
    }

    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_request("GET", path, port, NULL, NULL, 0, &out);
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

static int cmd_errors(int argc, char **argv, const HlCommandEnv *env)
{
    const char *app_dir = ".";
    int tui = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--tui") == 0) { tui = 1; continue; }
        if (argv[i][0] != '-')
            app_dir = argv[i];
    }

#ifdef HL_TUI_LINKED
    if (tui) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            fprintf(stderr, "hull agent errors --tui: not attached to a "
                            "terminal (stdin/stdout redirected). Run "
                            "without --tui to get JSON.\n");
            return 1;
        }
        return hull_tool("hull.agent_errors_tui", argc, argv, env->hull_exe);
    }
#else
    if (tui) {
        fprintf(stderr, "hull agent errors --tui: this build was compiled "
                        "without HL_ENABLE_TUI.\n");
        return 1;
    }
#endif
    (void)env;

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

static int cmd_context(int argc, char **argv, const HlCommandEnv *env)
{
    const char *task = NULL;
    const char *level = "compact";
    int interactive = 0;
    int list_mode = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--interactive") == 0 ||
            strcmp(argv[i], "--tui") == 0) {
            interactive = 1;
            continue;
        }
        if (strcmp(argv[i], "--list") == 0) {
            list_mode = 1;
            continue;
        }
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

    /* --list short-circuits everything else — it enumerates the
     * embedded context: registry so cold-start agents discover the
     * task set without scraping. */
    if (list_mode) {
        ShJsonBuf out;
        sh_json_buf_init(&out);
        int rc = hl_agent_context_list(&out);
        return output_result(&out, rc);
    }

#ifdef HL_TUI_LINKED
    if (interactive) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            fprintf(stderr,
                "hull agent context --interactive: not attached to a "
                "terminal. Use --task=NAME instead.\n");
            return 1;
        }
        return hull_tool("hull.agent_context_tui", argc, argv, env->hull_exe);
    }
#else
    if (interactive) {
        fprintf(stderr, "hull agent context --interactive: this build was "
                        "compiled without HL_ENABLE_TUI.\n");
        return 1;
    }
#endif
    (void)env;

    if (!task) {
        fprintf(stderr,
            "Usage: hull agent context --task=TASK [--level=LEVEL]\n"
            "       hull agent context --list   (enumerate available tasks)\n"
            "  Levels: minimal, compact (default), full\n"
            "  Try --interactive (--tui) for a TUI picker.\n");
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

/* ── Extended introspection subcommands ─────── */

#include "hull/app_context.h"

/* Open a warm app context for the duration of a single subcommand. */
static HlAppContext *open_warm_ctx(const char *app_dir)
{
    /* Agent commands that run app code (eval, request, scaffold)
     * inherit the app's declared module surface — same gate the server
     * applies. Read-only introspection (manifest, routes, schema, ...)
     * never triggers require/import so the gate is a no-op there. */
    HlAppContextOpts opts = { .app_dir = app_dir, .gate_modules = 1 };
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

static int cmd_tools_sub(int argc, char **argv)
{
    /* No app context needed — the registry + install state are
     * host-level concerns independent of any particular app. */
    (void)argc; (void)argv;
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_tools(&out);
    return output_result(&out, rc);
}

static int cmd_overview(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_overview(app_dir, &out);
    return output_result(&out, rc);
}

static int cmd_modules_sub(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-') app_dir = argv[0];
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_modules(app_dir, &out);
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
        "  context --list                 Enumerate available context tasks\n"
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
        "  tools                          Side-loaded tool registry + install state\n"
        "  overview [app_dir]             Composite project summary (one shot)\n"
        "  inspect [app_dir]              Analyzed project model: annotated declarations (JSON)\n"
        "\n"
        "All output is JSON to stdout.\n");
}

/* ── hull agent inspect ────────────────────────────────────────────────
 *
 * Read a text field's integer value from a small JSON blob (crude, matching
 * hl_agent_status's dev.json port parse): "<key>"<ws>:<ws><digits>. Returns -1 if absent.
 */
static long json_int_field(const char *buf, const char *key)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) return -1;
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p < '0' || *p > '9') return -1;              /* must start with a digit (no sign / garbage) */
    errno = 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (errno != 0 || end == p) return -1;            /* overflow / no digits consumed */
    /* the numeric token must terminate on a valid JSON boundary (not e.g. "12x") */
    if (*end != ',' && *end != '}' && *end != ' ' && *end != '\t' &&
        *end != '\n' && *end != '\r' && *end != '\0') return -1;
    if (v <= 0 || v > INT_MAX) return -1;             /* a pid fits in a positive int */
    return v;
}

/* Read a whole file into a NUL-terminated malloc'd buffer (cap-bounded), or NULL. Requires
 * a COMPLETE read (short read / read error -> NULL) so a truncated sidecar is rejected
 * rather than parsed. On success `*out_len` (if non-NULL) is the exact byte length -- pass
 * it (not strlen) to the parser + emitter so an embedded NUL cannot hide trailing bytes. */
static char *read_whole_file(const char *path, size_t cap, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > cap) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    int bad = (got != (size_t)sz) || ferror(f);       /* demand the full file, no read error */
    fclose(f);
    if (bad) { free(buf); return NULL; }
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* Validate that `buf` (of `len` bytes) is a COMPLETE, well-formed JSON document via the
 * repo's parser. A truncated / corrupt discovery.json -- even one whose session_pid token
 * happens to match a live PID -- must never be streamed. Returns 1 iff the whole document
 * parses; 0 otherwise (incl. OOM, which fails closed to a standalone re-analysis). */
static int json_document_valid(const char *buf, size_t len)
{
    SHArena *arena = sh_arena_create(len * 8 + 4096);   /* generous; a parse failure only costs a fresh analysis */
    if (!arena) return 0;
    ShJsonValue *root = NULL;
    int ok = (sh_json_parse(buf, len, arena, &root) == SH_JSON_OK) && root != NULL;
    sh_arena_free(arena);
    return ok;
}

/* If a LIVE dev session has published a discovery generation for `app_dir`, stream it to
 * stdout and return 0. "Live" (D9): both .hull/discovery.json and .hull/dev.json exist,
 * their session_pid values match, that supervisor PID is still alive (kill(pid,0)), AND
 * the discovery document parses as complete valid JSON. Any mismatch / dead session /
 * missing file / malformed-or-truncated document -> -1 (caller falls back to standalone).
 * This rejects a stale/cross-reload sidecar and a corrupt/partial one. */
static int agent_inspect_stream_live(const char *app_dir)
{
    char disc_path[PATH_MAX], dev_path[PATH_MAX];
    int a = snprintf(disc_path, sizeof(disc_path), "%s/.hull/discovery.json", app_dir);
    int b = snprintf(dev_path, sizeof(dev_path), "%s/.hull/dev.json", app_dir);
    if (a < 0 || (size_t)a >= sizeof(disc_path) || b < 0 || (size_t)b >= sizeof(dev_path))
        return -1;

    char *dev = read_whole_file(dev_path, 64 * 1024, NULL);
    if (!dev) return -1;
    long sp_dev = json_int_field(dev, "session_pid");
    free(dev);
    if (sp_dev <= 0) return -1;

    size_t disc_len = 0;
    char *disc = read_whole_file(disc_path, 64 * 1024 * 1024, &disc_len);   /* 64 MB cap */
    if (!disc) return -1;
    long sp_disc = json_int_field(disc, "session_pid");

    /* Bind the published generation to a specific live dev session. */
    if (sp_disc != sp_dev || kill((pid_t)sp_disc, 0) != 0) { free(disc); return -1; }

    /* Only emit a COMPLETE, well-formed document. Validate + emit over the EXACT byte
     * length (not strlen): an embedded NUL must not hide trailing bytes, so `{valid}\0junk`
     * -- whose complete on-disk document is invalid -- falls back to standalone. */
    if (!json_document_valid(disc, disc_len)) { free(disc); return -1; }

    fwrite(disc, 1, disc_len, stdout);
    free(disc);
    return 0;
}

/* `hull agent inspect [app_dir]` (+ the INTERNAL publish flags used by `hull dev`).
 * The live-published fast path (D9) is taken ONLY for a fully-valid public read
 * invocation -- at most one positional, no flag other than --json. Any invalid or
 * non-read form (a second root, an unknown flag, --help) is NOT fast-pathed; it is
 * delegated to hull.project.inspect so the Lua boundary does the full validation (exit 2 /
 * usage). The internal `--generation`/`--session-pid` publish flags route to the SEPARATE
 * hull.project.publish module. `argv` is hl_cmd_agent's own (argv[1]=="inspect"), so
 * argv+1 keeps arg[0]="inspect" in the VM. */
static int cmd_inspect(int argc, char **argv, const HlCommandEnv *env)
{
    const char *app_dir = ".";
    int positionals = 0, has_publish = 0, clean_read = 1;
    for (int i = 2; i < argc; i++) {          /* argv[1]=="inspect"; args start at 2 */
        const char *a = argv[i];
        if (strncmp(a, "--generation=", 13) == 0 || strncmp(a, "--session-pid=", 14) == 0) {
            has_publish = 1; clean_read = 0;
        } else if (strcmp(a, "--json") == 0) {
            /* accepted read format; stays a clean read */
        } else if (a[0] == '-' && strcmp(a, "-") != 0) {
            clean_read = 0;                    /* --help / unknown flag -> let Lua handle it */
        } else {
            positionals++; app_dir = a;
        }
    }
    if (positionals > 1) clean_read = 0;       /* extra roots -> Lua rejects (exit 2) */

    if (has_publish)
        return hull_tool("hull.project.publish", argc - 1, argv + 1,
                         env ? env->hull_exe : NULL);

    if (clean_read && agent_inspect_stream_live(app_dir) == 0)
        return 0;                              /* served the live published generation */

    return hull_tool("hull.project.inspect", argc - 1, argv + 1,
                     env ? env->hull_exe : NULL);
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
    if (strcmp(sub, "auth-status") == 0)
        return cmd_auth_status(sub_argc, sub_argv);
    if (strcmp(sub, "errors") == 0)
        return cmd_errors(sub_argc, sub_argv, env);
    if (strcmp(sub, "test") == 0)
        return cmd_test(sub_argc, sub_argv, env);
    if (strcmp(sub, "context") == 0)
        return cmd_context(sub_argc, sub_argv, env);
#ifdef HL_ENABLE_DB
    if (strcmp(sub, "migrate") == 0)
        return cmd_migrate(sub_argc, sub_argv);
#endif
    if (strcmp(sub, "deploy") == 0)
        return cmd_deploy(sub_argc, sub_argv);
    /* Extended introspection */
    if (strcmp(sub, "manifest") == 0)     return cmd_manifest(sub_argc, sub_argv);
    if (strcmp(sub, "vfs") == 0)          return cmd_vfs(sub_argc, sub_argv);
    if (strcmp(sub, "compute") == 0)      return cmd_compute_sub(sub_argc, sub_argv);
    if (strcmp(sub, "gpu") == 0)          return cmd_gpu_sub(sub_argc, sub_argv);
    if (strcmp(sub, "capabilities") == 0) return cmd_capabilities(sub_argc, sub_argv);
    if (strcmp(sub, "modules") == 0)      return cmd_modules_sub(sub_argc, sub_argv);
    if (strcmp(sub, "validate") == 0)     return cmd_validate(sub_argc, sub_argv);
    if (strcmp(sub, "logs") == 0)         return cmd_logs(sub_argc, sub_argv);
    if (strcmp(sub, "endpoint") == 0)     return cmd_endpoint(sub_argc, sub_argv);
    if (strcmp(sub, "middleware") == 0)   return cmd_middleware(sub_argc, sub_argv);
    if (strcmp(sub, "eval") == 0)         return cmd_eval(sub_argc, sub_argv);
    if (strcmp(sub, "perf") == 0)         return cmd_perf(sub_argc, sub_argv);
    if (strcmp(sub, "template") == 0)     return cmd_template(sub_argc, sub_argv);
    if (strcmp(sub, "compute-call") == 0) return cmd_compute_call(sub_argc, sub_argv);
    /* Tool registry + install state — independent of HL_ENABLE_HTTP_CLIENT.
     * Agents on CLI-flavor builds can still see the registry; the install
     * command itself is the only thing gated by HTTP_CLIENT. */
    if (strcmp(sub, "tools") == 0)        return cmd_tools_sub(sub_argc, sub_argv);
    /* SBOM. Same data as `hull sbom --format=json`, no app_dir argument.
     * Independent of capability layer; pure compile-time data. */
    if (strcmp(sub, "sbom") == 0) {
        (void)sub_argc; (void)sub_argv;
        if (env && env->hull_exe) hl_sbom_set_binary_path(env->hull_exe);
        return hl_sbom_format(HL_SBOM_JSON, stdout) == 0 ? 0 : 1;
    }
    /* Composite project summary — one call to orient an agent. */
    if (strcmp(sub, "overview") == 0)     return cmd_overview(sub_argc, sub_argv);
    /* Project source discovery — the canonical analyzed project model (annotated
     * declarations) as versioned JSON. A live dev session's published generation is
     * streamed directly (D9); otherwise the tool VM analyzes (standalone / publish). */
    if (strcmp(sub, "inspect") == 0)
        return cmd_inspect(argc, argv, env);
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
