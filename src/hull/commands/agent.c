/*
 * commands/agent.c — hull agent: AI-native development tooling
 *
 * Provides machine-readable (JSON) introspection and interaction for
 * AI coding agents. Subcommands:
 *
 *   hull agent routes [app_dir]            — list registered routes
 *   hull agent db schema [app_dir]         — introspect DB schema
 *   hull agent db query "SQL" [app_dir]    — run read-only query
 *   hull agent request METHOD PATH [opts]  — HTTP request to dev server
 *   hull agent status                      — dev server status
 *   hull agent errors                      — structured errors from last reload
 *   hull agent test [app_dir]              — run tests (delegates to hull test)
 *
 * All output is JSON to stdout. Errors go to stderr.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/agent.h"
#include "hull/cap/db.h"
#include "hull/cap/test.h"
#include "hull/cap/tool.h"
#include "hull/migrate.h"
#include "hull/vfs.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "lua.h"
#include "lauxlib.h"
#endif

#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#include "quickjs.h"
#endif

#include <keel/allocator.h>
#include <keel/router.h>

#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

/* ── JSON output helpers ──────────────────────────────────────────── */

/*
 * Write a JSON-escaped string (without surrounding quotes) to fp.
 * Handles: \\ \" \n \r \t and control characters.
 */
static void json_escape(FILE *fp, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp);  break;
        case '\r': fputs("\\r", fp);  break;
        case '\t': fputs("\\t", fp);  break;
        default:
            if (c < 0x20)
                fprintf(fp, "\\u%04x", c);
            else
                fputc(c, fp);
            break;
        }
    }
}

static void json_string(FILE *fp, const char *s)
{
    fputc('"', fp);
    if (s)
        json_escape(fp, s, strlen(s));
    fputs("\"", fp);
}

/* ── Entry point detection (shared by in-process subcommands) ─────── */

static const char *detect_entry(const char *app_dir, const char *ext,
                                char *buf, size_t buf_size)
{
    /* Strip trailing slash from app_dir */
    size_t dir_len = strlen(app_dir);
    while (dir_len > 1 && app_dir[dir_len - 1] == '/')
        dir_len--;

    snprintf(buf, buf_size, "%.*s/app.%s", (int)dir_len, app_dir, ext);
    if (access(buf, F_OK) == 0) return buf;
    return NULL;
}

/* ── Subcommand: routes ───────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA
static int agent_routes_lua(const char *app_dir, const char *entry)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 1;

    HlLua lua;
    memset(&lua, 0, sizeof(lua));

    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "hull agent: cannot open :memory: database\n");
        return 1;
    }
    hl_cap_db_init(db);

    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    HlVfs app_vfs, platform_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    hl_migrate_run(db, &app_vfs);

    HlStmtCache stmt_cache;
    hl_stmt_cache_init(&stmt_cache, db);
    lua.base.db = db;
    lua.base.stmt_cache = &stmt_cache;
    lua.base.app_vfs = &app_vfs;
    lua.base.platform_vfs = &platform_vfs;

    if (hl_lua_init(&lua, &cfg) != 0) {
        fprintf(stderr, "hull agent: Lua init failed\n");
        sqlite3_close(db);
        return 1;
    }

    if (hl_lua_load_app(&lua, entry) != 0) {
        fprintf(stderr, "hull agent: failed to load %s\n", entry);
        hl_lua_free(&lua);
        sqlite3_close(db);
        return 1;
    }

    /* Extract routes from __hull_route_defs */
    printf("{\"runtime\":\"lua\",\"routes\":[");

    lua_getfield(lua.L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_isnil(lua.L, -1)) {
        int count = (int)luaL_len(lua.L, -1);
        for (int i = 1; i <= count; i++) {
            if (i > 1) putchar(',');
            lua_rawgeti(lua.L, -1, i);

            lua_getfield(lua.L, -1, "method");
            const char *method = lua_tostring(lua.L, -1);
            lua_pop(lua.L, 1);

            lua_getfield(lua.L, -1, "pattern");
            const char *pattern = lua_tostring(lua.L, -1);
            lua_pop(lua.L, 1);

            printf("{\"method\":");
            json_string(stdout, method);
            printf(",\"pattern\":");
            json_string(stdout, pattern);
            putchar('}');

            lua_pop(lua.L, 1); /* pop def table */
        }
    }
    lua_pop(lua.L, 1);

    /* Extract pre-body middleware from __hull_middleware */
    printf("],\"middleware\":[");

    lua_getfield(lua.L, LUA_REGISTRYINDEX, "__hull_middleware");
    if (!lua_isnil(lua.L, -1)) {
        int count = (int)luaL_len(lua.L, -1);
        for (int i = 1; i <= count; i++) {
            if (i > 1) putchar(',');
            lua_rawgeti(lua.L, -1, i);

            lua_getfield(lua.L, -1, "method");
            const char *method = lua_tostring(lua.L, -1);
            lua_pop(lua.L, 1);

            lua_getfield(lua.L, -1, "pattern");
            const char *pattern = lua_tostring(lua.L, -1);
            lua_pop(lua.L, 1);

            printf("{\"method\":");
            json_string(stdout, method);
            printf(",\"pattern\":");
            json_string(stdout, pattern);
            printf(",\"phase\":\"pre\"}");

            lua_pop(lua.L, 1);
        }
    }
    lua_pop(lua.L, 1);

    /* Extract post-body middleware from __hull_post_middleware */
    lua_getfield(lua.L, LUA_REGISTRYINDEX, "__hull_post_middleware");
    if (!lua_isnil(lua.L, -1)) {
        int count = (int)luaL_len(lua.L, -1);
        /* Check if we need a leading comma (pre-body middleware exists) */
        lua_getfield(lua.L, LUA_REGISTRYINDEX, "__hull_middleware");
        int pre_count = lua_isnil(lua.L, -1) ? 0 : (int)luaL_len(lua.L, -1);
        lua_pop(lua.L, 1);

        for (int i = 1; i <= count; i++) {
            if (i > 1 || pre_count > 0) putchar(',');
            lua_rawgeti(lua.L, -1, i);

            lua_getfield(lua.L, -1, "method");
            const char *method = lua_tostring(lua.L, -1);
            lua_pop(lua.L, 1);

            lua_getfield(lua.L, -1, "pattern");
            const char *pattern = lua_tostring(lua.L, -1);
            lua_pop(lua.L, 1);

            printf("{\"method\":");
            json_string(stdout, method);
            printf(",\"pattern\":");
            json_string(stdout, pattern);
            printf(",\"phase\":\"post\"}");

            lua_pop(lua.L, 1);
        }
    }
    lua_pop(lua.L, 1);

    printf("]}\n");

    hl_lua_free(&lua);
    hl_stmt_cache_destroy(&stmt_cache);
    hl_cap_db_shutdown(db);
    sqlite3_close(db);
    return 0;
}
#endif

#ifdef HL_ENABLE_JS
static int agent_routes_js(const char *app_dir, const char *entry)
{
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    HlJS js;
    memset(&js, 0, sizeof(js));

    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "hull agent: cannot open :memory: database\n");
        return 1;
    }
    hl_cap_db_init(db);

    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    HlVfs app_vfs, platform_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    hl_migrate_run(db, &app_vfs);

    HlStmtCache stmt_cache;
    hl_stmt_cache_init(&stmt_cache, db);
    js.base.db = db;
    js.base.stmt_cache = &stmt_cache;
    js.base.app_vfs = &app_vfs;
    js.base.platform_vfs = &platform_vfs;

    if (hl_js_init(&js, &cfg) != 0) {
        fprintf(stderr, "hull agent: QuickJS init failed\n");
        sqlite3_close(db);
        return 1;
    }

    if (hl_js_load_app(&js, entry) != 0) {
        fprintf(stderr, "hull agent: failed to load %s\n", entry);
        hl_js_free(&js);
        sqlite3_close(db);
        return 1;
    }

    JSValue global = JS_GetGlobalObject(js.ctx);

    /* Helper: extract array of {method, pattern} from a global property */
    #define EXTRACT_ARRAY(prop, phase_str, need_comma) do { \
        JSValue arr = JS_GetPropertyStr(js.ctx, global, prop); \
        if (!JS_IsUndefined(arr) && !JS_IsNull(arr)) { \
            JSValue len_val = JS_GetPropertyStr(js.ctx, arr, "length"); \
            int32_t len = 0; \
            JS_ToInt32(js.ctx, &len, len_val); \
            JS_FreeValue(js.ctx, len_val); \
            for (int32_t i = 0; i < len; i++) { \
                if (i > 0 || (need_comma)) putchar(','); \
                JSValue item = JS_GetPropertyUint32(js.ctx, arr, (uint32_t)i); \
                JSValue m = JS_GetPropertyStr(js.ctx, item, "method"); \
                JSValue p = JS_GetPropertyStr(js.ctx, item, "pattern"); \
                const char *ms = JS_ToCString(js.ctx, m); \
                const char *ps = JS_ToCString(js.ctx, p); \
                printf("{\"method\":"); \
                json_string(stdout, ms); \
                printf(",\"pattern\":"); \
                json_string(stdout, ps); \
                if (phase_str) printf(",\"phase\":\"%s\"", (const char *)(phase_str)); \
                putchar('}'); \
                JS_FreeCString(js.ctx, ms); \
                JS_FreeCString(js.ctx, ps); \
                JS_FreeValue(js.ctx, m); \
                JS_FreeValue(js.ctx, p); \
                JS_FreeValue(js.ctx, item); \
            } \
        } \
        JS_FreeValue(js.ctx, arr); \
    } while (0)

    printf("{\"runtime\":\"js\",\"routes\":[");
    // cppcheck-suppress nullPointer
    EXTRACT_ARRAY("__hull_route_defs", NULL, 0);
    printf("],\"middleware\":[");

    /* Pre-body middleware */
    JSValue pre_arr = JS_GetPropertyStr(js.ctx, global, "__hull_middleware");
    int pre_count = 0;
    if (!JS_IsUndefined(pre_arr) && !JS_IsNull(pre_arr)) {
        JSValue len_val = JS_GetPropertyStr(js.ctx, pre_arr, "length");
        JS_ToInt32(js.ctx, &pre_count, len_val);
        JS_FreeValue(js.ctx, len_val);
    }
    JS_FreeValue(js.ctx, pre_arr);

    EXTRACT_ARRAY("__hull_middleware", "pre", 0);
    EXTRACT_ARRAY("__hull_post_middleware", "post", pre_count > 0);
    printf("]}\n");

    #undef EXTRACT_ARRAY

    JS_FreeValue(js.ctx, global);
    hl_js_free(&js);
    hl_stmt_cache_destroy(&stmt_cache);
    hl_cap_db_shutdown(db);
    sqlite3_close(db);
    return 0;
}
#endif

static int cmd_routes(int argc, char **argv)
{
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-')
        app_dir = argv[0];

    char lua_buf[4096], js_buf[4096];
    const char *lua_entry = detect_entry(app_dir, "lua", lua_buf, sizeof(lua_buf));
    const char *js_entry  = detect_entry(app_dir, "js",  js_buf,  sizeof(js_buf));

    if (!lua_entry && !js_entry) {
        fprintf(stderr, "hull agent routes: no entry point found in %s\n", app_dir);
        return 1;
    }

    /* Prefer Lua if both exist (consistent with hull test) */
#ifdef HL_ENABLE_LUA
    if (lua_entry)
        return agent_routes_lua(app_dir, lua_entry);
#endif
#ifdef HL_ENABLE_JS
    if (js_entry)
        return agent_routes_js(app_dir, js_entry);
#endif

    fprintf(stderr, "hull agent routes: no runtime available\n");
    return 1;
}

/* ── Subcommand: db schema / db query ─────────────────────────────── */

/*
 * Helper: open the app's database file. Tries -d flag value,
 * then data.db in app_dir, then falls back to :memory: with migrations.
 */
static sqlite3 *open_app_db(const char *app_dir, const char *db_path)
{
    sqlite3 *db = NULL;
    char default_path[PATH_MAX];

    if (!db_path) {
        snprintf(default_path, sizeof(default_path), "%s/data.db", app_dir);
        if (access(default_path, F_OK) == 0)
            db_path = default_path;
    }

    if (db_path) {
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            fprintf(stderr, "hull agent db: cannot open %s: %s\n",
                    db_path, sqlite3_errmsg(db));
            if (db) sqlite3_close(db);
            return NULL;
        }
    } else {
        /* No DB file found — open :memory: and run migrations */
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            fprintf(stderr, "hull agent db: cannot open :memory: database\n");
            return NULL;
        }
        hl_cap_db_init(db);

        extern const HlEntry hl_app_entries[];
        HlVfs app_vfs;
        hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
        hl_migrate_run(db, &app_vfs);
    }

    return db;
}

static int cmd_db_schema(int argc, char **argv)
{
    const char *app_dir = ".";
    const char *db_path = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (argv[i][0] != '-') {
            app_dir = argv[i];
        }
    }

    sqlite3 *db = open_app_db(app_dir, db_path);
    if (!db) return 1;

    /* Query all tables (skip internal SQLite tables) */
    printf("{\"tables\":[");

    sqlite3_stmt *tables_stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name",
        -1, &tables_stmt, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "hull agent db schema: query failed: %s\n",
                sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    int table_idx = 0;
    while (sqlite3_step(tables_stmt) == SQLITE_ROW) {
        if (table_idx > 0) putchar(',');

        const char *table_name = (const char *)sqlite3_column_text(tables_stmt, 0);
        printf("{\"name\":");
        json_string(stdout, table_name);
        printf(",\"columns\":[");

        /* Query columns for this table via PRAGMA */
        char pragma[512];
        snprintf(pragma, sizeof(pragma), "PRAGMA table_info(\"%s\")", table_name);

        sqlite3_stmt *col_stmt = NULL;
        int rc2 = sqlite3_prepare_v2(db, pragma, -1, &col_stmt, NULL);
        if (rc2 == SQLITE_OK) {
            int col_idx = 0;
            while (sqlite3_step(col_stmt) == SQLITE_ROW) {
                if (col_idx > 0) putchar(',');

                const char *col_name = (const char *)sqlite3_column_text(col_stmt, 1);
                const char *col_type = (const char *)sqlite3_column_text(col_stmt, 2);
                int notnull = sqlite3_column_int(col_stmt, 3);
                int pk = sqlite3_column_int(col_stmt, 5);

                printf("{\"name\":");
                json_string(stdout, col_name);
                printf(",\"type\":");
                json_string(stdout, col_type ? col_type : "");
                if (notnull) printf(",\"notnull\":true");
                if (pk) printf(",\"pk\":true");

                /* Default value */
                if (sqlite3_column_type(col_stmt, 4) != SQLITE_NULL) {
                    const char *def = (const char *)sqlite3_column_text(col_stmt, 4);
                    printf(",\"default\":");
                    json_string(stdout, def);
                }

                putchar('}');
                col_idx++;
            }
            sqlite3_finalize(col_stmt);
        }

        printf("]}");
        table_idx++;
    }
    sqlite3_finalize(tables_stmt);

    printf("]}\n");
    sqlite3_close(db);
    return 0;
}

static int cmd_db_query(int argc, char **argv)
{
    const char *sql = NULL;
    const char *app_dir = ".";
    const char *db_path = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (argv[i][0] != '-' && !sql) {
            sql = argv[i];
        } else if (argv[i][0] != '-') {
            app_dir = argv[i];
        }
    }

    if (!sql) {
        fprintf(stderr, "hull agent db query: SQL argument required\n");
        return 1;
    }

    sqlite3 *db = open_app_db(app_dir, db_path);
    if (!db) return 1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("{\"error\":");
        json_string(stdout, sqlite3_errmsg(db));
        printf("}\n");
        sqlite3_close(db);
        return 1;
    }

    /* Get column names */
    int ncols = sqlite3_column_count(stmt);
    printf("{\"columns\":[");
    for (int i = 0; i < ncols; i++) {
        if (i > 0) putchar(',');
        json_string(stdout, sqlite3_column_name(stmt, i));
    }
    printf("],\"rows\":[");

    int row_idx = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (row_idx > 0) putchar(',');
        putchar('[');

        for (int i = 0; i < ncols; i++) {
            if (i > 0) putchar(',');

            switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:
                printf("null");
                break;
            case SQLITE_INTEGER:
                printf("%lld", (long long)sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                printf("%g", sqlite3_column_double(stmt, i));
                break;
            case SQLITE_TEXT: {
                const char *text = (const char *)sqlite3_column_text(stmt, i);
                json_string(stdout, text);
                break;
            }
            case SQLITE_BLOB:
                printf("\"<blob:%d>\"", sqlite3_column_bytes(stmt, i));
                break;
            }
        }

        putchar(']');
        row_idx++;
    }

    printf("],\"count\":%d", row_idx);

    if (rc != SQLITE_DONE) {
        printf(",\"error\":");
        json_string(stdout, sqlite3_errmsg(db));
    }

    printf("}\n");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
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

/* ── Subcommand: request ──────────────────────────────────────────── */

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
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            body = argv[++i];
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            if (header_count < 32)
                headers[header_count++] = argv[++i];
        }
    }

    /* Connect to localhost:port */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("{\"error\":\"cannot create socket\"}\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* Set connect timeout (5s) */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct timeval start, end;
    gettimeofday(&start, NULL);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        printf("{\"error\":\"cannot connect to 127.0.0.1:%d\"}\n", port);
        return 1;
    }

    /* Build HTTP request */
    size_t body_len = body ? strlen(body) : 0;
    char req_buf[8192];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Connection: close\r\n",
        method, path, port);

    if (body_len > 0) {
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (size_t)req_len,
            "Content-Length: %zu\r\n", body_len);
    }

    for (int i = 0; i < header_count; i++) {
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (size_t)req_len,
            "%s\r\n", headers[i]);
    }

    req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (size_t)req_len, "\r\n");

    /* Send request */
    ssize_t sent = send(sock, req_buf, (size_t)req_len, 0);
    if (sent < 0) {
        close(sock);
        printf("{\"error\":\"send failed\"}\n");
        return 1;
    }

    if (body && body_len > 0) {
        sent = send(sock, body, body_len, 0);
        if (sent < 0) {
            close(sock);
            printf("{\"error\":\"send body failed\"}\n");
            return 1;
        }
    }

    /* Read response */
    char *resp_buf = malloc(1024 * 1024); /* 1 MB max */
    if (!resp_buf) {
        close(sock);
        printf("{\"error\":\"allocation failed\"}\n");
        return 1;
    }

    size_t resp_len = 0;
    for (;;) {
        ssize_t n = recv(sock, resp_buf + resp_len,
                         1024 * 1024 - resp_len - 1, 0);
        if (n <= 0) break;
        resp_len += (size_t)n;
        if (resp_len >= 1024 * 1024 - 1) break;
    }
    resp_buf[resp_len] = '\0';
    close(sock);

    gettimeofday(&end, NULL);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_usec - start.tv_usec) / 1000;

    /* Parse status line: "HTTP/1.1 200 OK\r\n" */
    int status = 0;
    const char *header_end = strstr(resp_buf, "\r\n\r\n");
    if (!header_end) {
        printf("{\"error\":\"malformed response\",\"raw\":");
        json_string(stdout, resp_buf);
        printf("}\n");
        free(resp_buf);
        return 1;
    }

    if (resp_len > 12) {
        char *sp = memchr(resp_buf, ' ', 12);
        if (sp) status = atoi(sp + 1);
    }

    const char *resp_body = header_end + 4;
    size_t resp_body_len = resp_len - (size_t)(resp_body - resp_buf);

    /* Output JSON */
    printf("{\"status\":%d", status);
    printf(",\"elapsed_ms\":%ld", elapsed_ms);

    /* Parse response headers */
    printf(",\"headers\":{");
    const char *line = resp_buf;
    /* Skip status line */
    const char *first_crlf = strstr(line, "\r\n");
    if (first_crlf) line = first_crlf + 2;

    int hdr_idx = 0;
    while (line < header_end) {
        const char *next = strstr(line, "\r\n");
        if (!next || next == line) break;

        const char *colon = memchr(line, ':', (size_t)(next - line));
        if (colon) {
            if (hdr_idx > 0) putchar(',');

            /* Header name */
            fputc('"', stdout);
            json_escape(stdout, line, (size_t)(colon - line));
            fputc('"', stdout);

            /* Header value (skip ": ") */
            const char *val = colon + 1;
            while (val < next && *val == ' ') val++;
            putchar(':');
            fputc('"', stdout);
            json_escape(stdout, val, (size_t)(next - val));
            fputc('"', stdout);

            hdr_idx++;
        }
        line = next + 2;
    }
    printf("}");

    printf(",\"body\":");
    fputc('"', stdout);
    json_escape(stdout, resp_body, resp_body_len);
    fputc('"', stdout);
    printf("}\n");

    free(resp_buf);
    return 0;
}

/* ── Subcommand: status ───────────────────────────────────────────── */

static int cmd_status(int argc, char **argv)
{
    int port = 3000;
    const char *app_dir = ".";

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    /* Try to read .hull/dev.json for port info */
    char dev_json_path[PATH_MAX];
    snprintf(dev_json_path, sizeof(dev_json_path), "%s/.hull/dev.json", app_dir);

    FILE *f = fopen(dev_json_path, "r");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        /* Extract port from JSON (simple parse) */
        const char *port_key = strstr(buf, "\"port\":");
        if (port_key) {
            int p = atoi(port_key + 7);
            if (p > 0) port = p;
        }
    }

    /* Probe the dev server with a socket connect */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("{\"running\":false,\"port\":%d,\"error\":\"cannot create socket\"}\n",
               port);
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int connected = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(sock);

    printf("{\"running\":%s,\"port\":%d}\n",
           connected ? "true" : "false", port);
    return 0;
}

/* ── Subcommand: errors ───────────────────────────────────────────── */

static int cmd_errors(int argc, char **argv)
{
    const char *app_dir = ".";
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-')
            app_dir = argv[i];
    }

    char err_path[PATH_MAX];
    snprintf(err_path, sizeof(err_path), "%s/.hull/last_error.json", app_dir);

    FILE *f = fopen(err_path, "r");
    if (!f) {
        printf("{\"errors\":[]}\n");
        return 0;
    }

    /* Read and pass through the JSON file */
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Pass through — the file is already JSON */
    printf("%s", buf);
    if (n > 0 && buf[n - 1] != '\n')
        putchar('\n');

    return 0;
}

/* ── Subcommand: test ─────────────────────────────────────────────── */

#define MAX_TEST_RESULTS 256

/* JSON-escape a string to stdout (reuses json_escape/json_string from above) */

#ifdef HL_ENABLE_LUA
static int agent_test_lua(const char *app_dir, const char *entry)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 1;

    HlLua lua;
    memset(&lua, 0, sizeof(lua));

    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        printf("{\"error\":\"cannot open :memory: database\"}\n");
        return 1;
    }
    hl_cap_db_init(db);

    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    HlVfs app_vfs, platform_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    hl_migrate_run(db, &app_vfs);

    HlStmtCache stmt_cache;
    hl_stmt_cache_init(&stmt_cache, db);
    lua.base.db = db;
    lua.base.stmt_cache = &stmt_cache;
    lua.base.app_vfs = &app_vfs;
    lua.base.platform_vfs = &platform_vfs;

    if (hl_lua_init(&lua, &cfg) != 0) {
        printf("{\"error\":\"Lua init failed\"}\n");
        sqlite3_close(db);
        return 1;
    }

    if (hl_lua_load_app(&lua, entry) != 0) {
        printf("{\"error\":\"failed to load app\"}\n");
        hl_lua_free(&lua);
        sqlite3_close(db);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_lua_wire_routes(&lua, &router) != 0) {
        printf("{\"error\":\"no routes registered\"}\n");
        kl_router_free(&router);
        hl_lua_free(&lua);
        sqlite3_close(db);
        return 1;
    }

    hl_cap_test_register_lua(lua.L, &router, &lua);

    char **test_files = hl_tool_find_files(app_dir, "test_*.lua", NULL);
    if (!test_files || !test_files[0]) {
        printf("{\"error\":\"no test files found\"}\n");
        if (test_files) free(test_files);
        kl_router_free(&router);
        hl_lua_free(&lua);
        sqlite3_close(db);
        return 1;
    }

    int grand_total = 0, grand_passed = 0, grand_failed = 0;
    HlTestCaseResult results[MAX_TEST_RESULTS];

    printf("{\"runtime\":\"lua\",\"files\":[");

    int file_idx = 0;
    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        if (file_idx > 0) putchar(',');

        hl_cap_test_clear_lua(lua.L);

        printf("{\"name\":");
        json_string(stdout, basename);

        if (luaL_dofile(lua.L, file) != LUA_OK) {
            const char *err = lua_tostring(lua.L, -1);
            printf(",\"error\":");
            json_string(stdout, err ? err : "unknown");
            printf(",\"tests\":[]}");
            lua_pop(lua.L, 1);
            grand_failed++;
            grand_total++;
            free(*fp);
            file_idx++;
            continue;
        }

        int file_total = 0, file_passed = 0, file_failed = 0;
        memset(results, 0, sizeof(results));
        hl_cap_test_run_lua(lua.L, &file_total, &file_passed, &file_failed,
                            NULL, results, MAX_TEST_RESULTS);

        printf(",\"tests\":[");
        for (int i = 0; i < file_total && i < MAX_TEST_RESULTS; i++) {
            if (i > 0) putchar(',');
            printf("{\"name\":");
            json_string(stdout, results[i].name);
            printf(",\"status\":\"%s\"", results[i].passed ? "pass" : "fail");
            if (!results[i].passed && results[i].error[0]) {
                printf(",\"error\":");
                json_string(stdout, results[i].error);
            }
            putchar('}');
        }
        printf("]}");

        grand_total += file_total;
        grand_passed += file_passed;
        grand_failed += file_failed;

        free(*fp);
        file_idx++;
    }
    free(test_files);

    printf("],\"total\":%d,\"passed\":%d,\"failed\":%d}\n",
           grand_total, grand_passed, grand_failed);

    kl_router_free(&router);
    hl_lua_free(&lua);
    hl_stmt_cache_destroy(&stmt_cache);
    hl_cap_db_shutdown(db);
    sqlite3_close(db);
    return grand_failed > 0 ? 1 : 0;
}
#endif

#ifdef HL_ENABLE_JS
static int agent_test_js(const char *app_dir, const char *entry)
{
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    HlJS js;
    memset(&js, 0, sizeof(js));

    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        printf("{\"error\":\"cannot open :memory: database\"}\n");
        return 1;
    }
    hl_cap_db_init(db);

    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    HlVfs app_vfs, platform_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    hl_migrate_run(db, &app_vfs);

    HlStmtCache stmt_cache;
    hl_stmt_cache_init(&stmt_cache, db);
    js.base.db = db;
    js.base.stmt_cache = &stmt_cache;
    js.base.app_vfs = &app_vfs;
    js.base.platform_vfs = &platform_vfs;

    if (hl_js_init(&js, &cfg) != 0) {
        printf("{\"error\":\"QuickJS init failed\"}\n");
        sqlite3_close(db);
        return 1;
    }

    if (hl_js_load_app(&js, entry) != 0) {
        printf("{\"error\":\"failed to load app\"}\n");
        hl_js_free(&js);
        sqlite3_close(db);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_js_wire_routes(&js, &router) != 0) {
        printf("{\"error\":\"no routes registered\"}\n");
        kl_router_free(&router);
        hl_js_free(&js);
        sqlite3_close(db);
        return 1;
    }

    hl_cap_test_register_js(js.ctx, &router, &js);

    char **test_files = hl_tool_find_files(app_dir, "test_*.js", NULL);
    if (!test_files || !test_files[0]) {
        printf("{\"error\":\"no test files found\"}\n");
        if (test_files) free(test_files);
        kl_router_free(&router);
        hl_js_free(&js);
        sqlite3_close(db);
        return 1;
    }

    int grand_total = 0, grand_passed = 0, grand_failed = 0;
    HlTestCaseResult results[MAX_TEST_RESULTS];

    printf("{\"runtime\":\"js\",\"files\":[");

    int file_idx = 0;
    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        if (file_idx > 0) putchar(',');

        hl_cap_test_clear_js(js.ctx);

        printf("{\"name\":");
        json_string(stdout, basename);

        /* Read and evaluate the test file */
        FILE *f = fopen(file, "r");
        if (!f) {
            printf(",\"error\":\"cannot open file\",\"tests\":[]}");
            grand_failed++;
            grand_total++;
            free(*fp);
            file_idx++;
            continue;
        }
        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        if (flen < 0) { fclose(f); free(*fp); file_idx++; continue; }
        fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)flen + 1);
        if (!src) { fclose(f); free(*fp); file_idx++; continue; }
        if (fread(src, 1, (size_t)flen, f) != (size_t)flen) {
            free(src); fclose(f); free(*fp); file_idx++; continue;
        }
        src[flen] = '\0';
        fclose(f);

        JSValue result = JS_Eval(js.ctx, src, (size_t)flen, file,
                                 JS_EVAL_TYPE_MODULE);
        free(src);

        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(js.ctx);
            JSValue msg_val = JS_GetPropertyStr(js.ctx, exc, "message");
            const char *msg = JS_ToCString(js.ctx, msg_val);
            printf(",\"error\":");
            json_string(stdout, msg ? msg : "unknown");
            printf(",\"tests\":[]}");
            if (msg) JS_FreeCString(js.ctx, msg);
            JS_FreeValue(js.ctx, msg_val);
            JS_FreeValue(js.ctx, exc);
            JS_FreeValue(js.ctx, result);
            grand_failed++;
            grand_total++;
            free(*fp);
            file_idx++;
            continue;
        }
        JS_FreeValue(js.ctx, result);

        int file_total = 0, file_passed = 0, file_failed = 0;
        memset(results, 0, sizeof(results));
        hl_cap_test_run_js(js.ctx, &file_total, &file_passed, &file_failed,
                           NULL, results, MAX_TEST_RESULTS);

        printf(",\"tests\":[");
        for (int i = 0; i < file_total && i < MAX_TEST_RESULTS; i++) {
            if (i > 0) putchar(',');
            printf("{\"name\":");
            json_string(stdout, results[i].name);
            printf(",\"status\":\"%s\"", results[i].passed ? "pass" : "fail");
            if (!results[i].passed && results[i].error[0]) {
                printf(",\"error\":");
                json_string(stdout, results[i].error);
            }
            putchar('}');
        }
        printf("]}");

        grand_total += file_total;
        grand_passed += file_passed;
        grand_failed += file_failed;

        free(*fp);
        file_idx++;
    }
    free(test_files);

    printf("],\"total\":%d,\"passed\":%d,\"failed\":%d}\n",
           grand_total, grand_passed, grand_failed);

    kl_router_free(&router);
    hl_js_free(&js);
    hl_stmt_cache_destroy(&stmt_cache);
    hl_cap_db_shutdown(db);
    sqlite3_close(db);
    return grand_failed > 0 ? 1 : 0;
}
#endif

static int cmd_test(int argc, char **argv, const char *hull_exe)
{
    (void)hull_exe;
    const char *app_dir = ".";
    if (argc >= 1 && argv[0][0] != '-')
        app_dir = argv[0];

    char lua_buf[4096], js_buf[4096];
    const char *lua_entry = detect_entry(app_dir, "lua", lua_buf, sizeof(lua_buf));
    const char *js_entry  = detect_entry(app_dir, "js",  js_buf,  sizeof(js_buf));

    if (!lua_entry && !js_entry) {
        printf("{\"error\":\"no entry point found in %s\"}\n", app_dir);
        return 1;
    }

#ifdef HL_ENABLE_LUA
    if (lua_entry)
        return agent_test_lua(app_dir, lua_entry);
#endif
#ifdef HL_ENABLE_JS
    if (js_entry)
        return agent_test_js(app_dir, js_entry);
#endif

    printf("{\"error\":\"no runtime available\"}\n");
    return 1;
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
        "\n"
        "All output is JSON to stdout.\n");
}

/* ── Command entry point ──────────────────────────────────────────── */

int hl_cmd_agent(int argc, char **argv, const char *hull_exe)
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
        return cmd_test(sub_argc, sub_argv, hull_exe);
    if (strcmp(sub, "help") == 0 || strcmp(sub, "--help") == 0 ||
        strcmp(sub, "-h") == 0) {
        agent_usage();
        return 0;
    }

    fprintf(stderr, "hull agent: unknown subcommand '%s'\n", sub);
    agent_usage();
    return 1;
}
