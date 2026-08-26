/*
 * hull_span.h - Hull mapped-spans SDK (guest side)
 *
 * Freestanding, dual-target header for Hull compute plugins that attach
 * host-mapped file windows via `compute.call(..., {spans={...}})` and read them
 * in place, with no per-access host call. It turns the raw `host_call(HL_WASM_OP_SPAN_INFO, ...)`
 * metadata query into typed, overflow-/alignment-safe accessors.
 *
 * Usage in a plugin (include AFTER hull_compute.h, which declares host_call):
 *
 *     #include "hull_compute.h"
 *     #include "hull_span.h"
 *
 *     HullSpan spans[HL_WASM_MAX_SPANS];
 *     int n = hull_span_setup(spans, HL_WASM_MAX_SPANS);   // once, at start
 *     int i = hull_span_find(spans, n, "source");          // by name
 *     if (i >= 0) {
 *         const unsigned char *w = (const unsigned char *)(size_t)spans[i].base;
 *         // read w[0 .. spans[i].len)
 *     }
 *
 * This realizes the checkpoint-3 SDK header `hull/wasm/span.h`
 * (docs/wasm_mapped_spans_checkpoint3.md §1.F) as a flat sibling of the
 * per-module hull_compute.h; refresh with `hull compute refresh-header`.
 *
 * DUAL-TARGET: the header compiles both as a freestanding wasm32 plugin and
 * natively (for the differential + unit tests). It uses the compiler's builtin
 * fixed-width type macros (no <stdint.h>, and no clash with hull_compute.h's own
 * `int32_t` typedefs) and decodes the wire record BY BYTE OFFSET (never by
 * casting linear memory to a struct - alignment/aliasing UB, and the host writes
 * the same 64-bit-field layout regardless of guest pointer width).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_SPAN_H
#define HULL_SPAN_H

/* ── Fixed-width types (builtin macros: freestanding + hosted, no <stdint.h>) ── */
typedef __UINT8_TYPE__   hull_span_u8;
typedef __UINT16_TYPE__  hull_span_u16;
typedef __UINT32_TYPE__  hull_span_u32;
typedef __UINT64_TYPE__  hull_span_u64;
typedef __INT8_TYPE__    hull_span_i8;
typedef __INT16_TYPE__   hull_span_i16;
typedef __INT32_TYPE__   hull_span_i32;
typedef __INT64_TYPE__   hull_span_i64;
typedef __UINTPTR_TYPE__ hull_span_uptr;

/* ── Host-call opcode + wire ABI (guest copy of include/hull/cap/wasm.h) ──────
 * HlSpanMetaV1: 96 bytes, little-endian, packed. Decode BY OFFSET. */
#define HULL_OP_SPAN_INFO        0x04

#define HULL_SPAN_META_V1_SIZE   96
#define HULL_SPAN_OFF_VERSION     0   /* u16, = 1                                */
#define HULL_SPAN_OFF_STRUCTSZ    2   /* u16, = 96 (also the caller's advertised cap) */
#define HULL_SPAN_OFF_FLAGS       4   /* u32, bit0 = read-only                   */
#define HULL_SPAN_OFF_NAME        8   /* char[64], NUL-terminated                */
#define HULL_SPAN_OFF_BASE       72   /* u64, guest WASM address of window[0]     */
#define HULL_SPAN_OFF_LEN        80   /* u64, window length in bytes              */
#define HULL_SPAN_OFF_FOFFSET    88   /* u64, 64-bit logical file offset of window[0] */
#define HULL_SPAN_FLAG_RO       0x1

/* Max spans per invocation (mirrors HL_WASM_MAX_SPANS host-side). */
#ifndef HULL_SPAN_MAX
#define HULL_SPAN_MAX 16
#endif

/* ── Error codes (negative; distinct from a valid count >= 0) ────────────────── */
#define HULL_SPAN_ERR_QUERY   (-1)  /* host_call reported an error (-1) or 0 for an in-range index */
#define HULL_SPAN_ERR_VERSION (-2)  /* version/struct_size mismatch, or the host record is short   */
#define HULL_SPAN_ERR_ADDR    (-3)  /* scratch record address >= 4 GiB: unrepresentable in the      */
                                    /* (i32,i32,i32) host_call ABI (Memory64 / 64-bit native).      */
#define HULL_SPAN_ERR_RANGE   (-4)  /* a bounded read would fall outside [0, len) (one-past /       */
                                    /* width-straddling / an offset that would overflow).           */
#define HULL_SPAN_ERR_ARG     (-5)  /* invalid argument: out_cap < 0, or out_cap > 0 with out ==    */
                                    /* NULL. (out == NULL with out_cap == 0 is a valid count query.) */

/* ── Public span descriptor (decoded, host-independent) ──────────────────────── */
typedef struct {
    hull_span_u64 base;      /* guest address of window[0]; on wasm32 use the low 32 bits */
    hull_span_u64 len;       /* window length in bytes                                    */
    hull_span_u64 foffset;   /* 64-bit logical file offset of window[0]                   */
    hull_span_u32 flags;     /* bit0 = read-only                                          */
    char          name[64];  /* NUL-terminated                                            */
} HullSpan;

/* host_call is declared by hull_compute.h in a plugin; a native test provides its
 * own. Override the symbol via HULL_SPAN_HOST_CALL for testing/embedding. */
#ifndef HULL_SPAN_HOST_CALL
#define HULL_SPAN_HOST_CALL host_call
#endif

/* ── Little-endian, alignment-safe byte readers ──────────────────────────────── */
static inline hull_span_u16 hull_span__rd16(const hull_span_u8 *p)
{ return (hull_span_u16)((hull_span_u16)p[0] | ((hull_span_u16)p[1] << 8)); }

static inline hull_span_u32 hull_span__rd32(const hull_span_u8 *p)
{ return (hull_span_u32)p[0] | ((hull_span_u32)p[1] << 8)
       | ((hull_span_u32)p[2] << 16) | ((hull_span_u32)p[3] << 24); }

static inline hull_span_u64 hull_span__rd64(const hull_span_u8 *p)
{ hull_span_u64 v = 0; for (int i = 0; i < 8; i++) v |= (hull_span_u64)p[i] << (8 * i); return v; }

/* ── Big-endian byte readers (window contents may be BE: PNG, network formats). ── */
static inline hull_span_u16 hull_span__rd16be(const hull_span_u8 *p)
{ return (hull_span_u16)(((hull_span_u16)p[0] << 8) | (hull_span_u16)p[1]); }

static inline hull_span_u32 hull_span__rd32be(const hull_span_u8 *p)
{ return ((hull_span_u32)p[0] << 24) | ((hull_span_u32)p[1] << 16)
       | ((hull_span_u32)p[2] << 8) | (hull_span_u32)p[3]; }

static inline hull_span_u64 hull_span__rd64be(const hull_span_u8 *p)
{ hull_span_u64 v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | (hull_span_u64)p[i]; return v; }

/* ── Bit-cast raw bits to a float, preserving special-value patterns (NaN/Inf/
 * signed zero) exactly - no arithmetic. Union punning is well-defined in C. ──── */
static inline float hull_span__bits_f32(hull_span_u32 b)
{ union { hull_span_u32 u; float f; } x; x.u = b; return x.f; }
static inline double hull_span__bits_f64(hull_span_u64 b)
{ union { hull_span_u64 u; double d; } x; x.u = b; return x.d; }

/* ── Overflow-safe range check: is [off, off+width) fully within [0, len)?
 * `len - off` is evaluated only after `off <= len`, so an off near UINT64_MAX is
 * rejected, never wrapped. width is 1..8. ───────────────────────────────────── */
static inline int hull_span__fits(hull_span_u64 off, hull_span_u64 width, hull_span_u64 len)
{ return off <= len && width <= (len - off); }

/* ── Bounded, typed window reads. `w` is the window base, `len` its length; each
 * reads `width` bytes at `off`, returning 0 + setting *out on success or
 * HULL_SPAN_ERR_RANGE (leaving *out untouched) on a one-past / width-straddling /
 * overflowing offset. LE + BE for each width > 1 byte; signed variants
 * reinterpret the same bytes; float variants bit-cast. A plugin passes a window
 * as `(const void *)(hull_span_uptr)span.base, span.len`. ────────────────────── */
static inline int hull_span_read_u8(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_u8 *out)
{ const hull_span_u8 *b = (const hull_span_u8 *)w; if (!hull_span__fits(off, 1, len)) return HULL_SPAN_ERR_RANGE; *out = b[off]; return 0; }
static inline int hull_span_read_i8(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_i8 *out)
{ hull_span_u8 v; int r = hull_span_read_u8(w, len, off, &v); if (r) return r; *out = (hull_span_i8)v; return 0; }

static inline int hull_span_read_u16le(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_u16 *out)
{ const hull_span_u8 *b = (const hull_span_u8 *)w; if (!hull_span__fits(off, 2, len)) return HULL_SPAN_ERR_RANGE; *out = hull_span__rd16(b + off); return 0; }
static inline int hull_span_read_u16be(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_u16 *out)
{ const hull_span_u8 *b = (const hull_span_u8 *)w; if (!hull_span__fits(off, 2, len)) return HULL_SPAN_ERR_RANGE; *out = hull_span__rd16be(b + off); return 0; }
static inline int hull_span_read_i16le(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_i16 *out)
{ hull_span_u16 v; int r = hull_span_read_u16le(w, len, off, &v); if (r) return r; *out = (hull_span_i16)v; return 0; }
static inline int hull_span_read_i16be(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_i16 *out)
{ hull_span_u16 v; int r = hull_span_read_u16be(w, len, off, &v); if (r) return r; *out = (hull_span_i16)v; return 0; }

static inline int hull_span_read_u32le(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_u32 *out)
{ const hull_span_u8 *b = (const hull_span_u8 *)w; if (!hull_span__fits(off, 4, len)) return HULL_SPAN_ERR_RANGE; *out = hull_span__rd32(b + off); return 0; }
static inline int hull_span_read_u32be(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_u32 *out)
{ const hull_span_u8 *b = (const hull_span_u8 *)w; if (!hull_span__fits(off, 4, len)) return HULL_SPAN_ERR_RANGE; *out = hull_span__rd32be(b + off); return 0; }
static inline int hull_span_read_i32le(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_i32 *out)
{ hull_span_u32 v; int r = hull_span_read_u32le(w, len, off, &v); if (r) return r; *out = (hull_span_i32)v; return 0; }
static inline int hull_span_read_i32be(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_i32 *out)
{ hull_span_u32 v; int r = hull_span_read_u32be(w, len, off, &v); if (r) return r; *out = (hull_span_i32)v; return 0; }

static inline int hull_span_read_u64le(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_u64 *out)
{ const hull_span_u8 *b = (const hull_span_u8 *)w; if (!hull_span__fits(off, 8, len)) return HULL_SPAN_ERR_RANGE; *out = hull_span__rd64(b + off); return 0; }
static inline int hull_span_read_u64be(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_u64 *out)
{ const hull_span_u8 *b = (const hull_span_u8 *)w; if (!hull_span__fits(off, 8, len)) return HULL_SPAN_ERR_RANGE; *out = hull_span__rd64be(b + off); return 0; }
static inline int hull_span_read_i64le(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_i64 *out)
{ hull_span_u64 v; int r = hull_span_read_u64le(w, len, off, &v); if (r) return r; *out = (hull_span_i64)v; return 0; }
static inline int hull_span_read_i64be(const void *w, hull_span_u64 len, hull_span_u64 off, hull_span_i64 *out)
{ hull_span_u64 v; int r = hull_span_read_u64be(w, len, off, &v); if (r) return r; *out = (hull_span_i64)v; return 0; }

static inline int hull_span_read_f32le(const void *w, hull_span_u64 len, hull_span_u64 off, float *out)
{ hull_span_u32 v; int r = hull_span_read_u32le(w, len, off, &v); if (r) return r; *out = hull_span__bits_f32(v); return 0; }
static inline int hull_span_read_f32be(const void *w, hull_span_u64 len, hull_span_u64 off, float *out)
{ hull_span_u32 v; int r = hull_span_read_u32be(w, len, off, &v); if (r) return r; *out = hull_span__bits_f32(v); return 0; }
static inline int hull_span_read_f64le(const void *w, hull_span_u64 len, hull_span_u64 off, double *out)
{ hull_span_u64 v; int r = hull_span_read_u64le(w, len, off, &v); if (r) return r; *out = hull_span__bits_f64(v); return 0; }
static inline int hull_span_read_f64be(const void *w, hull_span_u64 len, hull_span_u64 off, double *out)
{ hull_span_u64 v; int r = hull_span_read_u64be(w, len, off, &v); if (r) return r; *out = hull_span__bits_f64(v); return 0; }

/* ── Decode a raw HlSpanMetaV1 record.
 * `rec_len` is the number of valid bytes the host wrote (min(cap, struct_size)).
 * Returns 0 on success, or HULL_SPAN_ERR_VERSION if the record does not cover the
 * full v1 layout or the version/struct_size are not v1-compatible. The name is
 * always NUL-terminated in `out` regardless of the wire bytes. ─────────────── */
static inline int hull_span_decode(const void *rec, hull_span_u32 rec_len, HullSpan *out)
{
    const hull_span_u8 *b = (const hull_span_u8 *)rec;
    if (rec_len < HULL_SPAN_META_V1_SIZE)          /* must cover every v1 field */
        return HULL_SPAN_ERR_VERSION;
    hull_span_u16 version     = hull_span__rd16(b + HULL_SPAN_OFF_VERSION);
    hull_span_u16 struct_size = hull_span__rd16(b + HULL_SPAN_OFF_STRUCTSZ);
    if (version != 1 || struct_size < HULL_SPAN_META_V1_SIZE)
        return HULL_SPAN_ERR_VERSION;
    out->flags   = hull_span__rd32(b + HULL_SPAN_OFF_FLAGS);
    for (int i = 0; i < 63; i++)                   /* copy 63, force-terminate at 63 */
        out->name[i] = (char)b[HULL_SPAN_OFF_NAME + i];
    out->name[63] = '\0';
    out->base    = hull_span__rd64(b + HULL_SPAN_OFF_BASE);
    out->len     = hull_span__rd64(b + HULL_SPAN_OFF_LEN);
    out->foffset = hull_span__rd64(b + HULL_SPAN_OFF_FOFFSET);
    return 0;
}

/* ── Narrow a scratch pointer to the 32-bit host_call ABI, rejecting >= 4 GiB.
 * On wasm32 the address is always < 4 GiB, so this never rejects; on Memory64
 * (or a 64-bit native differential build) a scratch above UINT32_MAX cannot be
 * represented in the (i32,i32,i32) ABI and MUST be rejected BEFORE narrowing
 * (a silent truncation would hand the host a bogus, in-range-looking offset).
 * Returns 0 + sets *out on success, HULL_SPAN_ERR_ADDR otherwise. ──────────── */
static inline int hull_span__narrow(hull_span_uptr p, hull_span_i32 *out)
{
    if (p > (hull_span_uptr)0xFFFFFFFFu)
        return HULL_SPAN_ERR_ADDR;
    *out = (hull_span_i32)(hull_span_u32)p;
    return 0;
}

/* ── Discover every attached span in one pass.
 * Fills out[0 .. min(count, out_cap)) and returns the TRUE span count (>= 0), or
 * a negative HULL_SPAN_ERR_* on an argument / query / version / address failure.
 * A return value > out_cap means the caller's array was too small: the first
 * out_cap entries are valid, and the caller under-sized it.
 *
 * Preconditions (validated before any host call): out_cap must be >= 0, and a
 * positive out_cap requires out != NULL. `hull_span_setup(NULL, 0)` is a valid
 * count-only query: it issues ONLY the count query and returns the count on
 * every target (writing nothing, allocating no scratch). Otherwise returns
 * HULL_SPAN_ERR_ARG.
 *
 * The count query comes first (it needs no scratch); a metadata scratch record
 * is allocated and narrowed to the i32 ABI only when records are actually
 * fetched (out_cap > 0 && count > 0). HULL_SPAN_ERR_ADDR (a >= 4 GiB scratch on
 * Memory64 / 64-bit native) can therefore only occur on a record-fetching call,
 * never on the count-only query. Each record uses the cbSize handshake (advertise
 * our capacity in struct_size, validate the returned size covers v1). No host
 * calls happen after setup - every later access is a pure inline read. */
static inline int hull_span_setup(HullSpan *out, int out_cap)
{
    /* 1. Argument preconditions (checked before any host call): a negative
     * capacity is invalid, and a positive capacity requires a non-NULL dest.
     * out == NULL with out_cap == 0 is the valid count-only query. */
    if (out_cap < 0)
        return HULL_SPAN_ERR_ARG;
    if (out_cap > 0 && !out)
        return HULL_SPAN_ERR_ARG;

    /* 2. Count query. It passes ptr = 0, so it needs no metadata scratch and
     * therefore works on every target (including a 64-bit native / Memory64
     * context where a stack address is >= 4 GiB). */
    hull_span_i32 count = HULL_SPAN_HOST_CALL(HULL_OP_SPAN_INFO, 0, -1);
    if (count < 0)
        return HULL_SPAN_ERR_QUERY;

    /* 3. Nothing to fill (a count-only query, out_cap == 0, or count == 0):
     * return the count immediately without touching the scratch record. */
    int n = ((int)count < out_cap) ? (int)count : out_cap;
    if (n <= 0)
        return (int)count;

    /* 4. Only now, with records to fetch, allocate + narrow the scratch record
     * whose address must fit the (i32,i32,i32) host_call ABI. On Memory64 / a
     * 64-bit native scratch above UINT32_MAX this rejects with ERR_ADDR -- but
     * only when records are actually requested, never for the count query. */
    hull_span_u8   rec[HULL_SPAN_META_V1_SIZE];
    hull_span_i32  rec_ptr;
    if (hull_span__narrow((hull_span_uptr)(void *)rec, &rec_ptr) != 0)
        return HULL_SPAN_ERR_ADDR;

    for (int i = 0; i < n; i++) {
        /* cbSize: advertise our record capacity in the dest's struct_size field. */
        rec[HULL_SPAN_OFF_STRUCTSZ]     = (hull_span_u8)(HULL_SPAN_META_V1_SIZE & 0xff);
        rec[HULL_SPAN_OFF_STRUCTSZ + 1] = (hull_span_u8)((HULL_SPAN_META_V1_SIZE >> 8) & 0xff);

        hull_span_i32 r = HULL_SPAN_HOST_CALL(HULL_OP_SPAN_INFO, rec_ptr, (hull_span_i32)i);
        if (r <= 0)                                /* <0 = error, 0 = out-of-range (unexpected for i<count) */
            return HULL_SPAN_ERR_QUERY;
        if ((hull_span_u32)r < HULL_SPAN_META_V1_SIZE)   /* host record shorter than v1 */
            return HULL_SPAN_ERR_VERSION;
        if (hull_span_decode(rec, HULL_SPAN_META_V1_SIZE, &out[i]) != 0)
            return HULL_SPAN_ERR_VERSION;
    }
    return (int)count;
}

/* ── Resolve a span by name over the cached records: index, or -1 if unknown.
 * Linear scan (<= 16 records, read once at setup); no host calls. ──────────── */
static inline int hull_span_find(const HullSpan *spans, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        const char *a = spans[i].name;
        const char *b = name;
        while (*a != '\0' && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            return i;
    }
    return -1;
}

#endif /* HULL_SPAN_H */
