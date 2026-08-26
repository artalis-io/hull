/*
 * commands/mcp.c - hull mcp serve: MCP stdio JSON-RPC 2.0 server
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

/* ── Tool schema definitions (static JSON) ─────────────────────────────
 *
 * The strings below are MCP tool schemas: JSON-shaped *constants*,
 * not runtime-emitted JSON. They are compile-time `const char[]`
 * literals served verbatim to clients (Claude Code, Cursor, etc.) on
 * the `tools/list` MCP method.
 *
 * Why these are NOT migrated to `sh_json` like every other JSON site
 * in the codebase (cache.c, sbom.c, doctor.c, tools.c, version.c,
 * serve.c, audit.c, agent_*.c all use ShJsonWriter as of v0.2.0):
 *
 *   1. **Static rodata vs runtime allocation.** These literals live in
 *      the binary's `.rodata` section at zero runtime cost. Building
 *      them via `sh_json_buf` would add startup work, heap allocation,
 *      and a cleanup path for what is currently free.
 *   2. **No injection surface.** A JSON-shaped string literal cannot
 *      have an escape vulnerability: the bytes are fixed at compile
 *      time. The original audit (2026-06) flagged them as "Low /
 *      defensible" for exactly this reason.
 *   3. **Schema authoring readability.** A reader of the schema can
 *      compare it 1:1 against the MCP spec text without mentally
 *      compiling `sh_json_write_kv_string` calls. Schemas evolve by
 *      copy-paste from the spec; literal JSON minimises drift.
 *
 * If a runtime-generated schema is ever needed (e.g. tool list that
 * varies by `HL_ENABLE_*` build flags beyond the few `#ifdef`s
 * already here), at that point convert the affected schema to
 * `sh_json_buf`-built rather than escalating to a full rewrite.
 */

static const char SCHEMA_ROUTES[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"}}}";

#ifdef HL_ENABLE_SQLITE
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
#endif

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
    "testing, build, deploy, search, i18n, webhooks, validation, compute)\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"minimal\",\"compact\",\"full\"],"
    "\"description\":\"Detail level (default: compact)\"}},"
    "\"required\":[\"task\"]}";

#ifdef HL_ENABLE_SQLITE
static const char SCHEMA_MIGRATE[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"},"
    "\"db_path\":{\"type\":\"string\",\"description\":\"Database file path\"}}}";
#endif

static const char SCHEMA_RELOAD[] =
    "{\"type\":\"object\",\"properties\":{}}";

/* ── Extended introspection schemas ─────────── */

/* Shared schema for any subcommand whose only param is app_dir. */
static const char SCHEMA_APP_DIR_ONLY[] =
    "{\"type\":\"object\",\"properties\":{\"app_dir\":{\"type\":\"string\","
    "\"description\":\"Application directory (default: .)\"}}}";

static const char SCHEMA_ENDPOINT[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"method\":{\"type\":\"string\",\"description\":\"HTTP method (e.g. GET)\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Request path (e.g. /api/users/42)\"},"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"}},"
    "\"required\":[\"method\",\"path\"]}";

static const char SCHEMA_VALIDATE[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"file\":{\"type\":\"string\",\"description\":\"Path to a .lua or .js file\"}},"
    "\"required\":[\"file\"]}";

static const char SCHEMA_EVAL[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\",\"description\":\"Lua/JS expression or statements to evaluate\"},"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"}},"
    "\"required\":[\"code\"]}";

static const char SCHEMA_LOGS[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"},"
    "\"tail\":{\"type\":\"integer\",\"description\":\"Number of trailing lines (default 100, max 10000)\"}}}";

static const char SCHEMA_TEMPLATE[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Template name (e.g. pages/home.html)\"},"
    "\"data\":{\"type\":\"string\",\"description\":\"JSON string of data to render with (default: {})\"},"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"}},"
    "\"required\":[\"name\"]}";

static const char SCHEMA_COMPUTE_CALL[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"module\":{\"type\":\"string\",\"description\":\"WASM module name (e.g. score)\"},"
    "\"input_file\":{\"type\":\"string\",\"description\":\"Path to a file with input bytes\"},"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"}},"
    "\"required\":[\"module\",\"input_file\"]}";

#ifdef HL_ENABLE_SQLITE
static const char SCHEMA_SCHEMA_DIFF[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"},"
    "\"db_path\":{\"type\":\"string\",\"description\":\"Database file path (default: app/data.db)\"}}}";

static const char SCHEMA_SQL_NAMED[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Query name from app/queries.json\"},"
    "\"params\":{\"type\":\"string\",\"description\":\"JSON string of parameter bindings (e.g. {\\\"id\\\":42})\"},"
    "\"app_dir\":{\"type\":\"string\",\"description\":\"Application directory (default: .)\"}},"
    "\"required\":[\"name\"]}";
#endif

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;
} McpTool;

static const McpTool mcp_tools[] = {
    { "hull_routes",         "List registered routes and middleware",
                              SCHEMA_ROUTES },
#ifdef HL_ENABLE_SQLITE
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
#ifdef HL_ENABLE_SQLITE
    { "hull_migrate_status", "Show migration status",
                              SCHEMA_MIGRATE },
#endif
    { "hull_reload",         "Reload application context (after code changes)",
                              SCHEMA_RELOAD },
    /* Extended introspection */
    { "hull_manifest",       "Effective manifest JSON (post-extraction)",
                              SCHEMA_APP_DIR_ONLY },
    { "hull_endpoint",       "Preview which handler+middleware would fire for METHOD PATH",
                              SCHEMA_ENDPOINT },
    { "hull_middleware",     "List middleware that match METHOD PATH",
                              SCHEMA_ENDPOINT },
    { "hull_capabilities",   "Capability declared-vs-used analysis (source walk vs manifest)",
                              SCHEMA_APP_DIR_ONLY },
    { "hull_validate",       "Parse + sandbox-check a Lua/JS file in isolation",
                              SCHEMA_VALIDATE },
    { "hull_vfs",            "List embedded files (path, size, bucket)",
                              SCHEMA_APP_DIR_ONLY },
    { "hull_compute",        "List WASM compute modules + AOT presence. For authoring use the shell commands `hull compute new|build|test|check|refresh-header` and the `compute` context (hull_context task=compute).",
                              SCHEMA_APP_DIR_ONLY },
    { "hull_gpu",            "List compiled WGSL shaders + GPU availability",
                              SCHEMA_APP_DIR_ONLY },
    { "hull_perf",           "Runtime stats snapshot (features, default limits)",
                              SCHEMA_APP_DIR_ONLY },
    { "hull_logs",           "Tail of app_dir/.hull/dev.log as JSON lines",
                              SCHEMA_LOGS },
    { "hull_eval",           "Run a one-shot Lua/JS snippet against the loaded app",
                              SCHEMA_EVAL },
    { "hull_template",       "Render a template with sample data via the loaded runtime",
                              SCHEMA_TEMPLATE },
    { "hull_compute_call",   "Invoke a WASM module against file input",
                              SCHEMA_COMPUTE_CALL },
#ifdef HL_ENABLE_SQLITE
    { "hull_schema_diff",    "DB schema drift analysis (sqlite_master vs migrations)",
                              SCHEMA_SCHEMA_DIFF },
    { "hull_sql_named",      "Run a pre-defined query from app/queries.json",
                              SCHEMA_SQL_NAMED },
#endif
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

#ifdef HL_ENABLE_SQLITE
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

#ifdef HL_ENABLE_SQLITE
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
            HlAppContextOpts opts = { .app_dir = app_dir, .gate_modules = 1 };
            if (hl_app_context_init(warm_ctx_ptr, &opts) != 0)
                *warm_ctx_ptr = NULL;
        }
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, &agent_out);
        sh_json_write_object_start(&w);
        sh_json_write_kv_bool(&w, "ok", warm_ctx_ptr && *warm_ctx_ptr);
        sh_json_write_object_end(&w);

    /* ── Extended introspection ─────────────────────────── */

    } else if (strcmp(tool_name, "hull_manifest") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_manifest_ctx(warm_ctx, &agent_out);
        else
            hl_agent_manifest(dir, &agent_out);

    } else if (strcmp(tool_name, "hull_endpoint") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        const char *method = sh_json_as_string(sh_json_get(args, "method"), "GET");
        const char *path = sh_json_as_string(sh_json_get(args, "path"), "/");
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_endpoint_ctx(warm_ctx, method, path, &agent_out);
        else
            hl_agent_endpoint(dir, method, path, &agent_out);

    } else if (strcmp(tool_name, "hull_middleware") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        const char *method = sh_json_as_string(sh_json_get(args, "method"), "GET");
        const char *path = sh_json_as_string(sh_json_get(args, "path"), "/");
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_middleware_ctx(warm_ctx, method, path, &agent_out);
        else
            hl_agent_middleware(dir, method, path, &agent_out);

    } else if (strcmp(tool_name, "hull_capabilities") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_capabilities_ctx(warm_ctx, &agent_out);
        else
            hl_agent_capabilities(dir, &agent_out);

    } else if (strcmp(tool_name, "hull_validate") == 0) {
        const char *file = sh_json_as_string(sh_json_get(args, "file"), NULL);
        hl_agent_validate(file, &agent_out);

    } else if (strcmp(tool_name, "hull_vfs") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_vfs_ctx(warm_ctx, &agent_out);
        else
            hl_agent_vfs(dir, &agent_out);

    } else if (strcmp(tool_name, "hull_compute") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_compute_ctx(warm_ctx, &agent_out);
        else
            hl_agent_compute(dir, &agent_out);

    } else if (strcmp(tool_name, "hull_gpu") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_gpu_ctx(warm_ctx, &agent_out);
        else
            hl_agent_gpu(dir, &agent_out);

    } else if (strcmp(tool_name, "hull_perf") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0) {
            hl_agent_perf_ctx(warm_ctx, &agent_out);
        } else {
            /* No warm context - load briefly and free. */
            HlAppContextOpts opts = { .app_dir = dir, .gate_modules = 1 };
            HlAppContext *tmp = NULL;
            if (hl_app_context_init(&tmp, &opts) == 0) {
                hl_agent_perf_ctx(tmp, &agent_out);
                hl_app_context_free(tmp);
            } else {
                ShJsonWriter w;
                sh_json_writer_init(&w, sh_json_buf_write, &agent_out);
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "error", "failed to load app");
                sh_json_write_object_end(&w);
            }
        }

    } else if (strcmp(tool_name, "hull_logs") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        int tail_n = sh_json_as_int(sh_json_get(args, "tail"), 0);
        hl_agent_logs(dir, tail_n, &agent_out);

    } else if (strcmp(tool_name, "hull_eval") == 0) {
        const char *code = sh_json_as_string(sh_json_get(args, "code"), NULL);
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0) {
            hl_agent_eval_ctx(warm_ctx, code, &agent_out);
        } else {
            HlAppContextOpts opts = { .app_dir = dir, .gate_modules = 1 };
            HlAppContext *tmp = NULL;
            if (hl_app_context_init(&tmp, &opts) == 0) {
                hl_agent_eval_ctx(tmp, code, &agent_out);
                hl_app_context_free(tmp);
            } else {
                ShJsonWriter w;
                sh_json_writer_init(&w, sh_json_buf_write, &agent_out);
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "error", "failed to load app");
                sh_json_write_object_end(&w);
            }
        }

    } else if (strcmp(tool_name, "hull_template") == 0) {
        const char *name = sh_json_as_string(sh_json_get(args, "name"), NULL);
        const char *data = sh_json_as_string(sh_json_get(args, "data"), "{}");
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0) {
            hl_agent_template_ctx(warm_ctx, name, data, &agent_out);
        } else {
            HlAppContextOpts opts = { .app_dir = dir, .gate_modules = 1 };
            HlAppContext *tmp = NULL;
            if (hl_app_context_init(&tmp, &opts) == 0) {
                hl_agent_template_ctx(tmp, name, data, &agent_out);
                hl_app_context_free(tmp);
            } else {
                ShJsonWriter w;
                sh_json_writer_init(&w, sh_json_buf_write, &agent_out);
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "error", "failed to load app");
                sh_json_write_object_end(&w);
            }
        }

    } else if (strcmp(tool_name, "hull_compute_call") == 0) {
        const char *module = sh_json_as_string(sh_json_get(args, "module"), NULL);
        const char *input_file = sh_json_as_string(sh_json_get(args, "input_file"), NULL);
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0) {
            hl_agent_compute_call_ctx(warm_ctx, module, input_file, &agent_out);
        } else {
            HlAppContextOpts opts = { .app_dir = dir, .gate_modules = 1 };
            HlAppContext *tmp = NULL;
            if (hl_app_context_init(&tmp, &opts) == 0) {
                hl_agent_compute_call_ctx(tmp, module, input_file, &agent_out);
                hl_app_context_free(tmp);
            } else {
                ShJsonWriter w;
                sh_json_writer_init(&w, sh_json_buf_write, &agent_out);
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "error", "failed to load app");
                sh_json_write_object_end(&w);
            }
        }

#ifdef HL_ENABLE_SQLITE
    } else if (strcmp(tool_name, "hull_schema_diff") == 0) {
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        const char *db = sh_json_as_string(sh_json_get(args, "db_path"), NULL);
        if (warm_ctx && strcmp(dir, app_dir) == 0)
            hl_agent_schema_diff_ctx(warm_ctx, db, &agent_out);
        else
            hl_agent_schema_diff(dir, db, &agent_out);

    } else if (strcmp(tool_name, "hull_sql_named") == 0) {
        /* `params_json` (not `params`) - the JSON-RPC outer scope already
         * has a `params` argument to this handler, so cppcheck flags the
         * shadow under -shadowArgument. */
        const char *name = sh_json_as_string(sh_json_get(args, "name"), NULL);
        const char *params_json = sh_json_as_string(sh_json_get(args, "params"), NULL);
        const char *dir = sh_json_as_string(sh_json_get(args, "app_dir"), app_dir);
        if (warm_ctx && strcmp(dir, app_dir) == 0) {
            hl_agent_sql_named_ctx(warm_ctx, name, params_json, &agent_out);
        } else {
            HlAppContextOpts opts = { .app_dir = dir, .gate_modules = 1 };
            HlAppContext *tmp = NULL;
            if (hl_app_context_init(&tmp, &opts) == 0) {
                hl_agent_sql_named_ctx(tmp, name, params_json, &agent_out);
                hl_app_context_free(tmp);
            } else {
                ShJsonWriter w;
                sh_json_writer_init(&w, sh_json_buf_write, &agent_out);
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "error", "failed to load app");
                sh_json_write_object_end(&w);
            }
        }
#endif

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
    HlAppContextOpts ctx_opts = { .app_dir = app_dir, .gate_modules = 1 };
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
            /* No-op notification - no response */
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
