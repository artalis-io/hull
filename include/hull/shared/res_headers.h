/*
 * hull/shared/res_headers.h - response-header bookkeeping shared by the Lua and
 * JS res.* bindings (runtime/{lua,js}/bindings_response.c).
 *
 * Header-only: both callers live on the HTTP side of the seam (the per-runtime
 * web archives), and these helpers only read / edit the Keel response's header
 * buffer, which holds lines appended as "Name: value\r\n" by
 * kl_http_response_header.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_RES_HEADERS_H
#define HL_SHARED_RES_HEADERS_H

#include <keel/http_response.h>

#include <stddef.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */

#include "hull/limits/runtime.h"  /* HL_RES_HEADER_BYTES_MAX */

/* Length of the header line at @p p (up to and including its '\n'), and
 * whether it is a @p name header (case-insensitive). */
static inline size_t hl_res_header_line(const char *p, const char *end,
                                        const char *name, size_t name_len,
                                        int *is_name)
{
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    size_t line_len = eol ? (size_t)(eol - p) + 1 : (size_t)(end - p);
    *is_name = line_len > name_len && p[name_len] == ':' &&
               strncasecmp(p, name, name_len) == 0;
    return line_len;
}

/* Has a @p name header (case-insensitive) already been added? */
static inline int hl_res_header_has(const KlHttpResponse *res, const char *name)
{
    if (!res || !res->hdr_buf || !name) return 0;
    size_t name_len = strlen(name);
    const char *p   = res->hdr_buf;
    const char *end = res->hdr_buf + res->hdr_len;
    while (p < end) {
        int is_name;
        p += hl_res_header_line(p, end, name, name_len, &is_name);
        if (is_name) return 1;
    }
    return 0;
}

/* Drop every @p name header (case-insensitive) from the response. Used for a
 * header that describes the body when the body is replaced: res.json / html /
 * text called twice re-encode it, and a Content-Encoding the first call's
 * gzip added described bytes that are no longer there (audit 9). */
static inline void hl_res_header_remove(KlHttpResponse *res, const char *name)
{
    if (!res || !res->hdr_buf || !name) return;
    size_t name_len = strlen(name);
    char *p   = res->hdr_buf;
    char *end = res->hdr_buf + res->hdr_len;
    while (p < end) {
        int is_name;
        size_t line_len = hl_res_header_line(p, end, name, name_len, &is_name);
        if (is_name) {
            memmove(p, p + line_len, (size_t)(end - (p + line_len)));
            end -= line_len;
            res->hdr_len -= line_len;
        } else {
            p += line_len;
        }
    }
}

/* Called by res.json / html / text before they store (and maybe gzip) a body:
 * a body an earlier call gzipped left "Content-Encoding" and "Vary:
 * Accept-Encoding" (Keel's http_compress adds both), which described bytes
 * that are about to be replaced - a plain second body went out labelled gzip,
 * and a loop of gzipped bodies stacked a Vary per call outside the header
 * cap (audit 9). Only when a Content-Encoding is present, so an app's own
 * Vary on an uncompressed response stays. */
static inline void hl_res_body_reencode(KlHttpResponse *res)
{
    if (!hl_res_header_has(res, "Content-Encoding")) return;
    hl_res_header_remove(res, "Content-Encoding");
    /* Exactly the line Keel adds; any other Vary is the app's. */
    static const char vary[] = "Vary: Accept-Encoding\r\n";
    const size_t vlen = sizeof(vary) - 1;
    char *p   = res->hdr_buf;
    char *end = res->hdr_buf + res->hdr_len;
    while (p && (size_t)(end - p) >= vlen) {
        if (strncasecmp(p, vary, vlen) == 0) {
            memmove(p, p + vlen, (size_t)(end - (p + vlen)));
            end -= vlen;
            res->hdr_len -= vlen;
            continue;
        }
        char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        p = eol + 1;
    }
}

/* Is there room for one more "Name: value\r\n" under the per-response cap
 * (audit 9 M1)? Keel's header buffer lives outside the script heap, so the
 * heap limit does not bound it: a loop of res.header calls grew it to all of
 * memory. Every header counts - app ones and the ones Hull adds. */
static inline int hl_res_header_fits(const KlHttpResponse *res,
                                     size_t name_len, size_t value_len)
{
    return name_len <= HL_RES_HEADER_BYTES_MAX &&
           value_len <= HL_RES_HEADER_BYTES_MAX &&
           res->hdr_len + name_len + value_len + 4 <= HL_RES_HEADER_BYTES_MAX;
}

#endif /* HL_SHARED_RES_HEADERS_H */
