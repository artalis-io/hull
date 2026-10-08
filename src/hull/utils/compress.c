/*
 * compress.c - Response compression helper
 *
 * Checks Accept-Encoding and uses Keel's compression vtable to
 * compress response bodies when beneficial.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/utils/compress.h"

#include <string.h>
#include <strings.h>   /* strncasecmp */

/* 1 when the q-value starting at @p q (after "q=") is zero: "0", "0.",
 * "0.0", "0.00", "0.000". */
static int qvalue_is_zero(const char *q, const char *end)
{
    if (q >= end || *q != '0') return 0;
    q++;
    if (q < end && *q == '.') {
        q++;
        while (q < end && *q == '0') q++;
    }
    while (q < end && (*q == ' ' || *q == '\t')) q++;
    return q >= end;
}

/* Does Accept-Encoding accept gzip? Parsed as a list of codings with
 * parameters, not searched for the substring: "gzip;q=0" is a refusal (it
 * got gzip), and "x-gzip" / "*" count as asking for it. */
static int accepts_gzip(KlHttpRequest *req)
{
    const char *ae = kl_http_request_header(req, "Accept-Encoding");
    if (!ae)
        return 0;
    int gzip = -1, star = -1;   /* -1 not named, 0 refused, 1 accepted */
    const char *p = ae;
    while (*p) {
        const char *item_end = strchr(p, ',');
        if (!item_end) item_end = p + strlen(p);
        while (p < item_end && (*p == ' ' || *p == '\t')) p++;
        const char *name = p;
        while (p < item_end && *p != ';' && *p != ' ' && *p != '\t') p++;
        size_t nlen = (size_t)(p - name);
        int ok = 1;
        /* parameters: look for q= */
        while (p < item_end) {
            const char *semi = memchr(p, ';', (size_t)(item_end - p));
            if (!semi) break;
            p = semi + 1;
            while (p < item_end && (*p == ' ' || *p == '\t')) p++;
            if (p + 1 < item_end && (p[0] == 'q' || p[0] == 'Q') && p[1] == '=') {
                const char *qend = memchr(p, ';', (size_t)(item_end - p));
                if (!qend) qend = item_end;
                if (qvalue_is_zero(p + 2, qend)) ok = 0;
            }
        }
        if ((nlen == 4 && strncasecmp(name, "gzip", 4) == 0) ||
            (nlen == 6 && strncasecmp(name, "x-gzip", 6) == 0))
            gzip = ok;
        else if (nlen == 1 && name[0] == '*')
            star = ok;
        p = *item_end ? item_end + 1 : item_end;
    }
    if (gzip >= 0) return gzip;
    return star == 1;
}

int hl_maybe_compress(KlHttpRequest *req, KlHttpResponse *res,
                      KlCompressConfig *cfg,
                      const char *data, size_t len)
{
    if (cfg && len >= HL_COMPRESS_MIN_SIZE && accepts_gzip(req)) {
        if (kl_http_response_body_compress(res, cfg, data, len) == 0) {
            /* When gzip does not shrink the body, Keel keeps the original
             * by BORROWING @p data - bytes the caller frees as it returns
             * (a JS C string, a Lua string the VM may collect), before the
             * response is sent. Copy it instead (audit 9). */
            if (len == 0 || res->body != data)
                return 0; /* success - body + Content-Encoding set */
        }
    }

    /* Fallback: uncompressed */
    return kl_http_response_body_copy(res, data, len) == 0 ? 0 : -1;
}
