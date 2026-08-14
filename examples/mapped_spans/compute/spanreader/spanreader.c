/*
 * spanreader.c — reference Hull compute plugin for mapped spans.
 *
 * Reads a host-mapped, read-only file window that the caller attached via
 * compute.call(name, input, { spans = { { name = "...", buffer = mapped } } }),
 * using ONLY the public hull_span.h API — no hand-written host_call and no
 * wire-offset decoding. It discovers the attached spans with hull_span_setup(),
 * resolves a caller-supplied NAME with hull_span_find(), performs a BOUNDED read
 * of the window, and returns a deterministic text line. Unknown names and
 * out-of-range offsets are handled explicitly, without undefined behavior.
 *
 * Input:
 *   <name>                        -> resolve <name>, sample window[0]
 *   <name> "\0" <u32le offset>    -> resolve <name>, sample window[offset]
 *                                    (offset >= len is reported, never read)
 * Output (ASCII), one of:
 *   ok name=<name> len=<len> foff=<foffset> off=<off> val=<byte>
 *   range name=<name> len=<len> off=<off>          (out of range; no read)
 *   unknown name=<name> count=<n>                  (name not attached)
 *   err setup=<code>                               (hull_span_setup failed)
 *
 * Build: hull compute build spanreader
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull_compute.h"
#include "hull_span.h"

HULL_VERSION_EXPORT

/* Append a C string; returns the new write position (bounded by out_max). */
static int32_t put_str(unsigned char *o, int32_t p, int32_t max, const char *s)
{
    while (*s && p < max) o[p++] = (unsigned char)*s++;
    return p;
}

/* Append a u64 in decimal; returns the new write position. */
static int32_t put_u64(unsigned char *o, int32_t p, int32_t max, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0 && p < max) o[p++] = (unsigned char)tmp[--n];
    return p;
}

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    const unsigned char *in  = (const unsigned char *)in_ptr;
    unsigned char       *out = (unsigned char *)out_ptr;
    if (in_len < 0) in_len = 0;

    /* Parse: the name runs up to the first NUL (or the whole input); an optional
     * 4-byte little-endian offset follows the NUL separator. */
    int32_t name_len = 0;
    while (name_len < in_len && in[name_len] != 0) name_len++;

    char name[64];
    int32_t nl = name_len < 63 ? name_len : 63;
    int32_t ni = 0;
    while (ni < nl) { name[ni] = (char)in[ni]; ni++; }   /* loop, not a memcpy libcall */
    name[ni] = '\0';

    int      have_off = 0;
    uint32_t off = 0;
    if (name_len < in_len && (in_len - name_len - 1) >= 4) {
        const unsigned char *q = in + name_len + 1;
        off = (uint32_t)q[0] | ((uint32_t)q[1] << 8)
            | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        have_off = 1;
    }

    /* Discover the attached spans once via the SDK (no host_call by hand). */
    HullSpan spans[HULL_SPAN_MAX];
    int count = hull_span_setup(spans, HULL_SPAN_MAX);
    int32_t p = 0;
    if (count < 0) {
        p = put_str(out, p, out_max, "err setup=");
        p = put_u64(out, p, out_max, (uint64_t)(-(int64_t)count));
        return p;
    }

    int idx = hull_span_find(spans, count, name);
    if (idx < 0) {
        p = put_str(out, p, out_max, "unknown name=");
        p = put_str(out, p, out_max, name);
        p = put_str(out, p, out_max, " count=");
        p = put_u64(out, p, out_max, (uint64_t)count);
        return p;
    }

    HullSpan *s    = &spans[idx];   /* pointer, not a struct copy (no memcpy libcall) */
    uint32_t  roff = have_off ? off : 0;

    /* Out-of-range: a requested offset at/beyond the window length is reported,
     * never read (no OOB access). */
    if ((uint64_t)roff >= s->len) {
        p = put_str(out, p, out_max, "range name=");
        p = put_str(out, p, out_max, name);
        p = put_str(out, p, out_max, " len=");
        p = put_u64(out, p, out_max, s->len);
        p = put_str(out, p, out_max, " off=");
        p = put_u64(out, p, out_max, (uint64_t)roff);
        return p;
    }

    /* Bounded read: roff < s->len. On wasm32 the window base is the low 32 bits. */
    const unsigned char *w   = (const unsigned char *)(size_t)s->base;
    unsigned             val = w[roff];

    p = put_str(out, p, out_max, "ok name=");
    p = put_str(out, p, out_max, name);
    p = put_str(out, p, out_max, " len=");
    p = put_u64(out, p, out_max, s->len);
    p = put_str(out, p, out_max, " foff=");
    p = put_u64(out, p, out_max, s->foffset);
    p = put_str(out, p, out_max, " off=");
    p = put_u64(out, p, out_max, (uint64_t)roff);
    p = put_str(out, p, out_max, " val=");
    p = put_u64(out, p, out_max, (uint64_t)val);
    return p;
}
