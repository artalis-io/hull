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
#include <stdlib.h>   /* malloc / free (HlResBase) */
#include <string.h>
#include <strings.h>  /* strncasecmp */

#include "hull/limits/runtime.h"  /* HL_RES_HEADER_BYTES_MAX */
#include "hull/shared/res_base.h"  /* HlResBaseList */

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

/* ── Hull's default Content-Type ─────────────────────────────────────────
 *
 * res.json / html / text add a Content-Type (Hull's default) unless the app
 * set one with res.header; a later body call replaces Hull's default but
 * keeps the app's (audit 10). Which of the two the response carries is read
 * from the response itself (audit 11): it was a flag on the per-call `res`
 * object, so a middleware's res.html followed by the handler's res.json - two
 * objects, one response - kept text/html for the JSON.
 *
 * Hull's default is the line named exactly "Content-Type" (this casing) whose
 * value is one of the defaults below. An app's res.header("Content-Type", v)
 * is written under that name only when v is NOT a default value; when it is,
 * the app's line goes out as "content-type" (header names are
 * case-insensitive), so it can never be mistaken for Hull's. */

#define HL_RES_CT_JSON "application/json"
#define HL_RES_CT_HTML "text/html; charset=utf-8"
#define HL_RES_CT_TEXT "text/plain; charset=utf-8"

static inline int hl_res_ct_value_is_default(const char *v, size_t n)
{
    static const char *const defaults[] = {
        HL_RES_CT_JSON, HL_RES_CT_HTML, HL_RES_CT_TEXT,
    };
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
        size_t dl = strlen(defaults[i]);
        if (n == dl && memcmp(v, defaults[i], dl) == 0) return 1;
    }
    return 0;
}

/* Does the response carry Hull's default Content-Type (see above)? */
static inline int hl_res_content_type_is_default(const KlHttpResponse *res)
{
    if (!res || !res->hdr_buf) return 0;
    static const char name[] = "Content-Type";
    const size_t name_len = sizeof(name) - 1;
    const char *p   = res->hdr_buf;
    const char *end = res->hdr_buf + res->hdr_len;
    while (p < end) {
        int is_name;
        size_t line_len = hl_res_header_line(p, end, name, name_len, &is_name);
        if (is_name) {
            /* "Content-Type: <value>\r\n", the name in Hull's own casing. */
            if (memcmp(p, name, name_len) != 0 || line_len < name_len + 4 ||
                p[name_len + 1] != ' ' || p[line_len - 2] != '\r')
                return 0;
            return hl_res_ct_value_is_default(p + name_len + 2,
                                              line_len - name_len - 4);
        }
        p += line_len;
    }
    return 0;
}

/* Called by a body call (res.json / html / text) before it adds its default:
 * Hull's earlier default is removed. Returns 1 when the caller must add its
 * own (no Content-Type is left), 0 when the app's stays. */
static inline int hl_res_content_type_prepare(KlHttpResponse *res)
{
    if (hl_res_content_type_is_default(res))
        hl_res_header_remove(res, "Content-Type");
    return !hl_res_header_has(res, "Content-Type");
}

/* res.bytes sets no Content-Type, and res.header("Content-Type", ...) brings
 * the app's own: either way a default an earlier body call added no longer
 * describes the body (res.html then res.bytes(png) went out as text/html). */
static inline void hl_res_drop_default_content_type(KlHttpResponse *res)
{
    if (hl_res_content_type_is_default(res))
        hl_res_header_remove(res, "Content-Type");
}

/* Is @p name (length @p len) the Content-Type header name? */
static inline int hl_res_is_content_type(const char *name, size_t len)
{
    return len == 12 && strncasecmp(name, "Content-Type", 12) == 0;
}

/* The name an app's res.header(@p name, @p value) is written under: @p name,
 * except a Content-Type whose value is one of Hull's defaults, which goes out
 * as "content-type" so it is never taken for Hull's own (see above). */
static inline const char *hl_res_app_header_name(const char *name,
                                                 size_t name_len,
                                                 const char *value)
{
    if (hl_res_is_content_type(name, name_len) && value &&
        hl_res_ct_value_is_default(value, strlen(value)))
        return "content-type";
    return name;
}

/* Replace whatever a handler started with an error answer (audit 10): its
 * headers are dropped - a Set-Cookie or Location it had set went out on the
 * 500, a Content-Encoding described a body that is no longer there, and a
 * Content-Type it had set was followed by a second one - so the answer
 * carries exactly one Content-Type. Nothing to undo once the headers were
 * sent (a stream).
 *
 * @p keep / @p keep_len: the header lines the response had when the handler
 * started - the ones earlier middleware set (CSP, HSTS, CORS, a request id) -
 * which the answer keeps (audit 11: they were dropped with the handler's).
 * Restored from a copy, not by truncating to a length, since a body call may
 * have removed lines before that point. Of them, what describes a body
 * (Content-Type, Content-Encoding and its Vary) still goes. NULL / 0: none. */
static inline void hl_res_error_reset_keep(KlHttpResponse *res, int status,
                                           const char *body, size_t len,
                                           const char *keep, size_t keep_len)
{
    if (!res) return;
    if (!res->headers_sent) {
        res->hdr_len = 0;
        if (keep && keep_len > 0 && res->hdr_buf && keep_len <= res->hdr_cap) {
            memcpy(res->hdr_buf, keep, keep_len);
            res->hdr_len = keep_len;
            hl_res_header_remove(res, "Content-Type");
            hl_res_header_remove(res, "Content-Length");
            hl_res_body_reencode(res);   /* Content-Encoding + Keel's Vary */
        }
    }
    kl_http_response_status(res, status);
    (void)kl_http_response_header(res, "Content-Type", "text/plain");
    kl_http_response_body_borrow(res, body, len);
}

static inline void hl_res_error_reset(KlHttpResponse *res, int status,
                                      const char *body, size_t len)
{
    hl_res_error_reset_keep(res, status, body, len, NULL, 0);
}

/* ── The headers a script entry started with ─────────────────────────────
 *
 * A copy of the response's header lines when a handler OR a middleware starts
 * on it, for hl_res_error_reset_keep: the entry's error answer keeps the
 * headers earlier middleware set and drops the failed entry's own. One per
 * response, in a list each runtime keeps in its own state (HlJS / HlLua
 * res_bases, res_base.h; audit 12 - it was one file-static list per runtime
 * kind, never freed). Every entry replaces the response's snapshot before any
 * of its error paths can answer (so a snapshot is never applied to a request
 * it was not made for), and it is dropped when the entry answers, when the
 * request's connection goes away under a suspended handler (an async
 * cancel), and when the runtime is freed. The list is capped by count and by
 * bytes (res_base.h): past the cap the oldest goes, and that response's error
 * answer drops every header. */

/* An entry starts on @p res (a request on @p conn, NULL if unknown): remember
 * its header lines. Out of memory: none is kept (the error answer then drops
 * them all). */
static inline void hl_res_base_begin(HlResBaseList *l, const KlHttpResponse *res,
                                     const void *conn)
{
    if (!l || !res) return;
    hl_res_base_forget(l, res);
    if (res->hdr_len == 0 || !res->hdr_buf) return;   /* nothing to keep */
    if (res->hdr_len > HL_RES_BASE_BYTES_MAX) return;
    HlResBase *b = (HlResBase *)malloc(sizeof *b);
    if (!b) return;
    b->hdrs = (char *)malloc(res->hdr_len);
    if (!b->hdrs) { free(b); return; }
    memcpy(b->hdrs, res->hdr_buf, res->hdr_len);
    b->len = res->hdr_len;
    b->res = res;
    b->conn = conn;
    b->prev = NULL;
    b->next = l->head;
    if (l->head) l->head->prev = b; else l->tail = b;
    l->head = b;
    l->count++;
    l->bytes += b->len;
    while ((l->count > HL_RES_BASE_MAX || l->bytes > HL_RES_BASE_BYTES_MAX) &&
           l->tail && l->tail != b)
        hl_res_base_drop(l, l->tail);
}

/* The error answer for @p res, keeping its entry-start headers (and dropping
 * the entry). */
static inline void hl_res_base_error_reset(HlResBaseList *l, KlHttpResponse *res,
                                           int status, const char *body,
                                           size_t len)
{
    HlResBase *b = hl_res_base_find(l, res);
    if (!b) {
        hl_res_error_reset(res, status, body, len);
        return;
    }
    hl_res_base_unlink(l, b);
    hl_res_error_reset_keep(res, status, body, len, b->hdrs, b->len);
    free(b->hdrs);
    free(b);
}

/* ── App header names and values ─────────────────────────────────────────
 *
 * res.header(name, value) hands Keel C strings, which it measures with
 * strlen: a name or value with an embedded NUL went out cut at the NUL while
 * Hull's own checks (is it Content-Type?) had looked at the whole string -
 * "Content-Type\0x" passed as an ordinary header and was sent as a second
 * Content-Type (audit 12). A name must be an RFC 9110 token; a value may not
 * hold a NUL (a CR or LF Keel refuses, as before). */

static inline int hl_res_header_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'))
        return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|':
    case '~':
        return 1;
    default:
        return 0;
    }
}

/* Is @p name (length @p len) a header field name (a non-empty token)? */
static inline int hl_res_header_name_valid(const char *name, size_t len)
{
    if (!name || len == 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (!hl_res_header_tchar((unsigned char)name[i])) return 0;
    return 1;
}

/* Does @p value (length @p len) hold no NUL? */
static inline int hl_res_header_value_valid(const char *value, size_t len)
{
    return value && memchr(value, '\0', len) == NULL;
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
