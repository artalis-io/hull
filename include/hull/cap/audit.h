/**
 * @file cap/audit.h
 * @brief Capability audit logging.
 *
 * Structured JSON audit lines to stderr, gated by #hl_audit_enabled.
 * Zero overhead when disabled — the writer returned by #hl_audit_begin
 * carries `error=1` which makes all subsequent `sh_json_write_*` calls
 * no-ops at the inline-function level.
 *
 * Enable at runtime via `--audit` CLI flag or `HULL_AUDIT=1` env var.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_AUDIT_H
#define HL_CAP_AUDIT_H

#include <sh_json.h>

/**
 * @brief Global enable flag.
 *
 * Set by `main.c` after CLI parsing. Hot-path branch (`if (hl_audit_enabled)`)
 * in every capability function so disabled audit costs one predictable
 * branch.
 */
extern int hl_audit_enabled;

/**
 * @brief Begin an audit log entry.
 *
 * Opens a JSON object on stderr containing `ts` (ISO 8601 UTC) and
 * `cap` (the capability name). Caller appends domain-specific fields via
 * the returned writer, then calls #hl_audit_end.
 *
 * @param cap  Capability name, e.g. `"db.query"`, `"fs.read"`. Static
 *             string lifetime expected.
 *
 * @return An ShJsonWriter targeting stderr. When auditing is
 *         disabled, the writer's `error` flag is set so subsequent
 *         writes are no-ops.
 *
 * @par Example:
 * @code
 * ShJsonWriter w = hl_audit_begin("db.query");
 * sh_json_write_kv_string(&w, "sql", sql);
 * sh_json_write_kv_int(&w, "rows", row_count);
 * hl_audit_end(&w);
 * @endcode
 */
ShJsonWriter hl_audit_begin(const char *cap);

/**
 * @brief Finalize an audit log entry.
 *
 * Closes the JSON object started by #hl_audit_begin and writes a
 * trailing newline. Safe to call on a no-op writer.
 *
 * @param w  Writer returned from #hl_audit_begin.
 */
void hl_audit_end(ShJsonWriter *w);

#endif /* HL_CAP_AUDIT_H */
