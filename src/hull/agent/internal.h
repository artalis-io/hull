/*
 * agent/internal.h - Private shared helpers for the agent surface.
 *
 * Not installed; internal to the agent_lib.c split (roadmap item I step 3).
 * Public agent API lives in include/hull/agent_lib.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_AGENT_INTERNAL_H
#define HL_AGENT_INTERNAL_H

#include <stddef.h>
#include <sh_json.h>  /* ShJsonBuf is a typedef of an anonymous struct - needs full include */
#include "hull/agent_lib.h"

struct sqlite3;

/* Emit {"error": msg} as JSON. Always returns -1. */
int hl_agent_write_error(ShJsonBuf *out, const char *msg);

/*
 * Bounded TCP-connect liveness probe to 127.0.0.1:<port> through Keel's socket
 * provider + a private KlEventCtx. Returns 1 iff a connection establishes
 * within timeout_ms, 0 otherwise. Closes the descriptor exactly once.
 * (agent/probe.c; used by hl_agent_status.)
 */
int hl_agent_tcp_probe(int port, int timeout_ms);

#ifdef HL_ENABLE_DB
/*
 * Open an app database for read-only introspection.
 * Tries db_path → app_dir/data.db → :memory: (with migrations applied).
 * Returns a sqlite3 handle the caller must close, or NULL on failure.
 */
struct sqlite3 *hl_agent_open_app_db(const char *app_dir, const char *db_path);
#endif

/*
 * Detect an app entry point (app.lua or app.js) under app_dir.
 * Writes the resolved path into buf and returns a pointer to buf,
 * or NULL if no entry of that extension exists.
 */
const char *hl_agent_detect_entry(const char *app_dir, const char *ext,
                                  char *buf, size_t buf_size);

#endif /* HL_AGENT_INTERNAL_H */
