/*
 * agent/db_stub.c - weak fallbacks for the SQLite-backed agent DB introspection
 * (SQLite as a composable feature, docs/sqlite_feature.md).
 *
 * The `hull agent db|migrate|sql|schema-diff` entry points are implemented
 * against the raw sqlite3 API in agent/{db,sql,schema_diff}.c, which are
 * SQLite-only and live in libhull_feature-sqlite.a; this TU
 * (always compiled into the base whenever the DB umbrella is on) provides WEAK
 * defaults for every public entry point so the base's agent dispatch, in-process
 * agent API, and MCP server link even when the SQLite backend is not composed.
 *
 * The strong definitions in agent/{db,sql,schema_diff}.c override these when
 * SQLite is present (compiled into the base, or supplied by the composed feature
 * archive). When absent, each weak stub emits a
 * clear `{"error": ...}` and returns -1 (hl_agent_write_error's convention), so
 * `hull agent db schema` on, say, a Postgres-only build fails closed with a
 * useful message instead of the command being compiled out.
 *
 * Mirrors http_weakstub.c / wasm_weakstub.c. Guarded on HL_ENABLE_DB (matches
 * where agent_lib.h declares these entry points); a compute-only HL_ENABLE_DB=0
 * build has no agent DB commands at all, so no stub is needed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "internal.h"            /* hl_agent_write_error */
#include "hull/agent_lib.h"      /* the entry-point prototypes */
#include "hull/app_context.h"    /* HlAppContext */

#include <sh_json.h>

/* One message for the whole surface: the introspection is SQLite-specific
 * (sqlite_master / PRAGMA), so it needs the SQLite backend regardless of which
 * DB the app actually uses. */
#define HL_AGENT_DB_ABSENT_MSG \
    "agent database introspection requires the SQLite backend " \
    "(not composed in this build)"

__attribute__((weak))
int hl_agent_db_schema_ctx(HlAppContext *ctx, const char *db_path, ShJsonBuf *out)
{
    (void)ctx; (void)db_path;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_db_schema(const char *app_dir, const char *db_path, ShJsonBuf *out)
{
    (void)app_dir; (void)db_path;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_db_query_ctx(HlAppContext *ctx, const char *db_path,
                          const char *sql, ShJsonBuf *out)
{
    (void)ctx; (void)db_path; (void)sql;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_db_query(const char *app_dir, const char *db_path,
                      const char *sql, ShJsonBuf *out)
{
    (void)app_dir; (void)db_path; (void)sql;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_migrate_status_ctx(HlAppContext *ctx, const char *db_path,
                                ShJsonBuf *out)
{
    (void)ctx; (void)db_path;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_migrate_status(const char *app_dir, const char *db_path,
                            ShJsonBuf *out)
{
    (void)app_dir; (void)db_path;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_schema_diff_ctx(HlAppContext *ctx, const char *db_path,
                             ShJsonBuf *out)
{
    (void)ctx; (void)db_path;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_schema_diff(const char *app_dir, const char *db_path,
                         ShJsonBuf *out)
{
    (void)app_dir; (void)db_path;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

__attribute__((weak))
int hl_agent_sql_named_ctx(HlAppContext *ctx, const char *query_name,
                           const char *params_json, ShJsonBuf *out)
{
    (void)ctx; (void)query_name; (void)params_json;
    return hl_agent_write_error(out, HL_AGENT_DB_ABSENT_MSG);
}

#endif /* HL_ENABLE_DB */
