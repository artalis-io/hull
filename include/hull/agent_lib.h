/*
 * agent_lib.h — Reusable agent introspection library
 *
 * Transport-agnostic functions for all agent operations.
 * Each function writes complete JSON to an ShJsonBuf.
 * Returns 0 on success, -1 on error (error JSON in buf).
 *
 * Used by: CLI (agent.c), MCP server (mcp.c), HTTP endpoints (agent_api.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_AGENT_LIB_H
#define HL_AGENT_LIB_H

#include <sh_json.h>

/* Forward declaration */
typedef struct HlAppContext HlAppContext;

/* ── Core agent operations ─────────────────────────────────────────── */

int hl_agent_routes(const char *app_dir, ShJsonBuf *out);

/* ── Warm-context variants (for MCP / long-lived sessions) ─────────── */

int hl_agent_routes_ctx(HlAppContext *ctx, ShJsonBuf *out);
int hl_agent_db_schema(const char *app_dir, const char *db_path, ShJsonBuf *out);
int hl_agent_db_query(const char *app_dir, const char *db_path,
                      const char *sql, ShJsonBuf *out);
int hl_agent_request(const char *method, const char *path, int port,
                     const char *body, const char **headers, int nhdrs,
                     ShJsonBuf *out);
int hl_agent_status(const char *app_dir, int port, ShJsonBuf *out);
int hl_agent_errors(const char *app_dir, ShJsonBuf *out);
int hl_agent_test(const char *app_dir, ShJsonBuf *out);

/* ── Phase 2: Context ──────────────────────────────────────────────── */

int hl_agent_context(const char *task, const char *level, ShJsonBuf *out);

/* ── Phase 4: Lifecycle ────────────────────────────────────────────── */

int hl_agent_migrate_status(const char *app_dir, const char *db_path,
                            ShJsonBuf *out);

#endif /* HL_AGENT_LIB_H */
