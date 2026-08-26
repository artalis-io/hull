/*
 * audit.c - Capability audit logging
 *
 * Streams structured JSON lines to stderr via ShJsonWriter.
 * When hl_audit_enabled == 0, hl_audit_begin returns a writer with
 * error=1 - all subsequent writes become no-ops.  Zero overhead.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/audit.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int hl_audit_enabled = 0;

/* Per-record scratch buffer. Audit records are emitted from BOTH the event
 * loop and worker threads (compute.async / gpu.async audit from their
 * work_fn). The ShJsonWriter emits token-by-token, so writing straight to
 * stderr lets two records interleave byte-wise and corrupt the JSONL.
 * Instead each record accumulates into a thread-local buffer and is flushed
 * with ONE fwrite, which the stdio stream lock makes atomic relative to
 * other threads. A record is always built start-to-finish on one thread
 * (tight begin/end pair, no nesting), so the buffer needs no further lock. */
#define HL_AUDIT_BUF_MAX 4096
typedef struct { char data[HL_AUDIT_BUF_MAX]; size_t len; } HlAuditBuf;
static _Thread_local HlAuditBuf g_audit_buf;

static int audit_buf_write(void *ctx, const char *data, size_t len)
{
    HlAuditBuf *b = (HlAuditBuf *)ctx;
    if (!b) return -1;
    size_t avail = sizeof(b->data) - b->len;
    if (len > avail) len = avail; /* truncate oversized records (rare) */
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

ShJsonWriter hl_audit_begin(const char *cap)
{
    ShJsonWriter w;

    if (!hl_audit_enabled) {
        /* Return a writer with error=1 - all writes become no-ops */
        sh_json_writer_init(&w, audit_buf_write, NULL);
        w.error = 1;
        return w;
    }

    g_audit_buf.len = 0;
    sh_json_writer_init(&w, audit_buf_write, &g_audit_buf);
    sh_json_write_object_start(&w);

    /* Timestamp: ISO 8601 UTC. L4: gmtime_r can fail (e.g. very large
     * negative time_t on platforms that don't normalize) - in that case
     * `tm` is undefined and strftime would print garbage. */
    {
        time_t now = time(NULL);
        struct tm tm;
        if (gmtime_r(&now, &tm) == NULL) {
            sh_json_write_kv_string(&w, "ts", "?");
        } else {
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
            sh_json_write_kv_string(&w, "ts", ts);
        }
    }

    sh_json_write_kv_string(&w, "cap", cap);

    return w;
}

void hl_audit_end(ShJsonWriter *w)
{
    if (!w || w->error)
        return;

    sh_json_write_object_end(w);
    /* Append the newline into the buffer, then emit the whole record with a
     * single fwrite so concurrent event-loop / worker audit lines cannot
     * interleave byte-wise on the shared stderr. */
    if (g_audit_buf.len < sizeof(g_audit_buf.data))
        g_audit_buf.data[g_audit_buf.len++] = '\n';
    fwrite(g_audit_buf.data, 1, g_audit_buf.len, stderr);
}
