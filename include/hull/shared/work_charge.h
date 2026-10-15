/*
 * hull/shared/work_charge.h - instruction-budget units for bulk binding work
 *
 * One rule for both runtimes (Lua lua_hlwork, JS hl_js_budget_charge): a
 * binding that copies or writes N bytes in one call charges N / 8 units
 * BEFORE it does the work, as fs.write / blob.put / the crypto digests do.
 * These helpers give the unit counts for the sends and reads audits 14 / 15
 * found uncharged, so the two runtimes cannot drift.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_WORK_CHARGE_H
#define HL_SHARED_WORK_CHARGE_H

#include <stddef.h>
#include <stdint.h>

/* Bytes copied, read or hashed in one call: one unit per 8. */
static inline uint64_t hl_work_bytes_units(uint64_t bytes)
{
    return bytes / 8;
}

/* One message of @p len bytes framed and queued to each of @p dests
 * connections (ws send = 1, ws.broadcast = the endpoint's connection count):
 * one unit per 8 bytes plus one per frame, per destination (audit 14). */
static inline uint64_t hl_work_send_units(size_t len, size_t dests)
{
    uint64_t per = (uint64_t)len / 8 + 1;
    if (dests && per > UINT64_MAX / dests) return UINT64_MAX;
    return per * (uint64_t)dests;
}

/* An SSE event / comment: Keel writes every line of @p text as its own field
 * (prefix, value and newline, each a separate stream write), so a text of
 * newlines cost several writes per byte. One unit per 8 bytes plus one per
 * line - each CR or LF starts one (audit 14). @p extra is the length of the
 * event name and id, written as one field each. */
static inline uint64_t hl_work_sse_units(const char *text, size_t len, size_t extra)
{
    uint64_t lines = 1;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n' || text[i] == '\r') lines++;
    return ((uint64_t)len + extra) / 8 + lines + 2;
}

#endif /* HL_SHARED_WORK_CHARGE_H */
