/*
 * hull_span.h — Hull mapped-spans SDK (guest side)
 *
 * Freestanding, dual-target header for Hull compute plugins that attach
 * host-mapped file windows via `compute.call(..., {spans={...}})` and read them
 * at native speed. It turns the raw `host_call(HL_WASM_OP_SPAN_INFO, ...)`
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
 * casting linear memory to a struct — alignment/aliasing UB, and the host writes
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
typedef __INT32_TYPE__   hull_span_i32;
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
 * a negative HULL_SPAN_ERR_* on a query / version / address failure. A return
 * value > out_cap means the caller's array was too small: the first out_cap
 * entries are valid, and the caller under-sized it. Issues one count query then
 * one record query per index (the cbSize handshake: advertise our capacity in
 * struct_size, validate the returned size covers v1). No host calls happen after
 * setup — every later access is a pure inline read. ─────────────────────────── */
static inline int hull_span_setup(HullSpan *out, int out_cap)
{
    hull_span_u8   rec[HULL_SPAN_META_V1_SIZE];
    hull_span_i32  rec_ptr;
    if (hull_span__narrow((hull_span_uptr)(void *)rec, &rec_ptr) != 0)
        return HULL_SPAN_ERR_ADDR;                 /* scratch unreachable via the i32 ABI */

    hull_span_i32 count = HULL_SPAN_HOST_CALL(HULL_OP_SPAN_INFO, 0, -1);
    if (count < 0)
        return HULL_SPAN_ERR_QUERY;

    int n = ((int)count < out_cap) ? (int)count : out_cap;
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
