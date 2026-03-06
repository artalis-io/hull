/*
 * audit.h — Capability audit logging
 *
 * Structured JSON audit lines to stderr, gated by hl_audit_enabled.
 * Zero overhead when disabled — writer returned with error=1 makes
 * all subsequent sh_json_write_* calls no-ops.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_AUDIT_H
#define HL_CAP_AUDIT_H

#include <sh_json.h>

extern int hl_audit_enabled;

ShJsonWriter hl_audit_begin(const char *cap);
void hl_audit_end(ShJsonWriter *w);

#endif /* HL_CAP_AUDIT_H */
