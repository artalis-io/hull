/* spanlist.c - enumerate every attached mapped span in DECLARATION ORDER and
 * resolve a caller-supplied name, using ONLY the public hull_span.h SDK
 * (hull_span_setup + hull_span_find). Output (text):
 *   "count=<N>;order=<n0>,<n1>,...;find=<idx>"   idx = hull_span_find(name), -1 unknown
 * or "err setup=<code>" on a setup failure.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"
#include "hull_span.h"

static int put_s(char *o, int pos, int max, const char *s)
{ while (*s && pos < max) o[pos++] = *s++; return pos; }

static int put_i(char *o, int pos, int max, int v)
{
    char t[12];
    int n = 0, neg = v < 0;
    unsigned uv = neg ? (unsigned)(-(long)v) : (unsigned)v;
    if (uv == 0) t[n++] = '0';
    while (uv) { t[n++] = (char)('0' + uv % 10); uv /= 10; }
    if (neg && pos < max) o[pos++] = '-';
    while (n > 0 && pos < max) o[pos++] = t[--n];
    return pos;
}

HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    char *o = (char *)out;
    HullSpan spans[HULL_SPAN_MAX];
    int count = hull_span_setup(spans, HULL_SPAN_MAX);
    if (count < 0) {
        int p = put_s(o, 0, out_max, "err setup=");
        return put_i(o, p, out_max, count);
    }

    /* NUL-terminate the query name from input. */
    char q[64];
    int qn = (in_len < 63) ? in_len : 63;
    if (qn < 0) qn = 0;
    for (int i = 0; i < qn; i++) q[i] = ((const char *)in)[i];
    q[qn] = '\0';

    /* Scan only the entries setup actually populated: filled = min(count, cap).
     * hull_span_setup was called with cap = HULL_SPAN_MAX, so filled is bounded
     * by the stack array size regardless of the host-reported true count. */
    int filled = count < HULL_SPAN_MAX ? count : HULL_SPAN_MAX;

    int p = 0;
    p = put_s(o, p, out_max, "count=");
    p = put_i(o, p, out_max, count);
    p = put_s(o, p, out_max, ";order=");
    for (int i = 0; i < filled; i++) {
        if (i) p = put_s(o, p, out_max, ",");
        p = put_s(o, p, out_max, spans[i].name);
    }
    p = put_s(o, p, out_max, ";find=");
    p = put_i(o, p, out_max, hull_span_find(spans, filled, q));
    return p;
}
