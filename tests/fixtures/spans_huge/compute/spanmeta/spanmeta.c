/* spanmeta.c - probe hull_span_setup() capacity semantics + a 64-bit foffset +
 * bounded reads on the real window, using ONLY the public hull_span.h SDK.
 * Input byte 0 = requested out_cap for hull_span_setup. Output (text):
 *   "ret=<true-count>;filled=<min(ret,cap)>;names=<n0,...>;foff0=<u64>;
 *    first=<byte>;end=<byte>;onepast=<code>;straddle=<code>"
 * so a caller can assert: setup returns the TRUE count even when out_cap is
 * smaller (insufficient), the 64-bit foffset survives an offset > 4 GiB, and the
 * bounded read accessors accept first/exact-end and reject one-past/straddle.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"
#include "hull_span.h"

static int put_s(char *o, int p, int max, const char *s)
{ while (*s && p < max) o[p++] = *s++; return p; }
static int put_i(char *o, int p, int max, int v)
{
    char t[12]; int n = 0, neg = v < 0;
    unsigned uv = neg ? (unsigned)(-(long)v) : (unsigned)v;
    if (uv == 0) t[n++] = '0';
    while (uv) { t[n++] = (char)('0' + uv % 10); uv /= 10; }
    if (neg && p < max) o[p++] = '-';
    while (n > 0 && p < max) o[p++] = t[--n];
    return p;
}
static int put_u64(char *o, int p, int max, unsigned long long v)
{
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && p < max) o[p++] = t[--n];
    return p;
}

HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    char *o = (char *)out;
    int raw = (in_len >= 1) ? ((const unsigned char *)in)[0] : 0;

    /* Sentinel 17 (just above HULL_SPAN_MAX; ASCII-safe so it survives JS string
     * encoding): probe hull_span_setup() argument validation from inside a real
     * guest (out_cap>0 + NULL, negative out_cap, and the NULL+0 count-only
     * query). Output "argnull=<>;argneg=<>;argzero=<>". */
    if (raw == 17) {
        HullSpan sp[HULL_SPAN_MAX];
        int r_null = hull_span_setup((HullSpan *)0, 5);
        int r_neg  = hull_span_setup(sp, -1);
        int r_zero = hull_span_setup((HullSpan *)0, 0);
        int q = 0;
        q = put_s(o, q, out_max, "argnull=");  q = put_i(o, q, out_max, r_null);
        q = put_s(o, q, out_max, ";argneg=");  q = put_i(o, q, out_max, r_neg);
        q = put_s(o, q, out_max, ";argzero="); q = put_i(o, q, out_max, r_zero);
        return q;
    }

    int cap = raw;
    if (cap <= 0 || cap > HULL_SPAN_MAX) cap = HULL_SPAN_MAX;

    HullSpan spans[HULL_SPAN_MAX];
    int ret = hull_span_setup(spans, cap);

    int p = 0;
    p = put_s(o, p, out_max, "ret=");
    p = put_i(o, p, out_max, ret);
    if (ret < 0) return p;

    int filled = ret < cap ? ret : cap;

    /* Optional query name after the cap byte: resolve it over the FILLED entries
     * only (hull_span_find(spans, filled, ...)), so a name that exists as a real
     * span BEYOND out_cap is NOT found -- the insufficient-capacity lookup case.
     * Output: "ret=<true>;filled=<f>;find=<idx>". */
    int qn = (in_len > 1) ? (in_len - 1) : 0;
    if (qn > 63) qn = 63;
    if (qn > 0) {
        char q[64];
        for (int i = 0; i < qn; i++) q[i] = ((const char *)in)[1 + i];
        q[qn] = '\0';
        p = put_s(o, p, out_max, ";filled=");
        p = put_i(o, p, out_max, filled);
        p = put_s(o, p, out_max, ";find=");
        p = put_i(o, p, out_max, hull_span_find(spans, filled, q));
        return p;
    }

    p = put_s(o, p, out_max, ";filled=");
    p = put_i(o, p, out_max, filled);
    p = put_s(o, p, out_max, ";names=");
    for (int i = 0; i < filled; i++) {
        if (i) p = put_s(o, p, out_max, ",");
        p = put_s(o, p, out_max, spans[i].name);
    }
    if (filled > 0) {
        /* every filled span's DISTINCT 64-bit foffset, in declaration order. */
        p = put_s(o, p, out_max, ";foffs=");
        for (int i = 0; i < filled; i++) {
            if (i) p = put_s(o, p, out_max, ",");
            p = put_u64(o, p, out_max, (unsigned long long)spans[i].foffset);
        }
        /* bounded reads on span[0]'s window: first / exact-end / one-past / straddle. */
        const void *w = (const void *)(hull_span_uptr)spans[0].base;
        hull_span_u64 len = spans[0].len;
        hull_span_u8 fb = 0, eb = 0, d8 = 0;
        hull_span_u32 d32 = 0;
        int r_first    = hull_span_read_u8(w, len, 0, &fb);
        int r_end      = (len > 0) ? hull_span_read_u8(w, len, len - 1, &eb) : HULL_SPAN_ERR_RANGE;
        int r_onepast  = hull_span_read_u8(w, len, len, &d8);
        int r_straddle = hull_span_read_u32le(w, len, (len > 0) ? len - 1 : 0, &d32);
        (void)d8; (void)d32;
        p = put_s(o, p, out_max, ";first=");    p = put_i(o, p, out_max, r_first == 0 ? (int)fb : r_first);
        p = put_s(o, p, out_max, ";end=");      p = put_i(o, p, out_max, r_end == 0 ? (int)eb : r_end);
        p = put_s(o, p, out_max, ";onepast=");  p = put_i(o, p, out_max, r_onepast);
        p = put_s(o, p, out_max, ";straddle="); p = put_i(o, p, out_max, r_straddle);
    }
    return p;
}
