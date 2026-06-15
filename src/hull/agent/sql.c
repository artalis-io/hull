/*
 * agent/sql.c — `hull agent sql named <name>`: run a pre-defined query
 * from app_dir/queries.json. Safer than `hull agent db query "..."`
 * because the agent can only invoke named queries the developer has
 * explicitly published.
 *
 * Queries file format (JSON):
 *   {
 *     "list_active_users":   "SELECT id, name FROM users WHERE active = 1",
 *     "user_by_id":          "SELECT * FROM users WHERE id = :id",
 *     "recent_orders":       "SELECT * FROM orders ORDER BY created_at DESC LIMIT :limit"
 *   }
 *
 * Parameters: pass `--params '{"id": 42}'` and named placeholders
 * (`:id`, `:limit`) bind to JSON object values.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"

#include <sh_json.h>
#include <sh_arena.h>
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Slurp queries.json (or .yaml fallback) from app_dir. */
static char *read_queries_file(const char *app_dir, size_t *out_len)
{
    const char *names[] = { "queries.json", NULL };
    for (int i = 0; names[i]; i++) {
        char path[HL_AGENT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", app_dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long n = ftell(f);
        if (n < 0 || n > HL_AGENT_QUERIES_MAX_FILE) { fclose(f); continue; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); continue; }
        /* calloc over malloc so static analysis traces the bound
         * cleanly (n is already capped to HL_AGENT_QUERIES_MAX_FILE
         * above, but the bound is not visible at the allocation
         * call site). */
        char *buf = calloc((size_t)n + 1, 1);
        if (!buf) { fclose(f); continue; }
        size_t got = fread(buf, 1, (size_t)n, f);
        int read_err = ferror(f);
        fclose(f);
        if (read_err || got != (size_t)n) { free(buf); continue; }
        buf[got] = '\0';
        *out_len = got;
        return buf;
    }
    return NULL;
}

int hl_agent_sql_named_ctx(HlAppContext *ctx, const char *query_name,
                           const char *params_json, ShJsonBuf *out)
{
    if (!query_name)
        return hl_agent_write_error(out, "query name required");

    /* Load queries.json from the app's directory. */
    size_t qf_len = 0;
    char *qf = read_queries_file(hl_app_context_app_dir(ctx), &qf_len);
    if (!qf)
        return hl_agent_write_error(out,
            "queries.json not found in app_dir (place named queries there)");

    /* Parse JSON via sh_json (arena-based). */
    SHArena *arena = sh_arena_create(
        qf_len * HL_AGENT_ARENA_SLOPPY_FACTOR + HL_AGENT_ARENA_SLOPPY_SLACK);
    if (!arena) { free(qf); return hl_agent_write_error(out, "out of memory"); }
    ShJsonValue *root = NULL;
    if (sh_json_parse(qf, qf_len, arena, &root) != SH_JSON_OK || !root) {
        sh_arena_free(arena);
        free(qf);
        return hl_agent_write_error(out, "invalid JSON in queries.json");
    }
    if (sh_json_type(root) != SH_JSON_OBJECT) {
        sh_arena_free(arena);
        free(qf);
        return hl_agent_write_error(out, "queries.json must be a JSON object");
    }

    ShJsonValue *q = sh_json_get(root, query_name);
    if (!q || sh_json_type(q) != SH_JSON_STRING) {
        sh_arena_free(arena);
        free(qf);
        return hl_agent_write_error(out, "named query not found");
    }
    const char *sql = sh_json_as_string(q, NULL);
    if (!sql) {
        sh_arena_free(arena);
        free(qf);
        return hl_agent_write_error(out, "query value is not a string");
    }
    size_t sql_len = sh_json_string_len(q);

    /* Run the query. */
    sqlite3 *db = hl_app_context_db(ctx);
    if (!db) {
        sh_arena_free(arena);
        free(qf);
        return hl_agent_write_error(out, "no database open");
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, (int)sql_len, &stmt, NULL) != SQLITE_OK) {
        const char *err = sqlite3_errmsg(db);
        int rc = hl_agent_write_error(out, err ? err : "prepare failed");
        sh_arena_free(arena);
        free(qf);
        return rc;
    }

    /* Bind named parameters from params_json if provided. */
    if (params_json && *params_json) {
        SHArena *parena = sh_arena_create(
            strlen(params_json) * HL_AGENT_ARENA_SLOPPY_FACTOR +
            HL_AGENT_ARENA_SLOPPY_SLACK);
        ShJsonValue *params = NULL;
        if (parena && sh_json_parse(params_json, strlen(params_json),
                                    parena, &params) == SH_JSON_OK && params &&
            sh_json_type(params) == SH_JSON_OBJECT) {
            int ncols = sqlite3_bind_parameter_count(stmt);
            for (int i = 1; i <= ncols; i++) {
                const char *pname = sqlite3_bind_parameter_name(stmt, i);
                if (!pname || pname[0] != ':') continue;
                ShJsonValue *pv = sh_json_get(params, pname + 1);
                if (!pv) continue;
                switch (sh_json_type(pv)) {
                case SH_JSON_NULL:   sqlite3_bind_null(stmt, i); break;
                case SH_JSON_BOOL:   sqlite3_bind_int(stmt, i, sh_json_as_bool(pv, false) ? 1 : 0); break;
                case SH_JSON_NUMBER: sqlite3_bind_double(stmt, i, sh_json_as_double(pv, 0.0)); break;
                case SH_JSON_STRING: {
                    const char *sv = sh_json_as_string(pv, NULL);
                    if (sv) {
                        size_t slen = sh_json_string_len(pv);
                        sqlite3_bind_text(stmt, i, sv, (int)slen, SQLITE_TRANSIENT);
                    }
                    break;
                }
                default: break;
                }
            }
        }
        if (parena) sh_arena_free(parena);
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "name", query_name);

    int ncols = sqlite3_column_count(stmt);
    sh_json_write_key(&w, "columns");
    sh_json_write_array_start(&w);
    for (int i = 0; i < ncols; i++)
        sh_json_write_string(&w, sqlite3_column_name(stmt, i));
    sh_json_write_array_end(&w);

    sh_json_write_key(&w, "rows");
    sh_json_write_array_start(&w);
    int row_count = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sh_json_write_array_start(&w);
        for (int i = 0; i < ncols; i++) {
            switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:
                sh_json_write_null(&w); break;
            case SQLITE_INTEGER:
                sh_json_write_int(&w, sqlite3_column_int64(stmt, i)); break;
            case SQLITE_FLOAT:
                sh_json_write_double(&w, sqlite3_column_double(stmt, i)); break;
            case SQLITE_TEXT:
                sh_json_write_string(&w, (const char *)sqlite3_column_text(stmt, i));
                break;
            case SQLITE_BLOB: {
                char blob_str[HL_AGENT_BLOB_LABEL];
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

    if (step_rc != SQLITE_DONE)
        sh_json_write_kv_string(&w, "error", sqlite3_errmsg(db));

    sh_json_write_object_end(&w);
    sqlite3_finalize(stmt);
    sh_arena_free(arena);
    free(qf);
    return 0;
}

#else /* !HL_ENABLE_DB */
/* ISO C requires a translation unit to contain at least one declaration. */
typedef int hl_agent_sql_disabled;
#endif /* HL_ENABLE_DB */
