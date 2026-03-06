/*
 * audit.c — Capability audit logging
 *
 * Streams structured JSON lines to stderr via ShJsonWriter.
 * When hl_audit_enabled == 0, hl_audit_begin returns a writer with
 * error=1 — all subsequent writes become no-ops.  Zero overhead.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/audit.h"
#include <stdio.h>
#include <time.h>

int hl_audit_enabled = 0;

static int audit_stderr_write(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    size_t w = fwrite(data, 1, len, stderr);
    return w == len ? 0 : -1;
}

ShJsonWriter hl_audit_begin(const char *cap)
{
    ShJsonWriter w;

    if (!hl_audit_enabled) {
        /* Return a writer with error=1 — all writes become no-ops */
        sh_json_writer_init(&w, audit_stderr_write, NULL);
        w.error = 1;
        return w;
    }

    sh_json_writer_init(&w, audit_stderr_write, NULL);
    sh_json_write_object_start(&w);

    /* Timestamp: ISO 8601 UTC */
    {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
        sh_json_write_kv_string(&w, "ts", ts);
    }

    sh_json_write_kv_string(&w, "cap", cap);

    return w;
}

void hl_audit_end(ShJsonWriter *w)
{
    if (!w || w->error)
        return;

    sh_json_write_object_end(w);
    /* Write newline directly to stderr (not through JSON writer) */
    fputc('\n', stderr);
}
