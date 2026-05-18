/**
 * @file agent_lib.h
 * @brief Transport-agnostic agent introspection library.
 *
 * Every `hull agent <verb>` subcommand, every `mcp/<verb>` MCP method,
 * and every `/_agent/*` HTTP endpoint funnels into one of the functions
 * declared here. Each writes a complete JSON document to an
 * `ShJsonBuf`, returning 0 on success and -1 on error (in which case
 * the buf contains an error envelope `{"error": "..."}`).
 *
 * @par Function families:
 *   - **Plain variant** (`hl_agent_<verb>`) — takes `app_dir` and creates
 *     a fresh #HlAppContext per call. Used by the CLI.
 *   - **`_ctx` variant** (`hl_agent_<verb>_ctx`) — reuses a long-lived
 *     #HlAppContext. Used by the MCP server and HTTP endpoints to avoid
 *     per-request init.
 *
 * @par Consumers:
 *   `src/hull/commands/agent.c` (CLI), `src/hull/commands/mcp.c` (MCP
 *   server), `src/hull/agent_api.c` (HTTP `/_agent/*` endpoints).
 *
 * Stability: Tier 3 (internal — used across binaries but not pinned).
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
#ifdef HL_ENABLE_DB
int hl_agent_db_schema_ctx(HlAppContext *ctx, const char *db_path, ShJsonBuf *out);
int hl_agent_db_query_ctx(HlAppContext *ctx, const char *db_path,
                          const char *sql, ShJsonBuf *out);
#endif
int hl_agent_test_ctx(HlAppContext *ctx, ShJsonBuf *out);
#ifdef HL_ENABLE_DB
int hl_agent_migrate_status_ctx(HlAppContext *ctx, const char *db_path,
                                ShJsonBuf *out);

int hl_agent_db_schema(const char *app_dir, const char *db_path, ShJsonBuf *out);
int hl_agent_db_query(const char *app_dir, const char *db_path,
                      const char *sql, ShJsonBuf *out);
#endif
int hl_agent_request(const char *method, const char *path, int port,
                     const char *body, const char **headers, int nhdrs,
                     ShJsonBuf *out);
int hl_agent_status(const char *app_dir, int port, ShJsonBuf *out);
int hl_agent_errors(const char *app_dir, ShJsonBuf *out);
int hl_agent_test(const char *app_dir, ShJsonBuf *out);

/* ── Phase 2: Context ──────────────────────────────────────────────── */

int hl_agent_context(const char *task, const char *level, ShJsonBuf *out);

/* ── Phase 4: Lifecycle ────────────────────────────────────────────── */

#ifdef HL_ENABLE_DB
int hl_agent_migrate_status(const char *app_dir, const char *db_path,
                            ShJsonBuf *out);
#endif

/* ── Phase 5: Deploy ──────────────────────────────────────────────── */

int hl_agent_deploy(const char *app_dir, ShJsonBuf *out);

/* ── Phase 6: Extended introspection (2026-05-15) ──────────────────── */

/*
 * hl_agent_manifest — emit the effective manifest as JSON.
 *
 * Loads the app, runs hl_manifest_extract_*, and serializes the resulting
 * HlManifest to JSON. Different from `hull manifest` which prints a hash.
 */
int hl_agent_manifest(const char *app_dir, ShJsonBuf *out);
int hl_agent_manifest_ctx(HlAppContext *ctx, ShJsonBuf *out);

/*
 * hl_agent_endpoint — preview which handler + middleware would fire for
 * METHOD + PATH without running the request. Pattern matching mirrors
 * Keel's router.
 */
int hl_agent_endpoint(const char *app_dir, const char *method, const char *path,
                      ShJsonBuf *out);
int hl_agent_endpoint_ctx(HlAppContext *ctx, const char *method,
                          const char *path, ShJsonBuf *out);

/*
 * hl_agent_middleware — list middleware that match METHOD + PATH.
 * A focused subset of hl_agent_routes for one specific request shape.
 */
int hl_agent_middleware(const char *app_dir, const char *method,
                        const char *path, ShJsonBuf *out);
int hl_agent_middleware_ctx(HlAppContext *ctx, const char *method,
                            const char *path, ShJsonBuf *out);

/*
 * hl_agent_validate — parse one Lua/JS file in isolation, emit syntax
 * and basic sandbox-violation findings as JSON. Faster than `hull dev`
 * for iterative editing.
 */
int hl_agent_validate(const char *file_path, ShJsonBuf *out);

/*
 * hl_agent_capabilities — source-walk the app to find caps it USES vs
 * caps it DECLARES (manifest). Surfaces declared-but-unused and
 * used-but-undeclared.
 */
int hl_agent_capabilities(const char *app_dir, ShJsonBuf *out);
int hl_agent_capabilities_ctx(HlAppContext *ctx, ShJsonBuf *out);

/*
 * hl_agent_vfs — list embedded files (path → size) for the loaded app
 * VFS and the platform stdlib VFS.
 */
int hl_agent_vfs(const char *app_dir, ShJsonBuf *out);
int hl_agent_vfs_ctx(HlAppContext *ctx, ShJsonBuf *out);

/*
 * hl_agent_modules — emit the app's declared first-party stdlib modules
 * crossed with the canonical registry. Agents use it to discover what
 * the app imports and what the build can satisfy. Output shape:
 *
 *   { "declared": { "crypto": "1", "fs": "1" },
 *     "intrinsic": ["hull/app","hull/log","hull/json"],
 *     "build_caps": { "db": true, "wasm": true, "gpu": false },
 *     "registry_count": 39 }
 */
int hl_agent_modules(const char *app_dir, ShJsonBuf *out);
int hl_agent_modules_ctx(HlAppContext *ctx, ShJsonBuf *out);

/*
 * hl_agent_compute — list WASM compute modules in app_dir/compute/ +
 * their sizes (and AOT variant presence). Module-level metadata.
 */
int hl_agent_compute(const char *app_dir, ShJsonBuf *out);
int hl_agent_compute_ctx(HlAppContext *ctx, ShJsonBuf *out);

/*
 * hl_agent_compute_call — invoke a WASM compute module with input read
 * from a file, return stdout as base64 + length. For isolated testing
 * of compute modules without running the full app.
 */
int hl_agent_compute_call_ctx(HlAppContext *ctx, const char *module_name,
                              const char *input_path, ShJsonBuf *out);

/*
 * hl_agent_gpu — list compiled WGSL shaders in app_dir/shaders/ + device
 * info if GPU is available.
 */
int hl_agent_gpu(const char *app_dir, ShJsonBuf *out);
int hl_agent_gpu_ctx(HlAppContext *ctx, ShJsonBuf *out);

/*
 * hl_agent_perf — runtime stats: heap usage, instruction counts (last
 * known), connection counts. Static snapshot.
 */
int hl_agent_perf_ctx(HlAppContext *ctx, ShJsonBuf *out);

/*
 * hl_agent_template — render a template against sample data; returns
 * the output as JSON. Validates the template path independently of the
 * app routes.
 */
int hl_agent_template_ctx(HlAppContext *ctx, const char *template_name,
                          const char *data_json, ShJsonBuf *out);

/*
 * hl_agent_logs — read structured logs from app_dir/.hull/dev.log if it
 * exists; emit the last N lines as a JSON array. Falls back to empty
 * array if no log file.
 */
int hl_agent_logs(const char *app_dir, int tail_n, ShJsonBuf *out);

/*
 * hl_agent_eval — evaluate a one-shot Lua/JS snippet against the loaded
 * app context, return the result as JSON. Dev mode only; the manifest
 * still constrains capability access.
 */
int hl_agent_eval_ctx(HlAppContext *ctx, const char *code, ShJsonBuf *out);

#ifdef HL_ENABLE_DB
/*
 * hl_agent_schema_diff — compare the live DB schema against what the
 * migrations directory would produce. Reports tables/indexes present
 * in the DB but not in any migration (drift) and vice versa.
 */
int hl_agent_schema_diff(const char *app_dir, const char *db_path, ShJsonBuf *out);
int hl_agent_schema_diff_ctx(HlAppContext *ctx, const char *db_path, ShJsonBuf *out);

/*
 * hl_agent_sql_named — run a pre-defined query from app_dir/queries.json
 * (or queries.yaml). The agent can only invoke queries the developer
 * has explicitly named — no arbitrary SQL.
 */
int hl_agent_sql_named_ctx(HlAppContext *ctx, const char *query_name,
                           const char *params_json, ShJsonBuf *out);
#endif

#endif /* HL_AGENT_LIB_H */
