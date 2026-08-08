/*
 * cap/respwire.c: RESP2 / RESP3 codec (Redis / Valkey). See respwire.h.
 *
 * The parser treats every byte as untrusted: each length/count is validated
 * against HL_RESP_MAX_BULK / HL_RESP_MAX_ELEMS before use, nesting is capped at
 * HL_RESP_MAX_DEPTH, and no read ever passes `avail`. Requests are RESP arrays
 * of bulk strings (binary-safe). Fuzzed by fuzz/fuzz_respwire.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/respwire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── writer ───────────────────────────────────────────────────────────── */

void hl_resp_writer_init(HlRespWriter *w) { w->buf = NULL; w->len = w->cap = 0; w->err = 0; }
void hl_resp_writer_free(HlRespWriter *w) { free(w->buf); w->buf = NULL; w->len = w->cap = 0; }
void hl_resp_writer_reset(HlRespWriter *w) { w->len = 0; w->err = 0; }

static int rw_reserve(HlRespWriter *w, size_t extra) {
    if (w->err) return -1;
    if (extra > SIZE_MAX - w->len) { w->err = 1; return -1; }
    size_t need = w->len + extra;
    if (need <= w->cap) return 0;
    size_t ncap = w->cap ? w->cap : 256;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) { ncap = need; break; }
        ncap *= 2;
    }
    uint8_t *nb = realloc(w->buf, ncap);
    if (!nb) { w->err = 1; return -1; }
    w->buf = nb; w->cap = ncap;
    return 0;
}

static void rw_put(HlRespWriter *w, const void *p, size_t n) {
    if (n == 0) return;                 /* memcpy(dst, NULL, 0) is UB */
    if (rw_reserve(w, n) != 0) return;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void hl_resp_cmd_begin(HlRespWriter *w, size_t argc) {
    char h[24];
    int n = snprintf(h, sizeof h, "*%zu\r\n", argc);
    if (n > 0) rw_put(w, h, (size_t)n);
}

void hl_resp_cmd_arg(HlRespWriter *w, const void *arg, size_t len) {
    char h[24];
    int n = snprintf(h, sizeof h, "$%zu\r\n", len);
    if (n > 0) rw_put(w, h, (size_t)n);
    rw_put(w, arg, len);
    rw_put(w, "\r\n", 2);
}

void hl_resp_cmd_arg_cstr(HlRespWriter *w, const char *arg) {
    hl_resp_cmd_arg(w, arg, arg ? strlen(arg) : 0);
}

void hl_resp_cmd_arg_i64(HlRespWriter *w, int64_t v) {
    char b[24];
    int n = snprintf(b, sizeof b, "%lld", (long long)v);
    if (n > 0) hl_resp_cmd_arg(w, b, (size_t)n);
}

static int is_glob_meta(uint8_t b) {
    return b == '\\' || b == '*' || b == '?' || b == '[' || b == ']';
}

void hl_resp_cmd_arg_globprefix(HlRespWriter *w, const void *prefix, size_t plen) {
    const uint8_t *p = (const uint8_t *)prefix;
    size_t esc = 0;
    for (size_t i = 0; i < plen; i++) if (is_glob_meta(p[i])) esc++;
    size_t total = plen + esc + 1;                 /* escaped bytes + trailing '*' */
    char hd[24];
    int n = snprintf(hd, sizeof hd, "$%zu\r\n", total);
    if (n > 0) rw_put(w, hd, (size_t)n);
    for (size_t i = 0; i < plen; i++) {
        if (is_glob_meta(p[i])) { unsigned char bs = '\\'; rw_put(w, &bs, 1); }
        rw_put(w, &p[i], 1);
    }
    unsigned char star = '*';
    rw_put(w, &star, 1);
    rw_put(w, "\r\n", 2);
}

/* ── parser helpers ───────────────────────────────────────────────────── */

/* Parse a signed decimal integer from [p, p+n). Rejects empty, non-digits,
 * and out-of-range. Returns 0 on success. */
static int parse_i64(const uint8_t *p, size_t n, int64_t *out) {
    if (n == 0 || n > 20) return -1;
    size_t i = 0;
    int neg = 0;
    if (p[0] == '-') { neg = 1; i = 1; if (n == 1) return -1; }
    else if (p[0] == '+') { i = 1; if (n == 1) return -1; }
    uint64_t acc = 0;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        uint64_t d = (uint64_t)(p[i] - '0');
        if (acc > (UINT64_MAX - d) / 10) return -1;   /* overflow */
        acc = acc * 10 + d;
    }
    if (neg) { if (acc > (uint64_t)INT64_MAX + 1) return -1; *out = -(int64_t)acc; }
    else     { if (acc > (uint64_t)INT64_MAX)     return -1; *out =  (int64_t)acc; }
    return 0;
}

/* A big number is an optionally-signed run of decimal digits (kept as text). */
static int valid_bignum(const uint8_t *p, size_t n) {
    if (n == 0) return 0;
    size_t i = 0;
    if (p[0] == '-' || p[0] == '+') { i = 1; if (n == 1) return 0; }
    for (; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

/* RESP3 double: strtod over a bounded NUL-terminated copy (inf/nan allowed). */
static int parse_double(const uint8_t *p, size_t n, double *out) {
    char tmp[64];
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    char *end = NULL;
    double d = strtod(tmp, &end);
    if (end != tmp + n) return -1;
    *out = d;
    return 0;
}

/* Recursive single-value parse. Returns OK/NEED_MORE/PARSE_ERR. */
static HlRespResult parse_one(const uint8_t *buf, size_t avail, size_t *consumed,
                              HlRespValue *out, HlRespAlloc alloc, void *ctx, int depth) {
    if (depth > HL_RESP_MAX_DEPTH) return HL_RESP_PARSE_ERR;
    if (avail < 1) return HL_RESP_NEED_MORE;

    /* Locate the CRLF that ends the header line. */
    size_t i = 1;
    while (i + 1 < avail && !(buf[i] == '\r' && buf[i + 1] == '\n')) i++;
    if (i + 1 >= avail) return HL_RESP_NEED_MORE;   /* no CRLF yet */

    const uint8_t *hdr = buf + 1;
    size_t hdrlen = i - 1;
    size_t line_total = i + 2;                       /* through CRLF */
    uint8_t prefix = buf[0];

    switch (prefix) {
    case '+':   /* simple string */
        out->type = HL_RESP_STR; out->str.p = (const char *)hdr; out->str.len = hdrlen;
        *consumed = line_total; return HL_RESP_OK;
    case '-':   /* error */
        out->type = HL_RESP_ERR; out->str.p = (const char *)hdr; out->str.len = hdrlen;
        *consumed = line_total; return HL_RESP_OK;
    case ':': { /* integer */
        int64_t v; if (parse_i64(hdr, hdrlen, &v) != 0) return HL_RESP_PARSE_ERR;
        out->type = HL_RESP_INT; out->i = v; *consumed = line_total; return HL_RESP_OK;
    }
    case '(': { /* big number (kept as text) */
        if (!valid_bignum(hdr, hdrlen)) return HL_RESP_PARSE_ERR;
        out->type = HL_RESP_BIGNUM; out->str.p = (const char *)hdr; out->str.len = hdrlen;
        *consumed = line_total; return HL_RESP_OK;
    }
    case '#': { /* boolean */
        if (hdrlen != 1 || (hdr[0] != 't' && hdr[0] != 'f')) return HL_RESP_PARSE_ERR;
        out->type = HL_RESP_BOOL; out->b = (hdr[0] == 't'); *consumed = line_total; return HL_RESP_OK;
    }
    case '_': /* null (RESP3) */
        if (hdrlen != 0) return HL_RESP_PARSE_ERR;
        out->type = HL_RESP_NULL; *consumed = line_total; return HL_RESP_OK;
    case ',': { /* double */
        double d; if (parse_double(hdr, hdrlen, &d) != 0) return HL_RESP_PARSE_ERR;
        out->type = HL_RESP_DOUBLE; out->d = d; *consumed = line_total; return HL_RESP_OK;
    }
    case '$':   /* bulk string */
    case '=': { /* verbatim string */
        int64_t n; if (parse_i64(hdr, hdrlen, &n) != 0) return HL_RESP_PARSE_ERR;
        if (n < 0) {                                 /* $-1 = null (RESP2) */
            if (n != -1) return HL_RESP_PARSE_ERR;
            out->type = HL_RESP_NULL; *consumed = line_total; return HL_RESP_OK;
        }
        if ((uint64_t)n > HL_RESP_MAX_BULK) return HL_RESP_PARSE_ERR;
        size_t need = line_total + (size_t)n + 2;    /* body + CRLF */
        if (need < line_total || avail < need) return avail < need ? HL_RESP_NEED_MORE : HL_RESP_PARSE_ERR;
        const uint8_t *body = buf + line_total;
        if (body[n] != '\r' || body[n + 1] != '\n') return HL_RESP_PARSE_ERR;
        out->type = HL_RESP_STR;
        if (prefix == '=' && n >= 4) {               /* strip the "txt:" tag */
            out->str.p = (const char *)body + 4; out->str.len = (size_t)n - 4;
        } else {
            out->str.p = (const char *)body; out->str.len = (size_t)n;
        }
        *consumed = need; return HL_RESP_OK;
    }
    case '!': { /* bulk error (RESP3) */
        int64_t n; if (parse_i64(hdr, hdrlen, &n) != 0 || n < 0) return HL_RESP_PARSE_ERR;
        if ((uint64_t)n > HL_RESP_MAX_BULK) return HL_RESP_PARSE_ERR;
        size_t need = line_total + (size_t)n + 2;
        if (need < line_total || avail < need) return avail < need ? HL_RESP_NEED_MORE : HL_RESP_PARSE_ERR;
        const uint8_t *body = buf + line_total;
        if (body[n] != '\r' || body[n + 1] != '\n') return HL_RESP_PARSE_ERR;
        out->type = HL_RESP_ERR; out->str.p = (const char *)body; out->str.len = (size_t)n;
        *consumed = need; return HL_RESP_OK;
    }
    case '*':   /* array */
    case '~':   /* set */
    case '>':   /* push */
    case '%': { /* map */
        int64_t n; if (parse_i64(hdr, hdrlen, &n) != 0) return HL_RESP_PARSE_ERR;
        if (n < 0) {                                 /* *-1 = null array (RESP2) */
            if (n != -1) return HL_RESP_PARSE_ERR;
            out->type = HL_RESP_NULL; *consumed = line_total; return HL_RESP_OK;
        }
        uint64_t count = (uint64_t)n;
        if (prefix == '%') {                         /* map: 2 elements per pair */
            if (count > HL_RESP_MAX_ELEMS / 2) return HL_RESP_PARSE_ERR;
            count *= 2;
        } else if (count > HL_RESP_MAX_ELEMS) {
            return HL_RESP_PARSE_ERR;
        }
        out->type = (prefix == '%') ? HL_RESP_MAP : HL_RESP_ARRAY;
        out->arr.count = count;
        out->arr.items = NULL;
        if (count == 0) { *consumed = line_total; return HL_RESP_OK; }
        if (!alloc) return HL_RESP_PARSE_ERR;        /* no arena -> reject aggregate */
        HlRespValue *items = alloc(ctx, count * sizeof(HlRespValue));
        if (!items) return HL_RESP_PARSE_ERR;
        size_t off = line_total;
        for (uint64_t k = 0; k < count; k++) {
            size_t used = 0;
            HlRespResult r = parse_one(buf + off, avail - off, &used, &items[k],
                                       alloc, ctx, depth + 1);
            if (r != HL_RESP_OK) return r;           /* NEED_MORE / PARSE_ERR */
            off += used;
        }
        out->arr.items = items;
        *consumed = off; return HL_RESP_OK;
    }
    default:
        return HL_RESP_PARSE_ERR;
    }
}

HlRespResult hl_resp_parse(const uint8_t *buf, size_t avail, size_t *consumed,
                           HlRespValue *out, HlRespAlloc alloc, void *alloc_ctx) {
    *consumed = 0;
    if (!out) return HL_RESP_PARSE_ERR;
    /* A NULL buffer with no bytes is a valid "need more" state (an empty recv
     * buffer before the first read); only reject NULL with claimed bytes. */
    if (!buf && avail > 0) return HL_RESP_PARSE_ERR;
    if (avail == 0) return HL_RESP_NEED_MORE;
    return parse_one(buf, avail, consumed, out, alloc, alloc_ctx, 0);
}

int hl_resp_is_ok(const HlRespValue *v) {
    return v && v->type == HL_RESP_STR && v->str.len == 2 &&
           v->str.p[0] == 'O' && v->str.p[1] == 'K';
}
