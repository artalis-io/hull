/* spandiff_ops.h — shared op dispatcher for the hull_span.h native-vs-WASM
 * differential test (#324 3b). ONE body, compiled two ways: natively (against
 * templates/hull_span.h) by tests/hull/cap/test_span_diff.c, and as a wasm32
 * guest (against the scaffolded hull_span.h) by tests/fixtures/compute/spandiff.c.
 * Feeding the SAME fixture bytes through both and comparing the output proves the
 * SDK decodes identically on both targets, under interpreter and AOT.
 *
 * The includer must have already included hull_span.h (for HullSpan, the rd*
 * readers, hull_span_decode / _find / _narrow). Only the pure, host_call-free
 * ops are exercised, so the guest imports nothing and runs in a bare WAMR harness.
 *
 * Wire protocol — input is [op][payload]; output is op-specific, little-endian:
 *   RD16/RD32/RD64 : [op][off][bytes...]     -> u64 value (8 bytes, zero-extended)
 *   DECODE         : [op][rec_len u16][rec..] -> i32 status (4); on 0 append
 *                    flags u32(4) base u64(8) len u64(8) foffset u64(8) name(64)
 *   NARROW         : [op][p u64]             -> i32 status (4); on 0 append i32 out(4)
 *   FIND           : [op][count u8][count*64 name bytes][query NUL...] -> i32 index(4)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef SPANDIFF_OPS_H
#define SPANDIFF_OPS_H

#define SPANDIFF_OP_RD16   1
#define SPANDIFF_OP_RD32   2
#define SPANDIFF_OP_RD64   3
#define SPANDIFF_OP_DECODE 4
#define SPANDIFF_OP_NARROW 5
#define SPANDIFF_OP_FIND   6
#define SPANDIFF_OP_READ   7   /* bounded typed window read (BE/LE, signed, float) */

/* OP_READ kinds. */
#define SPANDIFF_K_U8  0
#define SPANDIFF_K_I8  1
#define SPANDIFF_K_U16 2
#define SPANDIFF_K_I16 3
#define SPANDIFF_K_U32 4
#define SPANDIFF_K_I32 5
#define SPANDIFF_K_U64 6
#define SPANDIFF_K_I64 7
#define SPANDIFF_K_F32 8
#define SPANDIFF_K_F64 9

#define SPANDIFF_ERR (-100)   /* malformed fixture (distinct from SDK error codes) */

/* Little-endian serializers for the output (independent of the SDK readers, so a
 * decode bug cannot hide behind a matching serialize bug). */
static void spandiff__wr32(hull_span_u8 *o, hull_span_u32 v)
{ for (int i = 0; i < 4; i++) o[i] = (hull_span_u8)(v >> (8 * i)); }
static void spandiff__wr64(hull_span_u8 *o, hull_span_u64 v)
{ for (int i = 0; i < 8; i++) o[i] = (hull_span_u8)(v >> (8 * i)); }

/* Reverse bit-casts (float value -> raw bits) so a float accessor's result is
 * compared by BITS — preserving NaN/Inf/signed-zero, which value-compare would
 * mangle. */
static hull_span_u32 spandiff__f32_bits(float f)
{ union { float f; hull_span_u32 u; } x; x.f = f; return x.u; }
static hull_span_u64 spandiff__f64_bits(double d)
{ union { double d; hull_span_u64 u; } x; x.d = d; return x.u; }

/* Run one fixture. Returns output length (>= 0) or a negative error. */
static int spandiff_run(const hull_span_u8 *in, hull_span_u32 in_len,
                        hull_span_u8 *out, hull_span_u32 out_max)
{
    if (in_len < 1) return SPANDIFF_ERR;
    hull_span_u8 op = in[0];

    switch (op) {
    case SPANDIFF_OP_RD16:
    case SPANDIFF_OP_RD32:
    case SPANDIFF_OP_RD64: {
        if (in_len < 2 || out_max < 8) return SPANDIFF_ERR;
        hull_span_u8 off = in[1];
        hull_span_u32 need = (op == SPANDIFF_OP_RD16) ? 2u : (op == SPANDIFF_OP_RD32) ? 4u : 8u;
        if ((hull_span_u32)off + need > in_len - 2) return SPANDIFF_ERR;  /* fixture must supply the bytes */
        const hull_span_u8 *p = in + 2 + off;
        hull_span_u64 v = (op == SPANDIFF_OP_RD16) ? (hull_span_u64)hull_span__rd16(p)
                        : (op == SPANDIFF_OP_RD32) ? (hull_span_u64)hull_span__rd32(p)
                                                   : hull_span__rd64(p);
        spandiff__wr64(out, v);
        return 8;
    }
    case SPANDIFF_OP_DECODE: {
        if (in_len < 3) return SPANDIFF_ERR;
        hull_span_u32 rec_len = (hull_span_u32)hull_span__rd16(in + 1);
        const hull_span_u8 *rec = in + 3;
        hull_span_u32 avail = in_len - 3;
        /* decode reads the full 96-byte layout only when rec_len >= 96 (else it
         * rejects before touching any field). The fixture must ship those bytes
         * so the decoder never reads past the input buffer. */
        hull_span_u32 touch = (rec_len < HULL_SPAN_META_V1_SIZE) ? 0u : (hull_span_u32)HULL_SPAN_META_V1_SIZE;
        if (avail < touch) return SPANDIFF_ERR;
        HullSpan s;
        int st = hull_span_decode(rec, rec_len, &s);
        if (out_max < 4) return SPANDIFF_ERR;
        spandiff__wr32(out, (hull_span_u32)(hull_span_i32)st);
        if (st != 0) return 4;
        if (out_max < 100) return SPANDIFF_ERR;
        spandiff__wr32(out + 4, s.flags);
        spandiff__wr64(out + 8, s.base);
        spandiff__wr64(out + 16, s.len);
        spandiff__wr64(out + 24, s.foffset);
        for (int i = 0; i < 64; i++) out[32 + i] = (hull_span_u8)s.name[i];
        return 96;
    }
    case SPANDIFF_OP_NARROW: {
        if (in_len < 9 || out_max < 4) return SPANDIFF_ERR;
        hull_span_u64 p = hull_span__rd64(in + 1);
        hull_span_i32 narrowed = 0;
        int st = hull_span__narrow((hull_span_uptr)p, &narrowed);
        spandiff__wr32(out, (hull_span_u32)(hull_span_i32)st);
        if (st != 0) return 4;
        if (out_max < 8) return SPANDIFF_ERR;
        spandiff__wr32(out + 4, (hull_span_u32)narrowed);
        return 8;
    }
    case SPANDIFF_OP_FIND: {
        if (in_len < 2 || out_max < 4) return SPANDIFF_ERR;
        int count = in[1];
        if (count < 0 || count > HULL_SPAN_MAX) return SPANDIFF_ERR;
        hull_span_u32 need = 2u + (hull_span_u32)count * 64u;
        /* need == in_len means the name table consumes the whole input with NO
         * query bytes left; the query pointer would then be one-past-the-end.
         * Reject that exact boundary (a query needs at least its NUL byte). */
        if (need >= in_len) return SPANDIFF_ERR;
        HullSpan spans[HULL_SPAN_MAX];
        for (int i = 0; i < count; i++) {
            const hull_span_u8 *nm = in + 2 + (hull_span_u32)i * 64u;
            for (int j = 0; j < 63; j++) spans[i].name[j] = (char)nm[j];
            spans[i].name[63] = '\0';
        }
        /* The query is the remaining bytes after the name table. hull_span_find
         * scans it as a C string, so it MUST be NUL-terminated within the input;
         * otherwise it would read past `in`. Validate termination first. */
        const char *query = (const char *)(in + need);
        hull_span_u32 qmax = in_len - need;   /* >= 1 by the need >= in_len guard */
        hull_span_u32 qi = 0;
        while (qi < qmax && query[qi] != 0) qi++;
        if (qi == qmax) return SPANDIFF_ERR;  /* no NUL terminator within the input */
        int idx = hull_span_find(spans, count, query);
        spandiff__wr32(out, (hull_span_u32)(hull_span_i32)idx);
        return 4;
    }
    case SPANDIFF_OP_READ: {
        /* [op][kind][endian][off u64][len u64][window bytes...] — a 19-byte
         * header. in_len must cover it fully (18 would over-read off/len by one
         * and underflow `avail = in_len - 19`). */
        if (in_len < 19 || out_max < 12) return SPANDIFF_ERR;
        hull_span_u8  kind   = in[1];
        hull_span_u8  endian = in[2];          /* 0 = LE, 1 = BE (ignored for 8-bit) */
        hull_span_u64 off    = hull_span__rd64(in + 3);
        hull_span_u64 len    = hull_span__rd64(in + 11);
        const hull_span_u8 *w = in + 19;
        hull_span_u32 avail = in_len - 19;
        if (len > avail) return SPANDIFF_ERR;  /* fixture must ship the declared window */

        int st = SPANDIFF_ERR;
        hull_span_u64 val = 0;                 /* integer: (i64/u64)-extended; float: raw bits */

        switch (kind) {
        case SPANDIFF_K_U8:  { hull_span_u8  v; st = hull_span_read_u8 (w, len, off, &v); val = (hull_span_u64)v; break; }
        case SPANDIFF_K_I8:  { hull_span_i8  v; st = hull_span_read_i8 (w, len, off, &v); val = (hull_span_u64)(hull_span_i64)v; break; }
        case SPANDIFF_K_U16: { hull_span_u16 v; st = endian ? hull_span_read_u16be(w, len, off, &v) : hull_span_read_u16le(w, len, off, &v); val = (hull_span_u64)v; break; }
        case SPANDIFF_K_I16: { hull_span_i16 v; st = endian ? hull_span_read_i16be(w, len, off, &v) : hull_span_read_i16le(w, len, off, &v); val = (hull_span_u64)(hull_span_i64)v; break; }
        case SPANDIFF_K_U32: { hull_span_u32 v; st = endian ? hull_span_read_u32be(w, len, off, &v) : hull_span_read_u32le(w, len, off, &v); val = (hull_span_u64)v; break; }
        case SPANDIFF_K_I32: { hull_span_i32 v; st = endian ? hull_span_read_i32be(w, len, off, &v) : hull_span_read_i32le(w, len, off, &v); val = (hull_span_u64)(hull_span_i64)v; break; }
        case SPANDIFF_K_U64: { hull_span_u64 v; st = endian ? hull_span_read_u64be(w, len, off, &v) : hull_span_read_u64le(w, len, off, &v); val = v; break; }
        case SPANDIFF_K_I64: { hull_span_i64 v; st = endian ? hull_span_read_i64be(w, len, off, &v) : hull_span_read_i64le(w, len, off, &v); val = (hull_span_u64)v; break; }
        case SPANDIFF_K_F32: { float  v; st = endian ? hull_span_read_f32be(w, len, off, &v) : hull_span_read_f32le(w, len, off, &v); val = (hull_span_u64)spandiff__f32_bits(v); break; }
        case SPANDIFF_K_F64: { double v; st = endian ? hull_span_read_f64be(w, len, off, &v) : hull_span_read_f64le(w, len, off, &v); val = spandiff__f64_bits(v); break; }
        default: return SPANDIFF_ERR;
        }

        spandiff__wr32(out, (hull_span_u32)(hull_span_i32)st);
        if (st != 0) return 4;
        spandiff__wr64(out + 4, val);
        return 12;
    }
    default:
        return SPANDIFF_ERR;
    }
}

#endif /* SPANDIFF_OPS_H */
