/*
 * agent_lib.c — Transport-agnostic agent introspection library
 *
 * Extracted from commands/agent.c. Each function writes complete JSON
 * to an ShJsonBuf via ShJsonWriter. Shared by CLI, MCP, and HTTP endpoints.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/agent_lib.h"
#include "hull/app_context.h"
#include "hull/cap/db.h"
#include "hull/cap/test.h"
#include "hull/cap/tool.h"
#include "hull/entry.h"
#include "hull/migrate.h"
#include "hull/runtime.h"
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

#include "hull/manifest.h"

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

/* ── Database opener (shared by schema/query) ──────────────────────── */

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
            if (db) sqlite3_close(db);
            return NULL;
        }
    } else {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK)
            return NULL;
        hl_cap_db_init(db);

        extern const HlEntry hl_app_entries[];
        HlVfs app_vfs;
        hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
        hl_migrate_run(db, &app_vfs);
    }

    return db;
}

/* ── JSON error helper ─────────────────────────────────────────────── */

static int write_error(ShJsonBuf *out, const char *msg)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "error", msg);
    sh_json_write_object_end(&w);
    return -1;
}

/* ── hl_agent_routes ───────────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA
static int agent_routes_lua(HlAppContext *ctx, ShJsonWriter *w)
{
    HlLua *lua = hl_app_context_lua(ctx);

    sh_json_write_kv_string(w, "runtime", "lua");

    /* Routes */
    sh_json_write_key(w, "routes");
    sh_json_write_array_start(w);

    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_isnil(lua->L, -1)) {
        int count = (int)luaL_len(lua->L, -1);
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(lua->L, -1, i);

            lua_getfield(lua->L, -1, "method");
            const char *method = lua_tostring(lua->L, -1);
            lua_pop(lua->L, 1);

            lua_getfield(lua->L, -1, "pattern");
            const char *pattern = lua_tostring(lua->L, -1);
            lua_pop(lua->L, 1);

            sh_json_write_object_start(w);
            sh_json_write_kv_string(w, "method", method);
            sh_json_write_kv_string(w, "pattern", pattern);
            sh_json_write_object_end(w);

            lua_pop(lua->L, 1);
        }
    }
    lua_pop(lua->L, 1);
    sh_json_write_array_end(w);

    /* Middleware */
    sh_json_write_key(w, "middleware");
    sh_json_write_array_start(w);

    /* Pre-body */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_middleware");
    if (!lua_isnil(lua->L, -1)) {
        int count = (int)luaL_len(lua->L, -1);
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(lua->L, -1, i);

            lua_getfield(lua->L, -1, "method");
            const char *method = lua_tostring(lua->L, -1);
            lua_pop(lua->L, 1);

            lua_getfield(lua->L, -1, "pattern");
            const char *pattern = lua_tostring(lua->L, -1);
            lua_pop(lua->L, 1);

            sh_json_write_object_start(w);
            sh_json_write_kv_string(w, "method", method);
            sh_json_write_kv_string(w, "pattern", pattern);
            sh_json_write_kv_string(w, "phase", "pre");
            sh_json_write_object_end(w);

            lua_pop(lua->L, 1);
        }
    }
    lua_pop(lua->L, 1);

    /* Post-body */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_post_middleware");
    if (!lua_isnil(lua->L, -1)) {
        int count = (int)luaL_len(lua->L, -1);
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(lua->L, -1, i);

            lua_getfield(lua->L, -1, "method");
            const char *method = lua_tostring(lua->L, -1);
            lua_pop(lua->L, 1);

            lua_getfield(lua->L, -1, "pattern");
            const char *pattern = lua_tostring(lua->L, -1);
            lua_pop(lua->L, 1);

            sh_json_write_object_start(w);
            sh_json_write_kv_string(w, "method", method);
            sh_json_write_kv_string(w, "pattern", pattern);
            sh_json_write_kv_string(w, "phase", "post");
            sh_json_write_object_end(w);

            lua_pop(lua->L, 1);
        }
    }
    lua_pop(lua->L, 1);

    sh_json_write_array_end(w);
    return 0;
}
#endif

#ifdef HL_ENABLE_JS
static int agent_routes_js(HlAppContext *ctx, ShJsonWriter *w)
{
    HlJS *js = hl_app_context_js(ctx);
    JSValue global = JS_GetGlobalObject(js->ctx);

    sh_json_write_kv_string(w, "runtime", "js");

    /* Helper macro to extract route/middleware arrays */
    #define EXTRACT_ARRAY(prop, phase_str) do { \
        JSValue arr = JS_GetPropertyStr(js->ctx, global, prop); \
        if (!JS_IsUndefined(arr) && !JS_IsNull(arr)) { \
            JSValue len_val = JS_GetPropertyStr(js->ctx, arr, "length"); \
            int32_t len = 0; \
            JS_ToInt32(js->ctx, &len, len_val); \
            JS_FreeValue(js->ctx, len_val); \
            for (int32_t i = 0; i < len; i++) { \
                JSValue item = JS_GetPropertyUint32(js->ctx, arr, (uint32_t)i); \
                JSValue m = JS_GetPropertyStr(js->ctx, item, "method"); \
                JSValue p = JS_GetPropertyStr(js->ctx, item, "pattern"); \
                const char *ms = JS_ToCString(js->ctx, m); \
                const char *ps = JS_ToCString(js->ctx, p); \
                sh_json_write_object_start(w); \
                sh_json_write_kv_string(w, "method", ms); \
                sh_json_write_kv_string(w, "pattern", ps); \
                if (phase_str) sh_json_write_kv_string(w, "phase", phase_str); \
                sh_json_write_object_end(w); \
                JS_FreeCString(js->ctx, ms); \
                JS_FreeCString(js->ctx, ps); \
                JS_FreeValue(js->ctx, m); \
                JS_FreeValue(js->ctx, p); \
                JS_FreeValue(js->ctx, item); \
            } \
        } \
        JS_FreeValue(js->ctx, arr); \
    } while (0)

    sh_json_write_key(w, "routes");
    sh_json_write_array_start(w);
    // cppcheck-suppress nullPointer
    EXTRACT_ARRAY("__hull_route_defs", NULL);
    sh_json_write_array_end(w);

    sh_json_write_key(w, "middleware");
    sh_json_write_array_start(w);
    EXTRACT_ARRAY("__hull_middleware", "pre");
    EXTRACT_ARRAY("__hull_post_middleware", "post");
    sh_json_write_array_end(w);

    #undef EXTRACT_ARRAY

    JS_FreeValue(js->ctx, global);
    return 0;
}
#endif

int hl_agent_routes_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    int rc = -1;
#ifdef HL_ENABLE_LUA
    if (hl_app_context_is_lua(ctx)) rc = agent_routes_lua(ctx, &w);
#endif
#ifdef HL_ENABLE_JS
    if (!hl_app_context_is_lua(ctx)) rc = agent_routes_js(ctx, &w);
#endif

    sh_json_write_object_end(&w);
    return rc;
}

int hl_agent_routes(const char *app_dir, ShJsonBuf *out)
{
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = { .app_dir = app_dir };
    if (hl_app_context_init(&ctx, &opts) != 0)
        return write_error(out, "no entry point found");

    int rc = hl_agent_routes_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}

/* ── hl_agent_db_schema ────────────────────────────────────────────── */

static int db_schema_impl(sqlite3 *db, int close_db, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "tables");
    sh_json_write_array_start(&w);

    sqlite3_stmt *tables_stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name",
        -1, &tables_stmt, NULL);

    if (rc == SQLITE_OK) {
        while (sqlite3_step(tables_stmt) == SQLITE_ROW) {
            const char *table_name = (const char *)sqlite3_column_text(tables_stmt, 0);

            sh_json_write_object_start(&w);
            sh_json_write_kv_string(&w, "name", table_name);
            sh_json_write_key(&w, "columns");
            sh_json_write_array_start(&w);

            char pragma[512];
            snprintf(pragma, sizeof(pragma), "PRAGMA table_info(\"%s\")", table_name);

            sqlite3_stmt *col_stmt = NULL;
            int rc2 = sqlite3_prepare_v2(db, pragma, -1, &col_stmt, NULL);
            if (rc2 == SQLITE_OK) {
                while (sqlite3_step(col_stmt) == SQLITE_ROW) {
                    const char *col_name = (const char *)sqlite3_column_text(col_stmt, 1);
                    const char *col_type = (const char *)sqlite3_column_text(col_stmt, 2);
                    int notnull = sqlite3_column_int(col_stmt, 3);
                    int pk = sqlite3_column_int(col_stmt, 5);

                    sh_json_write_object_start(&w);
                    sh_json_write_kv_string(&w, "name", col_name);
                    sh_json_write_kv_string(&w, "type", col_type ? col_type : "");
                    if (notnull) sh_json_write_kv_bool(&w, "notnull", true);
                    if (pk) sh_json_write_kv_bool(&w, "pk", true);

                    if (sqlite3_column_type(col_stmt, 4) != SQLITE_NULL) {
                        const char *def = (const char *)sqlite3_column_text(col_stmt, 4);
                        sh_json_write_kv_string(&w, "default", def);
                    }

                    sh_json_write_object_end(&w);
                }
                sqlite3_finalize(col_stmt);
            }

            sh_json_write_array_end(&w);
            sh_json_write_object_end(&w);
        }
        sqlite3_finalize(tables_stmt);
    }

    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    if (close_db) sqlite3_close(db);
    return 0;
}

int hl_agent_db_schema_ctx(HlAppContext *ctx, const char *db_path, ShJsonBuf *out)
{
    if (db_path) {
        /* Explicit db_path: open a separate read-only connection */
        sqlite3 *db = open_app_db(hl_app_context_app_dir(ctx), db_path);
        if (!db)
            return write_error(out, "cannot open database");
        return db_schema_impl(db, 1, out);
    }
    /* No db_path: use the context's :memory: database */
    return db_schema_impl(hl_app_context_db(ctx), 0, out);
}

int hl_agent_db_schema(const char *app_dir, const char *db_path, ShJsonBuf *out)
{
    sqlite3 *db = open_app_db(app_dir, db_path);
    if (!db)
        return write_error(out, "cannot open database");
    return db_schema_impl(db, 1, out);
}

/* ── hl_agent_db_query ─────────────────────────────────────────────── */

static int db_query_impl(sqlite3 *db, int close_db, const char *sql,
                          ShJsonBuf *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        int ret = write_error(out, sqlite3_errmsg(db));
        if (close_db) sqlite3_close(db);
        return ret;
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    /* Column names */
    int ncols = sqlite3_column_count(stmt);
    sh_json_write_key(&w, "columns");
    sh_json_write_array_start(&w);
    for (int i = 0; i < ncols; i++)
        sh_json_write_string(&w, sqlite3_column_name(stmt, i));
    sh_json_write_array_end(&w);

    /* Rows */
    sh_json_write_key(&w, "rows");
    sh_json_write_array_start(&w);

    int row_count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sh_json_write_array_start(&w);
        for (int i = 0; i < ncols; i++) {
            switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:
                sh_json_write_null(&w);
                break;
            case SQLITE_INTEGER:
                sh_json_write_int(&w, sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                sh_json_write_double(&w, sqlite3_column_double(stmt, i));
                break;
            case SQLITE_TEXT:
                sh_json_write_string(&w, (const char *)sqlite3_column_text(stmt, i));
                break;
            case SQLITE_BLOB: {
                char blob_str[64];
                snprintf(blob_str, sizeof(blob_str), "<blob:%d>",
                         sqlite3_column_bytes(stmt, i));
                sh_json_write_string(&w, blob_str);
                break;
            }
            }
        }
        sh_json_write_array_end(&w);
        row_count++;
    }

    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "count", row_count);

    if (rc != SQLITE_DONE)
        sh_json_write_kv_string(&w, "error", sqlite3_errmsg(db));

    sh_json_write_object_end(&w);
    sqlite3_finalize(stmt);
    if (close_db) sqlite3_close(db);
    return 0;
}

int hl_agent_db_query_ctx(HlAppContext *ctx, const char *db_path,
                          const char *sql, ShJsonBuf *out)
{
    if (!sql)
        return write_error(out, "SQL argument required");

    if (db_path) {
        sqlite3 *db = open_app_db(hl_app_context_app_dir(ctx), db_path);
        if (!db)
            return write_error(out, "cannot open database");
        return db_query_impl(db, 1, sql, out);
    }
    return db_query_impl(hl_app_context_db(ctx), 0, sql, out);
}

int hl_agent_db_query(const char *app_dir, const char *db_path,
                      const char *sql, ShJsonBuf *out)
{
    if (!sql)
        return write_error(out, "SQL argument required");

    sqlite3 *db = open_app_db(app_dir, db_path);
    if (!db)
        return write_error(out, "cannot open database");
    return db_query_impl(db, 1, sql, out);
}

/* ── hl_agent_request ──────────────────────────────────────────────── */

int hl_agent_request(const char *method, const char *path, int port,
                     const char *body, const char **headers, int nhdrs,
                     ShJsonBuf *out)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return write_error(out, "cannot create socket");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct timeval start, end;
    gettimeofday(&start, NULL);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        char err[128];
        snprintf(err, sizeof(err), "cannot connect to 127.0.0.1:%d", port);
        return write_error(out, err);
    }

    /* Build HTTP request */
    size_t body_len = body ? strlen(body) : 0;
    char req_buf[8192];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Connection: close\r\n",
        method, path, port);

    if (body_len > 0)
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (size_t)req_len,
            "Content-Length: %zu\r\n", body_len);

    for (int i = 0; i < nhdrs; i++)
        req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (size_t)req_len,
            "%s\r\n", headers[i]);

    req_len += snprintf(req_buf + req_len, sizeof(req_buf) - (size_t)req_len, "\r\n");

    ssize_t sent = send(sock, req_buf, (size_t)req_len, 0);
    if (sent < 0) {
        close(sock);
        return write_error(out, "send failed");
    }

    if (body && body_len > 0) {
        sent = send(sock, body, body_len, 0);
        if (sent < 0) {
            close(sock);
            return write_error(out, "send body failed");
        }
    }

    /* Read response */
    char *resp_buf = malloc(1024 * 1024);
    if (!resp_buf) {
        close(sock);
        return write_error(out, "allocation failed");
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

    /* Parse status line */
    int status = 0;
    const char *header_end = strstr(resp_buf, "\r\n\r\n");
    if (!header_end) {
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, out);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "error", "malformed response");
        sh_json_write_kv_string(&w, "raw", resp_buf);
        sh_json_write_object_end(&w);
        free(resp_buf);
        return -1;
    }

    if (resp_len > 12) {
        char *sp = memchr(resp_buf, ' ', 12);
        if (sp) status = (int)strtol(sp + 1, NULL, 10);
    }

    const char *resp_body = header_end + 4;
    size_t resp_body_len = resp_len - (size_t)(resp_body - resp_buf);

    /* Write JSON output */
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "status", status);
    sh_json_write_kv_int(&w, "elapsed_ms", elapsed_ms);

    /* Response headers */
    sh_json_write_key(&w, "headers");
    sh_json_write_object_start(&w);

    const char *line = resp_buf;
    const char *first_crlf = strstr(line, "\r\n");
    if (first_crlf) line = first_crlf + 2;

    while (line < header_end) {
        const char *next = strstr(line, "\r\n");
        if (!next || next == line) break;

        const char *colon = memchr(line, ':', (size_t)(next - line));
        if (colon) {
            size_t key_len = (size_t)(colon - line);
            char key_buf[256];
            if (key_len >= sizeof(key_buf)) key_len = sizeof(key_buf) - 1;
            memcpy(key_buf, line, key_len);
            key_buf[key_len] = '\0';

            const char *val = colon + 1;
            while (val < next && *val == ' ') val++;
            size_t val_len = (size_t)(next - val);
            char val_buf[4096];
            if (val_len >= sizeof(val_buf)) val_len = sizeof(val_buf) - 1;
            memcpy(val_buf, val, val_len);
            val_buf[val_len] = '\0';

            sh_json_write_kv_string(&w, key_buf, val_buf);
        }
        line = next + 2;
    }

    sh_json_write_object_end(&w);

    /* Body */
    sh_json_write_kv_string(&w, "body", resp_body);
    (void)resp_body_len;

    sh_json_write_object_end(&w);
    free(resp_buf);
    return 0;
}

/* ── hl_agent_status ───────────────────────────────────────────────── */

int hl_agent_status(const char *app_dir, int port, ShJsonBuf *out)
{
    /* Try to read .hull/dev.json for port info */
    char dev_json_path[PATH_MAX];
    snprintf(dev_json_path, sizeof(dev_json_path), "%s/.hull/dev.json", app_dir);

    FILE *f = fopen(dev_json_path, "r");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        const char *port_key = strstr(buf, "\"port\":");
        if (port_key) {
            int p = (int)strtol(port_key + 7, NULL, 10);
            if (p > 0) port = p;
        }
    }

    /* Probe with socket connect */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int connected = 0;
    if (sock >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        connected = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        close(sock);
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "running", connected != 0);
    sh_json_write_kv_int(&w, "port", port);
    sh_json_write_object_end(&w);
    return 0;
}

/* ── hl_agent_errors ───────────────────────────────────────────────── */

int hl_agent_errors(const char *app_dir, ShJsonBuf *out)
{
    char err_path[PATH_MAX];
    snprintf(err_path, sizeof(err_path), "%s/.hull/last_error.json", app_dir);

    FILE *f = fopen(err_path, "r");
    if (!f) {
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, out);
        sh_json_write_object_start(&w);
        sh_json_write_key(&w, "errors");
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
        sh_json_write_object_end(&w);
        return 0;
    }

    /* Read and pass through the JSON file */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long flen = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    if (flen <= 0 || flen > 1024 * 1024) {
        fclose(f);
        return -1;
    }
    char *buf = malloc((size_t)flen + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)flen, f);
    buf[n] = '\0';
    fclose(f);

    /* Pass through raw JSON — write directly to buffer */
    sh_json_buf_write(out, buf, n);
    free(buf);
    return 0;
}

/* ── hl_agent_test ─────────────────────────────────────────────────── */

#define MAX_TEST_RESULTS 256

static void write_test_results(ShJsonWriter *w, HlTestCaseResult *results,
                               int total)
{
    sh_json_write_key(w, "tests");
    sh_json_write_array_start(w);
    for (int i = 0; i < total && i < MAX_TEST_RESULTS; i++) {
        sh_json_write_object_start(w);
        sh_json_write_kv_string(w, "name", results[i].name);
        sh_json_write_kv_string(w, "status", results[i].passed ? "pass" : "fail");
        if (!results[i].passed && results[i].error[0])
            sh_json_write_kv_string(w, "error", results[i].error);
        sh_json_write_object_end(w);
    }
    sh_json_write_array_end(w);
}

/* ── Entry point detection (for test) ─────────────────────────────── */

static const char *detect_entry(const char *app_dir, const char *ext,
                                char *buf, size_t buf_size)
{
    size_t dir_len = strlen(app_dir);
    while (dir_len > 1 && app_dir[dir_len - 1] == '/')
        dir_len--;

    snprintf(buf, buf_size, "%.*s/app.%s", (int)dir_len, app_dir, ext);
    if (access(buf, F_OK) == 0) return buf;
    return NULL;
}

#ifdef HL_ENABLE_LUA
static int agent_test_lua_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    HlLua *lua = hl_app_context_lua(ctx);
    const char *app_dir = hl_app_context_app_dir(ctx);

    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_lua_wire_routes(lua, &router) != 0) {
        kl_router_free(&router);
        return write_error(out, "no routes registered");
    }

    hl_cap_test_register_lua(lua->L, &router, lua);

    char **test_files = hl_tool_find_files(app_dir, "test_*.lua", NULL);
    if (!test_files || !test_files[0]) {
        if (test_files) free(test_files);
        kl_router_free(&router);
        return write_error(out, "no test files found");
    }

    int grand_total = 0, grand_passed = 0, grand_failed = 0;
    HlTestCaseResult results[MAX_TEST_RESULTS];

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "runtime", "lua");
    sh_json_write_key(&w, "files");
    sh_json_write_array_start(&w);

    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        hl_cap_test_clear_lua(lua->L);

        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name", basename);

        if (luaL_dofile(lua->L, file) != LUA_OK) {
            const char *err = lua_tostring(lua->L, -1);
            sh_json_write_kv_string(&w, "error", err ? err : "unknown");
            sh_json_write_key(&w, "tests");
            sh_json_write_array_start(&w);
            sh_json_write_array_end(&w);
            sh_json_write_object_end(&w);
            lua_pop(lua->L, 1);
            grand_failed++;
            grand_total++;
            free(*fp);
            continue;
        }

        int file_total = 0, file_passed = 0, file_failed = 0;
        memset(results, 0, sizeof(results));
        hl_cap_test_run_lua(lua->L, &file_total, &file_passed, &file_failed,
                            NULL, results, MAX_TEST_RESULTS);

        write_test_results(&w, results, file_total);
        sh_json_write_object_end(&w);

        grand_total += file_total;
        grand_passed += file_passed;
        grand_failed += file_failed;
        free(*fp);
    }
    free(test_files);

    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "total", grand_total);
    sh_json_write_kv_int(&w, "passed", grand_passed);
    sh_json_write_kv_int(&w, "failed", grand_failed);
    sh_json_write_object_end(&w);

    kl_router_free(&router);
    return grand_failed > 0 ? 1 : 0;
}
#endif

#ifdef HL_ENABLE_JS
static int agent_test_js_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    HlJS *js = hl_app_context_js(ctx);
    const char *app_dir = hl_app_context_app_dir(ctx);

    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_js_wire_routes(js, &router) != 0) {
        kl_router_free(&router);
        return write_error(out, "no routes registered");
    }

    hl_cap_test_register_js(js->ctx, &router, js);

    char **test_files = hl_tool_find_files(app_dir, "test_*.js", NULL);
    if (!test_files || !test_files[0]) {
        if (test_files) free(test_files);
        kl_router_free(&router);
        return write_error(out, "no test files found");
    }

    int grand_total = 0, grand_passed = 0, grand_failed = 0;
    HlTestCaseResult results[MAX_TEST_RESULTS];

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "runtime", "js");
    sh_json_write_key(&w, "files");
    sh_json_write_array_start(&w);

    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        hl_cap_test_clear_js(js->ctx);

        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name", basename);

        /* Read and evaluate the test file */
        FILE *f = fopen(file, "r");
        if (!f) {
            sh_json_write_kv_string(&w, "error", "cannot open file");
            sh_json_write_key(&w, "tests");
            sh_json_write_array_start(&w);
            sh_json_write_array_end(&w);
            sh_json_write_object_end(&w);
            grand_failed++;
            grand_total++;
            free(*fp);
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); free(*fp); continue; }
        long flen = ftell(f);
        if (flen < 0) { fclose(f); free(*fp); continue; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); free(*fp); continue; }
        char *src = malloc((size_t)flen + 1);
        if (!src) { fclose(f); free(*fp); continue; }
        if (fread(src, 1, (size_t)flen, f) != (size_t)flen) {
            free(src); fclose(f); free(*fp); continue;
        }
        src[flen] = '\0';
        fclose(f);

        JSValue result = JS_Eval(js->ctx, src, (size_t)flen, file,
                                 JS_EVAL_TYPE_MODULE);
        free(src);

        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(js->ctx);
            JSValue msg_val = JS_GetPropertyStr(js->ctx, exc, "message");
            const char *msg = JS_ToCString(js->ctx, msg_val);
            sh_json_write_kv_string(&w, "error", msg ? msg : "unknown");
            sh_json_write_key(&w, "tests");
            sh_json_write_array_start(&w);
            sh_json_write_array_end(&w);
            sh_json_write_object_end(&w);
            if (msg) JS_FreeCString(js->ctx, msg);
            JS_FreeValue(js->ctx, msg_val);
            JS_FreeValue(js->ctx, exc);
            JS_FreeValue(js->ctx, result);
            grand_failed++;
            grand_total++;
            free(*fp);
            continue;
        }
        JS_FreeValue(js->ctx, result);

        int file_total = 0, file_passed = 0, file_failed = 0;
        memset(results, 0, sizeof(results));
        hl_cap_test_run_js(js->ctx, &file_total, &file_passed, &file_failed,
                           NULL, results, MAX_TEST_RESULTS);

        write_test_results(&w, results, file_total);
        sh_json_write_object_end(&w);

        grand_total += file_total;
        grand_passed += file_passed;
        grand_failed += file_failed;
        free(*fp);
    }
    free(test_files);

    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "total", grand_total);
    sh_json_write_kv_int(&w, "passed", grand_passed);
    sh_json_write_kv_int(&w, "failed", grand_failed);
    sh_json_write_object_end(&w);

    kl_router_free(&router);
    return grand_failed > 0 ? 1 : 0;
}
#endif

int hl_agent_test_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
#ifdef HL_ENABLE_LUA
    if (hl_app_context_is_lua(ctx))
        return agent_test_lua_ctx(ctx, out);
#endif
#ifdef HL_ENABLE_JS
    if (!hl_app_context_is_lua(ctx))
        return agent_test_js_ctx(ctx, out);
#endif

    (void)ctx;
    return write_error(out, "no runtime available");
}

int hl_agent_test(const char *app_dir, ShJsonBuf *out)
{
    char entry_buf[4096];
    const char *entry = NULL;

#ifdef HL_ENABLE_LUA
    entry = detect_entry(app_dir, "lua", entry_buf, sizeof(entry_buf));
#endif
#ifdef HL_ENABLE_JS
    if (!entry)
        entry = detect_entry(app_dir, "js", entry_buf, sizeof(entry_buf));
#endif

    // cppcheck-suppress knownConditionTrueFalse
    if (!entry)
        return write_error(out, "no entry point found");

    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = { .app_dir = app_dir, .entry_point = entry };
    if (hl_app_context_init(&ctx, &opts) != 0)
        return write_error(out, "runtime init failed");

    int rc = hl_agent_test_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}

/* ── Phase 2: hl_agent_context ─────────────────────────────────────── */

int hl_agent_context(const char *task, const char *level, ShJsonBuf *out)
{
    if (!task || !level)
        return write_error(out, "task and level are required");

    /* Look up context doc from platform VFS */
    extern const HlEntry hl_stdlib_entries[];
    HlVfs platform_vfs;
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);

    char key[128];
    snprintf(key, sizeof(key), "context:%s", task);

    const HlEntry *entry = hl_vfs_find(&platform_vfs, key);
    if (!entry)
        return write_error(out, "unknown task");

    /* Find the content boundaries based on level markers */
    const char *content = (const char *)entry->data;
    size_t content_len = entry->len;

    /* Level markers in the document */
    const char *minimal_marker = "<!-- minimal -->";
    const char *compact_marker = "<!-- compact -->";
    const char *full_marker    = "<!-- full -->";

    /* Find the start of content (after the level marker for the requested level) */
    const char *start = content;
    size_t start_offset = 0;

    /* Find marker positions */
    const char *minimal_pos = NULL;
    const char *compact_pos = NULL;
    const char *full_pos = NULL;

    /* Scan for markers */
    for (size_t i = 0; i + 15 < content_len; i++) {
        if (memcmp(content + i, "<!-- ", 5) == 0) {
            if (i + strlen(minimal_marker) <= content_len &&
                memcmp(content + i, minimal_marker, strlen(minimal_marker)) == 0) {
                minimal_pos = content + i + strlen(minimal_marker);
            }
            if (i + strlen(compact_marker) <= content_len &&
                memcmp(content + i, compact_marker, strlen(compact_marker)) == 0) {
                compact_pos = content + i + strlen(compact_marker);
            }
            if (i + strlen(full_marker) <= content_len &&
                memcmp(content + i, full_marker, strlen(full_marker)) == 0) {
                full_pos = content + i + strlen(full_marker);
            }
        }
    }

    /* Determine content range based on level */
    const char *end_ptr = content + content_len;

    if (strcmp(level, "minimal") == 0) {
        if (minimal_pos) start = minimal_pos;
        if (compact_pos) end_ptr = compact_pos - strlen(compact_marker);
        else if (full_pos) end_ptr = full_pos - strlen(full_marker);
    } else if (strcmp(level, "compact") == 0) {
        if (minimal_pos) start = minimal_pos;
        if (full_pos) end_ptr = full_pos - strlen(full_marker);
    } else if (strcmp(level, "full") == 0) {
        if (minimal_pos) start = minimal_pos;
        /* end_ptr stays at end of content */
    } else {
        return write_error(out, "level must be minimal, compact, or full");
    }

    if (end_ptr < start) end_ptr = start;
    start_offset = (size_t)(start - content);
    (void)start_offset;

    /* Skip leading whitespace */
    while (start < end_ptr && (*start == '\n' || *start == '\r'))
        start++;

    /* Trim trailing whitespace */
    while (end_ptr > start && (end_ptr[-1] == '\n' || end_ptr[-1] == '\r' ||
                                end_ptr[-1] == ' '))
        end_ptr--;

    size_t result_len = (size_t)(end_ptr - start);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "task", task);
    sh_json_write_kv_string(&w, "level", level);
    sh_json_write_key(&w, "content");
    sh_json_write_string_n(&w, start, result_len);
    sh_json_write_object_end(&w);
    return 0;
}

/* ── Phase 4: hl_agent_migrate_status ──────────────────────────────── */

static int migrate_status_impl(sqlite3 *db, int close_db, const HlVfs *vfs,
                                ShJsonBuf *out)
{
    HlMigrationStatus *entries = NULL;
    int count = 0;
    int rc = hl_migrate_status(db, vfs, &entries, &count);
    if (rc != 0) {
        if (close_db) sqlite3_close(db);
        return write_error(out, "failed to query migration status");
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    sh_json_write_key(&w, "applied");
    sh_json_write_array_start(&w);
    for (int i = 0; i < count; i++) {
        if (entries[i].applied)
            sh_json_write_string(&w, entries[i].name);
    }
    sh_json_write_array_end(&w);

    sh_json_write_key(&w, "pending");
    sh_json_write_array_start(&w);
    for (int i = 0; i < count; i++) {
        if (!entries[i].applied)
            sh_json_write_string(&w, entries[i].name);
    }
    sh_json_write_array_end(&w);

    sh_json_write_kv_int(&w, "total", count);
    sh_json_write_object_end(&w);

    hl_migrate_status_free(entries, count);
    if (close_db) sqlite3_close(db);
    return 0;
}

int hl_agent_migrate_status_ctx(HlAppContext *ctx, const char *db_path,
                                ShJsonBuf *out)
{
    if (db_path) {
        sqlite3 *db = open_app_db(hl_app_context_app_dir(ctx), db_path);
        if (!db)
            return write_error(out, "cannot open database");
        return migrate_status_impl(db, 1, hl_app_context_app_vfs(ctx), out);
    }
    return migrate_status_impl(hl_app_context_db(ctx), 0,
                               hl_app_context_app_vfs(ctx), out);
}

int hl_agent_migrate_status(const char *app_dir, const char *db_path,
                            ShJsonBuf *out)
{
    sqlite3 *db = open_app_db(app_dir, db_path);
    if (!db)
        return write_error(out, "cannot open database");

    extern const HlEntry hl_app_entries[];
    HlVfs app_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);

    return migrate_status_impl(db, 1, &app_vfs, out);
}

/* ── hl_agent_deploy ───────────────────────────────────────────────── */

static int count_files_in_dir(const char *app_dir, const char *subdir,
                               const char *pattern)
{
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", app_dir, subdir);

    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;

    char **files = hl_tool_find_files(dir_path, pattern, NULL);
    if (!files) return 0;

    int count = 0;
    for (char **fp = files; *fp; fp++) {
        count++;
        free(*fp);
    }
    free(files);
    return count;
}

int hl_agent_deploy(const char *app_dir, ShJsonBuf *out)
{
    /* Detect runtime */
    char entry_buf[PATH_MAX];
    const char *runtime = NULL;
    const char *entry_point = NULL;

#ifdef HL_ENABLE_LUA
    entry_point = detect_entry(app_dir, "lua", entry_buf, sizeof(entry_buf));
    if (entry_point) runtime = "lua";
#endif
#ifdef HL_ENABLE_JS
    if (!entry_point) {
        entry_point = detect_entry(app_dir, "js", entry_buf, sizeof(entry_buf));
        if (entry_point) runtime = "js";
    }
#endif

    if (!entry_point)
        return write_error(out, "no entry point found (app.lua or app.js)");

    /* Initialize app context to extract manifest */
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = {
        .app_dir = app_dir,
        .entry_point = entry_point,
        .no_db = 1,
    };

    HlManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    int has_manifest = 0;

    if (hl_app_context_init(&ctx, &opts) == 0) {
#ifdef HL_ENABLE_LUA
        if (hl_app_context_is_lua(ctx)) {
            HlLua *lua = hl_app_context_lua(ctx);
            if (hl_manifest_extract(lua->L, &manifest, NULL) == 0)
                has_manifest = 1;
        }
#endif
#ifdef HL_ENABLE_JS
        if (!hl_app_context_is_lua(ctx)) {
            HlJS *js = hl_app_context_js(ctx);
            if (hl_manifest_extract_js(js->ctx, &manifest, NULL) == 0)
                has_manifest = 1;
        }
#endif
        hl_app_context_free(ctx);
    }

    /* Scan directories */
    int n_migrations = count_files_in_dir(app_dir, "migrations", "*.sql");
    int n_templates  = count_files_in_dir(app_dir, "templates", "*.html");
    int n_static     = count_files_in_dir(app_dir, "static", "*");
    int n_compute    = count_files_in_dir(app_dir, "compute", "*.wasm");
    int n_shaders    = count_files_in_dir(app_dir, "shaders", "*.wgsl");

    /* Check for existing configs */
    char path_buf[PATH_MAX];
    struct stat st;

    snprintf(path_buf, sizeof(path_buf), "%s/Dockerfile", app_dir);
    int has_dockerfile = (stat(path_buf, &st) == 0);

    snprintf(path_buf, sizeof(path_buf), "%s/fly.toml", app_dir);
    int has_fly_toml = (stat(path_buf, &st) == 0);

    snprintf(path_buf, sizeof(path_buf), "%s/deploy", app_dir);
    int has_systemd = 0;
    if (stat(path_buf, &st) == 0 && S_ISDIR(st.st_mode)) {
        char svc_pattern[PATH_MAX];
        snprintf(svc_pattern, sizeof(svc_pattern), "%s/deploy", app_dir);
        char **svc_files = hl_tool_find_files(svc_pattern, "*.service", NULL);
        if (svc_files) {
            has_systemd = (svc_files[0] != NULL);
            for (char **fp = svc_files; *fp; fp++) free(*fp);
            free(svc_files);
        }
    }

    snprintf(path_buf, sizeof(path_buf), "%s/package.sig", app_dir);
    int has_package_sig = (stat(path_buf, &st) == 0);

    /* Determine if app uses a database */
    int uses_database = (n_migrations > 0);
    if (!uses_database && has_manifest) {
        /* Check manifest for database-related indicators (fs_write for .db, etc.) */
        for (int i = 0; i < manifest.fs_write_count; i++) {
            if (strstr(manifest.fs_write[i], ".db"))
                uses_database = 1;
        }
    }

    /* Write JSON output */
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    sh_json_write_kv_string(&w, "app_dir", app_dir);
    sh_json_write_kv_string(&w, "runtime", runtime);

    const char *entry_basename = strrchr(entry_point, '/');
    entry_basename = entry_basename ? entry_basename + 1 : entry_point;
    sh_json_write_kv_string(&w, "entry_point", entry_basename);

    /* Manifest section */
    sh_json_write_key(&w, "manifest");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "present", has_manifest != 0);

    if (has_manifest) {
        sh_json_write_key(&w, "env");
        sh_json_write_array_start(&w);
        for (int i = 0; i < manifest.env_count; i++)
            sh_json_write_string(&w, manifest.env[i]);
        sh_json_write_array_end(&w);

        sh_json_write_key(&w, "hosts");
        sh_json_write_array_start(&w);
        for (int i = 0; i < manifest.hosts_count; i++)
            sh_json_write_string(&w, manifest.hosts[i]);
        sh_json_write_array_end(&w);

        sh_json_write_key(&w, "fs_write");
        sh_json_write_array_start(&w);
        for (int i = 0; i < manifest.fs_write_count; i++)
            sh_json_write_string(&w, manifest.fs_write[i]);
        sh_json_write_array_end(&w);

        sh_json_write_kv_bool(&w, "database", uses_database != 0);
        sh_json_write_kv_bool(&w, "gpu", manifest.gpu != 0);
        sh_json_write_kv_bool(&w, "compute", manifest.compute != 0);
    }
    sh_json_write_object_end(&w);

    /* Files section */
    sh_json_write_key(&w, "files");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "migrations", n_migrations);
    sh_json_write_kv_int(&w, "templates", n_templates);
    sh_json_write_kv_int(&w, "static", n_static);
    sh_json_write_kv_int(&w, "compute", n_compute);
    sh_json_write_kv_int(&w, "shaders", n_shaders);
    sh_json_write_object_end(&w);

    /* Signature section */
    sh_json_write_key(&w, "signature");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "package_sig", has_package_sig != 0);
    sh_json_write_object_end(&w);

    /* Existing configs */
    sh_json_write_key(&w, "configs");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "dockerfile", has_dockerfile != 0);
    sh_json_write_kv_bool(&w, "systemd", has_systemd != 0);
    sh_json_write_kv_bool(&w, "fly_toml", has_fly_toml != 0);
    sh_json_write_object_end(&w);

    /* Recommendations */
    sh_json_write_key(&w, "recommendations");
    sh_json_write_array_start(&w);
    if (!has_manifest)
        sh_json_write_string(&w, "No manifest found — add app.manifest({}) for env/host declarations");
    if (!has_package_sig)
        sh_json_write_string(&w, "No signature — run hull build --sign for verified deployments");
    if (has_manifest && manifest.env_count == 0 && manifest.hosts_count == 0)
        sh_json_write_string(&w, "Manifest is empty — declare env vars and hosts for better config generation");
    sh_json_write_array_end(&w);

    sh_json_write_object_end(&w);

    hl_manifest_free(&manifest);
    return 0;
}
