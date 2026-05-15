/*
 * commands/mcp.c — hull mcp serve: MCP stdio JSON-RPC 2.0 server
 *
 * Implements the Model Context Protocol for AI coding tools (Cursor,
 * Claude Code, Windsurf, OpenCode). Reads JSON-RPC requests from stdin,
 * dispatches to the agent library, writes responses to stdout.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/mcp.h"
#include "hull/agent_lib.h"
#include "hull/app_context.h"

#include <sh_json.h>
#include <sh_arena.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_MAX_LINE (64 * 1024)

/* ── Tool schema definitions (static JSON) ─────────────────────────── */

static const char SCHEMA_ROUTES[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"}}}";

static const char SCHEMA_DB_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"},"
    "\"db_path\":{\"type\":\"string\",\"description\":\"Database file path\"}}}";

static const char SCHEMA_QUERY[] =
    "{\"type\":\"object\",\"properties\":{\"sql\":{\"type\":\"string\","
    "\"description\":\"SQL query to execute\"},"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"},"
    "\"db_path\":{\"type\":\"string\",\"description\":\"Database file path\"}},"
    "\"required\":[\"sql\"]}";

static const char SCHEMA_REQUEST[] =
    "{\"type\":\"object\",\"properties\":{\"method\":{\"type\":\"string\","
    "\"description\":\"HTTP method (GET, POST, etc.)\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Request path\"},"
    "\"port\":{\"type\":\"integer\",\"description\":\"Server port (default: 3000)\"},"
    "\"body\":{\"type\":\"string\",\"description\":\"Request body\"}},"
    "\"required\":[\"method\",\"path\"]}";

static const char SCHEMA_STATUS[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"},"
    "\"port\":{\"type\":\"integer\",\"description\":\"Server port (default: 3000)\"}}}";

static const char SCHEMA_ERRORS[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"}}}";

static const char SCHEMA_TEST[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"}}}";

static const char SCHEMA_CONTEXT[] =
    "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\","
    "\"description\":\"Task keyword (auth, db, middleware, templates, routing, "
    "testing, build, deploy, search, i18n, webhooks, validation)\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"minimal\",\"compact\",\"full\"],"
    "\"description\":\"Detail level (default: compact)\"}},"
    "\"required\":[\"task\"]}";

static const char SCHEMA_MIGRATE[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"},"
    "\"db_path\":{\"type\":\"string\",\"description\":\"Database file path\"}}}";

static const char SCHEMA_RELOAD[] =
    "{\"type\":\"object\",\"properties\":{}}";

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;
} McpTool;

static const McpTool mcp_tools[] = {
    { "hull_routes",         "List registered routes and middleware",
                              SCHEMA_ROUTES },
#ifdef HL_ENABLE_DB
    { "hull_db_schema",      "Introspect database schema",
                              SCHEMA_DB_SCHEMA },
    { "hull_db_query",       "Run read-only SQL query",
                              SCHEMA_QUERY },
#endif
    { "hull_request",        "HTTP request to dev server",
                              SCHEMA_REQUEST },
    { "hull_status",         "Check dev server status",
                              SCHEMA_STATUS },
    { "hull_errors",         "Get structured errors from last reload",
                              SCHEMA_ERRORS },
    { "hull_test",           "Run application tests",
                              SCHEMA_TEST },
    { "hull_context",        "Get task-relevant documentation",
                              SCHEMA_CONTEXT },
#ifdef HL_ENABLE_DB
    { "hull_migrate_status", "Show migration status",
                              SCHEMA_MIGRATE },
#endif
    { "hull_reload",         "Reload application context (after code changes)",
                              SCHEMA_RELOAD },
    { NULL, NULL, NULL }
};

/* ── JSON-RPC response helpers ─────────────────────────────────────── */

static void write_jsonrpc_result(ShJsonWriter *w, const ShJsonValue *id,
                                 SHArena *arena)
{
    (void)arena;
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "jsonrpc", "2.0");

    sh_json_write_key(w, "id");
    if (!id || sh_json_is_null(id)) {
        sh_json_write_null(w);
    } else if (sh_json_type(id) == SH_JSON_NUMBER) {
        sh_json_write_int(w, sh_json_as_int(id, 0));
    } else {
        sh_json_write_string(w, sh_json_as_string(id, "null"));
    }
}

static void send_error(FILE *fp, const ShJsonValue *id, int code,
                       const char *message)
{
    ShJsonBuf buf;
    sh_json_buf_init(&buf);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &buf);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "jsonrpc", "2.0");

    sh_json_write_key(&w, "id");
    if (!id || sh_json_is_null(id))
        sh_json_write_null(&w);
    else if (sh_json_type(id) == SH_JSON_NUMBER)
        sh_json_write_int(&w, sh_json_as_int(id, 0));
    else
        sh_json_write_string(&w, sh_json_as_string(id, "null"));

    sh_json_write_key(&w, "error");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "code", code);
    sh_json_write_kv_string(&w, "message", message);
    sh_json_write_object_end(&w);
    sh_json_write_object_end(&w);

    if (buf.buf) {
        fputs(buf.buf, fp);
        fputc('\n', fp);
        fflush(fp);
    }
    sh_json_buf_free(&buf);
}

static void send_tool_result(FILE *fp, const ShJsonValue *id,
                             ShJsonBuf *agent_out)
{
    ShJsonBuf buf;
    sh_json_buf_init(&buf);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &buf);

    write_jsonrpc_result(&w, id, NULL);

    sh_json_write_key(&w, "result");
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "content");
    sh_json_write_array_start(&w);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "type", "text");

    /* Embed the agent JSON as the text value */
    sh_json_write_key(&w, "text");
    if (agent_out->buf)
        sh_json_write_string(&w, agent_out->buf);
    else
        sh_json_write_string(&w, "{}");

    sh_json_write_object_end(&w);
    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    sh_json_write_object_end(&w);

    if (buf.buf) {
        fputs(buf.buf, fp);
        fputc('\n', fp);
        fflush(fp);
    }
    sh_json_buf_free(&buf);
}

/* ── Method handlers ───────────────────────────────────────────────── */

static void handle_initialize(FILE *fp, const ShJsonValue *id)
{
    ShJsonBuf buf;
    sh_json_buf_init(&buf);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &buf);

    write_jsonrpc_result(&w, id, NULL);

    sh_json_write_key(&w, "result");
    sh_json_write_object_start(&w);

    sh_json_write_kv_string(&w, "protocolVersion", "2024-11-05");

    sh_json_write_key(&w, "capabilities");
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "tools");
    sh_json_write_object_start(&w);
    sh_json_write_object_end(&w);
    sh_json_write_object_end(&w);

    sh_json_write_key(&w, "serverInfo");
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "name", "hull");
    sh_json_write_kv_string(&w, "version", "0.1.0");
    sh_json_write_object_end(&w);

    sh_json_write_object_end(&w);
    sh_json_write_object_end(&w);

    if (buf.buf) {
        fputs(buf.buf, fp);
        fputc('\n', fp);
        fflush(fp);
    }
    sh_json_buf_free(&buf);
}

static void handle_tools_list(FILE *fp, const ShJsonValue *id)
{
    ShJsonBuf buf;
    sh_json_buf_init(&buf);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &buf);

    write_jsonrpc_result(&w, id, NULL);

    sh_json_write_key(&w, "result");
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "tools");
    sh_json_write_array_start(&w);

    for (const McpTool *t = mcp_tools; t->name; t++) {
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name", t->name);
        sh_json_write_kv_string(&w, "description", t->description);
        sh_json_write_key(&w, "inputSchema");
        sh_json_write_raw(&w, t->input_schema, strlen(t->input_schema));
        sh_json_write_object_end(&w);
    }

    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    sh_json_write_object_end(&w);

    if (buf.buf) {
        fputs(buf.buf, fp);
        fputc('\n', fp);
        fflush(fp);
    }
    sh_json_buf_free(&buf);
}

static void handle_tools_call(FILE *fp, const ShJsonValue *id,
                              const ShJsonValue *params, const char *app_dir,
                              HlAppContext **warm_ctx_ptr)
{
    const char *tool_name = sh_json_as_string(
        sh_json_get(params, "name"), "");
    const ShJsonValue *args = sh_json_get(params, "arguments");

    HlAppContext *warm_ctx = warm_ctx_ptr ? *warm_ctx_ptr : NULL;

    ShJsonBuf agent_out;
    sh_json_buf_init(&agent_out);

    if (strcmp(tool_name, "hull_routes") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_routes_ctx(warm_ctx, &agent_out);
        else
            hl_agent_routes(dir, &agent_out);

#ifdef HL_ENABLE_DB
    } else if (strcmp(tool_name, "hull_db_schema") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        const char *db = sh_json_as_string(sh_json_get(args, "db_path"), NULL);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_db_schema_ctx(warm_ctx, db, &agent_out);
        else
            hl_agent_db_schema(dir, db, &agent_out);

    } else if (strcmp(tool_name, "hull_db_query") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        const char *db = sh_json_as_string(sh_json_get(args, "db_path"), NULL);
        const char *sql = sh_json_as_string(sh_json_get(args, "sql"), NULL);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_db_query_ctx(warm_ctx, db, sql, &agent_out);
        else
            hl_agent_db_query(dir, db, sql, &agent_out);
#endif

    } else if (strcmp(tool_name, "hull_request") == 0) {
        const char *method = sh_json_as_string(sh_json_get(args, "method"), "GET");
        const char *path = sh_json_as_string(sh_json_get(args, "path"), "/");
        int port = sh_json_as_int(sh_json_get(args, "port"), 3000);
        const char *body = sh_json_as_string(sh_json_get(args, "body"), NULL);
        hl_agent_request(method, path, port, body, NULL, 0, &agent_out);

    } else if (strcmp(tool_name, "hull_status") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        int port = sh_json_as_int(sh_json_get(args, "port"), 3000);
        hl_agent_status(dir, port, &agent_out);

    } else if (strcmp(tool_name, "hull_errors") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        hl_agent_errors(dir, &agent_out);

    } else if (strcmp(tool_name, "hull_test") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_test_ctx(warm_ctx, &agent_out);
        else
            hl_agent_test(dir, &agent_out);

    } else if (strcmp(tool_name, "hull_context") == 0) {
        const char *task = sh_json_as_string(sh_json_get(args, "task"), NULL);
        const char *level = sh_json_as_string(sh_json_get(args, "level"), "compact");
        hl_agent_context(task, level, &agent_out);

#ifdef HL_ENABLE_DB
    } else if (strcmp(tool_name, "hull_migrate_status") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        const char *db = sh_json_as_string(sh_json_get(args, "db_path"), NULL);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_migrate_status_ctx(warm_ctx, db, &agent_out);
        else
            hl_agent_migrate_status(dir, db, &agent_out);
#endif

    } else if (strcmp(tool_name, "hull_reload") == 0) {
        if (warm_ctx_ptr) {
            if (*warm_ctx_ptr) {
                hl_app_context_free(*warm_ctx_ptr);
                *warm_ctx_ptr = NULL;
            }
            HlAppContextOpts opts = { .app_dir = app_dir };
            if (hl_app_context_init(warm_ctx_ptr, &opts) != 0)
                *warm_ctx_ptr = NULL;
        }
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, &agent_out);
        sh_json_write_object_start(&w);
        sh_json_write_kv_bool(&w, "ok", warm_ctx_ptr && *warm_ctx_ptr);
        sh_json_write_object_end(&w);

    } else {
        sh_json_buf_free(&agent_out);
        send_error(fp, id, -32602, "unknown tool");
        return;
    }

    send_tool_result(fp, id, &agent_out);
    sh_json_buf_free(&agent_out);
}

/* ── Main loop ─────────────────────────────────────────────────────── */

int hl_cmd_mcp(int argc, char **argv, const HlCommandEnv *env)
{
    /* Use env->app_dir as default; allow local --app-dir override */
    const char *app_dir = env->app_dir;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--app-dir") == 0 && i + 1 < argc)
            app_dir = argv[++i];
        else if (strncmp(argv[i], "--app-dir=", 10) == 0)
            app_dir = argv[i] + 10;
    }

    /* Initialize warm context for reuse across tool calls */
    HlAppContext *warm_ctx = NULL;
    HlAppContextOpts ctx_opts = { .app_dir = app_dir };
    if (hl_app_context_init(&warm_ctx, &ctx_opts) != 0)
        warm_ctx = NULL;  /* fallback to per-call init */

    char *line_buf = malloc(MCP_MAX_LINE);
    if (!line_buf) {
        hl_app_context_free(warm_ctx);
        return 1;
    }

    while (fgets(line_buf, MCP_MAX_LINE, stdin)) {
        /* Skip empty lines */
        size_t len = strlen(line_buf);
        while (len > 0 && (line_buf[len-1] == '\n' || line_buf[len-1] == '\r'))
            line_buf[--len] = '\0';
        if (len == 0) continue;

        /* Parse JSON-RPC request */
        SHArena *arena = sh_arena_create(4096);
        if (!arena) continue;

        ShJsonValue *root = NULL;
        ShJsonStatus status = sh_json_parse(line_buf, len, arena, &root);
        if (status != SH_JSON_OK) {
            send_error(stdout, NULL, -32700, "Parse error");
            sh_arena_free(arena);
            continue;
        }

        const ShJsonValue *id = sh_json_get(root, "id");
        const char *method = sh_json_as_string(sh_json_get(root, "method"), "");
        const ShJsonValue *params = sh_json_get(root, "params");

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(stdout, id);
        } else if (strcmp(method, "notifications/initialized") == 0) {
            /* No-op notification — no response */
        } else if (strcmp(method, "tools/list") == 0) {
            handle_tools_list(stdout, id);
        } else if (strcmp(method, "tools/call") == 0) {
            handle_tools_call(stdout, id, params, app_dir, &warm_ctx);
        } else {
            /* Only send error for methods with an id (requests, not notifications) */
            if (id && !sh_json_is_null(id))
                send_error(stdout, id, -32601, "Method not found");
        }

        sh_arena_free(arena);
    }

    hl_app_context_free(warm_ctx);
    free(line_buf);
    return 0;
}
