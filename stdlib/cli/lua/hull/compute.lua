--
-- hull.compute — WASM module developer tooling
--
-- Usage:
--   hull compute new <name>            Create a new WASM compute module
--   hull compute build [name]          Compile module(s) to .wasm
--   hull compute test <name>           Run test fixtures against a module
--   hull compute check <name>          Validate a .wasm module's exports
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json     = require("hull.json")
local cbuild   = require("hull.compute_build")

-- ── Embedded hull_compute.h ────────────────────────────────────────────

local HULL_COMPUTE_H = [[/*
 * hull_compute.h — Hull WASM compute module ABI header
 *
 * Freestanding header for Hull compute plugins. Provides:
 *   - Type definitions (no stdlib dependency)
 *   - Host call interface (logging, data segments)
 *   - Stream chunk metadata (hull_stream_is_first/is_last/chunk_index)
 *   - hull_* libc helpers (hull_memcpy/memset/memcmp/strlen)
 *   - bare memcpy/memset/memmove so compiler-emitted calls (struct
 *     copies, block init, runtime-length loops) resolve, not trap
 *   - 64KB bump allocator
 *   - Error codes and export macros
 *
 * Include this in your .c module and implement hull_process().
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_COMPUTE_H
#define HULL_COMPUTE_H

/* ── ABI version ──────────────────────────────────────────────────── */

#define HULL_ABI_VERSION 1

/* ── Freestanding type definitions ────────────────────────────────── */

typedef signed int       int32_t;
typedef unsigned int     uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned int     size_t;
typedef unsigned char    uint8_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── Export macros ─────────────────────────────────────────────────── */

#define HULL_EXPORT __attribute__((visibility("default")))
#define HULL_VERSION_EXPORT \
    HULL_EXPORT int32_t hull_version(void) { return HULL_ABI_VERSION; }

/* ── Error codes ──────────────────────────────────────────────────── */

#define HULL_OK          0
#define HULL_ERR_OUTPUT  (-2)   /* output buffer too small */
#define HULL_ERR_INPUT   (-3)   /* invalid input */
#define HULL_ERR_INTERNAL (-4)  /* internal error */

/* ── Host call import ─────────────────────────────────────────────── */

__attribute__((import_module("env"), import_name("host_call")))
extern int32_t host_call(int32_t opcode, int32_t ptr, int32_t len);

/* Host call opcodes */
#define HULL_OP_LOG       0x01
#define HULL_OP_DATA_INFO 0x02
#define HULL_OP_CALLBACK  0x10

/* ── Logging ──────────────────────────────────────────────────────── */

static inline void hull_log(const char *msg, int32_t len)
{
    host_call(HULL_OP_LOG, (int32_t)(size_t)msg, len);
}

/* ── Data segment access ──────────────────────────────────────────── */

/* Number of loaded data segments */
static inline int32_t hull_segment_count(void)
{
    return host_call(HULL_OP_DATA_INFO, -1, 0);
}

/* Get WASM address of segment (0 if not loaded) */
static inline void *hull_segment_addr(int32_t seg_id)
{
    return (void *)(size_t)host_call(HULL_OP_DATA_INFO, seg_id, 0);
}

/* Get size of segment */
static inline int32_t hull_segment_size(int32_t seg_id)
{
    return host_call(HULL_OP_DATA_INFO, seg_id, 1);
}

/* ── Stream chunk info ────────────────────────────────────────────── */

/* When a module is driven by compute.stream, the host exposes per-chunk
 * metadata via host_call(HULL_OP_STREAM, 0, selector). Ordinary (non-stream)
 * calls report flags 0 and chunk index 0. Constants mirror the host
 * (include/hull/cap/wasm.h: HL_WASM_OP_STREAM / HL_WASM_STREAM_*). */
#define HULL_OP_STREAM          0x03
#define HULL_STREAM_FLAGS       0
#define HULL_STREAM_CHUNK_INDEX 1
#define HULL_STREAM_FLAG_FIRST  0x02
#define HULL_STREAM_FLAG_LAST   0x01

static inline int hull_stream_flags(void)
{
    return host_call(HULL_OP_STREAM, 0, HULL_STREAM_FLAGS);
}

static inline int hull_stream_is_first(void)
{
    return (hull_stream_flags() & HULL_STREAM_FLAG_FIRST) != 0;
}

static inline int hull_stream_is_last(void)
{
    return (hull_stream_flags() & HULL_STREAM_FLAG_LAST) != 0;
}

static inline int hull_stream_chunk_index(void)
{
    return host_call(HULL_OP_STREAM, 0, HULL_STREAM_CHUNK_INDEX);
}

/* ── Minimal libc ─────────────────────────────────────────────────── */

static inline void *hull_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void *hull_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static inline int hull_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

static inline size_t hull_strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

/* ── Compiler-emitted libc (memcpy / memset / memmove) ─────────────── */

/*
 * Even under -nostdlib, clang lowers struct copies, block initializers,
 * and runtime-length byte loops to *implicit* calls to the standard
 * symbols memcpy / memset / memmove. Without definitions those become
 * undefined wasm imports that trap on first call ("failed to call
 * unlinked import function (env, memcpy)"). Provide them so the
 * compiler-generated calls resolve inside the module.
 *
 *   - External linkage (not static): the emitted calls bind to these.
 *   - __SIZE_TYPE__ params: match the builtin's ABI exactly on wasm32,
 *     regardless of the header's own size_t typedef.
 *   - no_builtin(...) is load-bearing: at -O2 clang would otherwise
 *     recognize each body's own loop as its namesake and replace it with
 *     a self-call (infinite recursion). The attribute keeps it a loop.
 *   - Guarded to __wasm__ so a native compile of this header (host-side
 *     test) keeps using the platform libc's memcpy/memset/memmove.
 *
 * The hull_* helpers above stay for source compatibility; their loops
 * may now lower to these same definitions, which is correct and cheap.
 *
 * memcpy keeps the STANDARD contract: the regions must not overlap
 * (undefined otherwise, exactly like libc). Use memmove for overlap.
 *
 * Linkage model: these are external, so multiple translation units that
 * each include this header and are then linked together would collide.
 * Hull compiles exactly ONE translation unit per compute module
 * (compute/<name>/<name>.c -> one .wasm); multi-source-per-module is not
 * a supported build model, so a single definition per module never
 * collides. If you build a module from several objects yourself, make
 * these `weak` or keep them in a single TU.
 */
#if defined(__wasm__)

/* These definitions require clang (the compiler Hull's wasm32 toolchain
 * uses): without no_builtin the bodies would self-recurse. Fail loudly on
 * a toolchain that cannot honor it rather than emit a recursive memcpy. */
#if !defined(__has_attribute) || !__has_attribute(no_builtin)
#  error "hull_compute.h needs a compiler supporting __attribute__((no_builtin)) (clang) to define freestanding memcpy/memset/memmove safely"
#endif

__attribute__((no_builtin("memcpy")))
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((no_builtin("memmove")))
void *memmove(void *dst, const void *src, __SIZE_TYPE__ n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

__attribute__((no_builtin("memset")))
void *memset(void *dst, int c, __SIZE_TYPE__ n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

#endif /* __wasm__ */

/* ── Bump allocator (64KB arena) ──────────────────────────────────── */

#define HULL_ARENA_SIZE (64 * 1024)

static uint8_t hull_arena[HULL_ARENA_SIZE];
static size_t  hull_arena_offset = 0;

static inline void *hull_alloc(size_t size)
{
    /* Align to 8 bytes */
    size_t aligned = (size + 7) & ~(size_t)7;
    if (hull_arena_offset + aligned > HULL_ARENA_SIZE) return NULL;
    void *ptr = &hull_arena[hull_arena_offset];
    hull_arena_offset += aligned;
    return ptr;
}

static inline void hull_alloc_reset(void)
{
    hull_arena_offset = 0;
}

/* ── UDF wire format (used when module is registered as a SQL function) ── */

#define HULL_UDF_OP_SCALAR    0x01
#define HULL_UDF_OP_STEP      0x01
#define HULL_UDF_OP_FINALIZE  0x02

#define HULL_UDF_TYPE_INTEGER 0x01
#define HULL_UDF_TYPE_REAL    0x02
#define HULL_UDF_TYPE_TEXT    0x03
#define HULL_UDF_TYPE_BLOB    0x04
#define HULL_UDF_TYPE_NULL    0x05

#define HULL_UDF_RESULT_VOID    0x00
#define HULL_UDF_RESULT_INTEGER 0x01
#define HULL_UDF_RESULT_REAL    0x02
#define HULL_UDF_RESULT_TEXT    0x03
#define HULL_UDF_RESULT_BLOB    0x04
#define HULL_UDF_RESULT_NULL    0x05

/* Helper: read opcode and argc from UDF input */
static inline uint8_t hull_udf_opcode(const void *in) { return ((const uint8_t *)in)[0]; }
static inline uint8_t hull_udf_argc(const void *in)   { return ((const uint8_t *)in)[1]; }

#endif /* HULL_COMPUTE_H */
]]

-- ── Embedded hull_span.h (canonical: templates/hull_span.h) ────────────
-- Byte-identical to templates/hull_span.h; enforced by tests/e2e_compute_headers.sh.
local HULL_SPAN_H = [[/*
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
 * signed zero) exactly — no arithmetic. Union punning is well-defined in C. ──── */
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
 * calls happen after setup — every later access is a pure inline read. */
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
]]

-- ── Module template (C) ────────────────────────────────────────────────

local function module_template_c(name)
    return string.format([[/*
 * %s.c — Hull WASM compute module
 *
 * Implement your processing logic in hull_process().
 * Input bytes arrive via in_ptr/in_len, write output to out_ptr (up to out_max).
 * Return the number of bytes written, or a negative error code.
 *
 * Build:  hull compute build %s
 * Test:   hull compute test %s
 */

#include "hull_compute.h"

HULL_VERSION_EXPORT

/*
 * hull_process — Main entry point
 *
 * Computes a simple byte-sum score (0-100) from the input.
 * Replace this with your actual processing logic.
 */
HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (out_max < 1)
        return HULL_ERR_OUTPUT;

    if (in_len <= 0) {
        /* Empty input: score = 0 */
        *(uint8_t *)out_ptr = 0;
        return 1;
    }

    /* Sum all input bytes, map to 0-100 range */
    const uint8_t *input = (const uint8_t *)in_ptr;
    uint32_t sum = 0;
    for (int32_t i = 0; i < in_len; i++)
        sum += input[i];

    uint8_t score = (uint8_t)(sum %% 101);
    *(uint8_t *)out_ptr = score;
    return 1;
}
]], name, name, name)
end

-- ── Test fixtures template ─────────────────────────────────────────────

local TEST_FIXTURES = [[
[
    {"name": "basic", "input": "hello", "expect_status": 0},
    {"name": "empty input", "input": "", "expect_status": 0}
]
]]

-- ── Argument parsing ───────────────────────────────────────────────────

local function parse_args()
    local opts = {
        subcmd = nil,
        name = nil,
        lang = "c",
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--lang" then
            i = i + 1
            opts.lang = arg[i]
        elseif a:sub(1, 1) ~= "-" then
            if not opts.subcmd then
                opts.subcmd = a
            elseif not opts.name then
                opts.name = a
            end
        end
        i = i + 1
    end

    return opts
end

-- ── Validation ─────────────────────────────────────────────────────────

local function validate_module_name(name)
    if not name or not name:match("^[a-zA-Z0-9_%-]+$") then
        tool.stderr("hull compute: invalid module name '" .. tostring(name) .. "'\n")
        tool.stderr("  Names must contain only letters, digits, underscores, and hyphens.\n")
        tool.exit(1)
    end
end

-- ── Helpers ────────────────────────────────────────────────────────────
-- Compilation lookup, module discovery, and per-module clang invocation
-- live in stdlib/lua/hull/compute_build.lua so they can be reused by
-- stdlib/lua/hull/build.lua during `hull build` auto-rebuilds.

-- ── Tempdir test harness (used by cmd_test + cmd_check) ───────────────
--
-- Both `hull compute test` and `hull compute check` exercise a module by
-- spawning a tempdir Hull app containing the .wasm + a synthesized
-- app.lua + tests, then running `hull test` against it. The shared
-- shape is extracted here so the two commands differ only in what they
-- write into the app.

--- Create a tempdir, copy `compute/<name>.wasm` into it.
-- @return path to the new tempdir (caller frees with cleanup_harness)
local function setup_harness(name, wasm_path)
    local tmpdir = tool.tmpdir()
    if not tmpdir then
        tool.stderr("hull compute: failed to create temp directory\n")
        tool.exit(1)
    end
    tool.mkdir(tmpdir .. "/compute")
    tool.copy(wasm_path, tmpdir .. "/compute/" .. name .. ".wasm")
    tool.mkdir(tmpdir .. "/tests")
    return tmpdir
end

--- Run `hull test <tmpdir>`. Returns true on pass.
local function run_harness(tmpdir)
    local hull_exe = __hull_exe or "hull"
    return tool.spawn({hull_exe, "test", tmpdir})
end

--- Remove the tempdir.
local function cleanup_harness(tmpdir)
    if tmpdir then tool.rmdir(tmpdir) end
end

--- Escape a string for Lua-source embedding (used by test fixture inputs).
local function lua_escape(s)
    return s:gsub("\\", "\\\\")
            :gsub('"', '\\"')
            :gsub("\n", "\\n")
            :gsub("\r", "\\r")
end

-- ── Subcommand: new ────────────────────────────────────────────────────

-- ── Hull-owned header set (canonical: templates/*) ─────────────────────
-- Both headers are installed/refreshed together. Keep this list the single
-- registration point; install_headers keeps the pair consistent.
local HULL_HEADERS = {
    { name = "hull_compute.h", body = HULL_COMPUTE_H },
    { name = "hull_span.h",    body = HULL_SPAN_H },
}

-- Install/update every Hull-owned header into `dir` (both-or-neither) with
-- rollback ON A REPORTED OPERATION FAILURE. Stage all headers to temp files
-- FIRST, so a staging failure touches no real file; then rename each into place,
-- backing up any existing original and restoring it if a later rename reports
-- failure. Returns true on success, or false, err on failure with every real
-- file restored to its pre-call state. Idempotent (identical bytes re-installed).
--
-- NOTE: this is failure-atomic, NOT crash-atomic. A process crash / power loss
-- BETWEEN the individual renames can leave a mismatched pair plus leftover
-- .hull-tmp / .hull-bak files; recovery is to re-run `hull compute refresh-header`.
-- (Crash-atomicity would need a journal or a single directory swap, not renames.)
local function install_headers(dir)
    local items = {}
    for _, h in ipairs(HULL_HEADERS) do
        local real = dir .. "/" .. h.name
        local tmp  = real .. ".hull-tmp"
        if not tool.write_file(tmp, h.body) then
            for _, it in ipairs(items) do tool.remove_file(it.tmp) end
            return false, "staging " .. real
        end
        items[#items + 1] = { real = real, tmp = tmp, bak = nil, done = false }
    end

    local function rollback()
        for i = #items, 1, -1 do
            local it = items[i]
            if it.done then
                tool.remove_file(it.real)                       -- drop the new file
                if it.bak then tool.rename(it.bak, it.real) end -- restore the original
            elseif it.bak then
                tool.rename(it.bak, it.real)                    -- moved but not installed
            end
            tool.remove_file(it.tmp)                            -- drop any leftover temp
        end
    end

    for _, it in ipairs(items) do
        if tool.file_exists(it.real) then
            it.bak = it.real .. ".hull-bak"
            tool.remove_file(it.bak)                            -- clear any stale backup
            if not tool.rename(it.real, it.bak) then
                it.bak = nil
                rollback()
                return false, "backing up " .. it.real
            end
        end
        if not tool.rename(it.tmp, it.real) then
            rollback()
            return false, "installing " .. it.real
        end
        it.done = true
    end

    for _, it in ipairs(items) do
        if it.bak then tool.remove_file(it.bak) end             -- success: drop backups
    end
    return true
end

local function cmd_new(name, lang)
    if not name then
        tool.stderr("Usage: hull compute new <name> [--lang c]\n")
        tool.exit(1)
    end

    validate_module_name(name)

    if lang ~= "c" then
        tool.stderr("hull compute new: only --lang c is supported\n")
        tool.exit(1)
    end

    local dir = "compute/" .. name

    if tool.file_exists(dir) then
        tool.stderr("hull compute new: directory '" .. dir .. "' already exists\n")
        tool.exit(1)
    end

    -- Create directory structure
    tool.mkdir("compute")
    tool.mkdir(dir)

    -- Install both Hull-owned headers atomically (both-or-neither).
    local ok, err = install_headers(dir)
    if not ok then
        tool.stderr("hull compute new: failed to install headers (" .. tostring(err) .. ")\n")
        tool.exit(1)
    end

    -- Write module source
    tool.write_file(dir .. "/" .. name .. ".c", module_template_c(name))

    -- Write test fixtures
    tool.write_file(dir .. "/test_fixtures.json", TEST_FIXTURES)

    print("hull compute new: created " .. dir .. "/")
    print("  " .. dir .. "/hull_compute.h")
    print("  " .. dir .. "/hull_span.h")
    print("  " .. dir .. "/" .. name .. ".c")
    print("  " .. dir .. "/test_fixtures.json")
    print("")
    print("Next steps:")
    print("  hull compute build " .. name)
    print("  hull compute test " .. name)
end

-- ── Subcommand: build ──────────────────────────────────────────────────

local function cmd_build(name)
    if name then validate_module_name(name) end

    -- Discover all modules; filter to one if `name` was given.
    local all_modules = cbuild.discover_modules(".")
    local todo
    if name then
        for _, m in ipairs(all_modules) do
            if m.name == name then todo = { m }; break end
        end
        if not todo then
            local src = "compute/" .. name .. "/" .. name .. ".c"
            tool.stderr("hull compute build: source not found: " .. src .. "\n")
            tool.exit(1)
        end
    else
        todo = all_modules
        if #todo == 0 then
            tool.stderr("hull compute build: no modules found under compute/\n")
            tool.exit(1)
        end
    end

    local cc = cbuild.find_clang()
    if not cc then
        tool.stderr("hull compute build: clang not found\n")
        tool.stderr("  Install clang with wasm32 target support.\n")
        tool.stderr("  macOS: brew install llvm@18\n")
        tool.stderr("  Linux: apt install clang lld\n")
        tool.exit(1)
    end

    local all_ok = true
    for _, m in ipairs(todo) do
        print("hull compute build: " .. m.name)
        local ok, err = cbuild.compile_module(cc, m)
        if not ok then
            tool.stderr("  " .. (err or "compile failed") .. "\n")
            all_ok = false
        else
            local data = tool.read_file(m.wasm)
            if data then
                print(string.format("  -> %s (%.1f KB)", m.wasm, #data / 1024))
            end
        end
    end

    if not all_ok then tool.exit(1) end
end

-- ── Subcommand: check ──────────────────────────────────────────────────

local function cmd_check(name)
    if not name then
        tool.stderr("Usage: hull compute check <name>\n")
        tool.exit(1)
    end

    validate_module_name(name)

    local wasm_path = "compute/" .. name .. ".wasm"
    if not tool.file_exists(wasm_path) then
        tool.stderr("hull compute check: " .. wasm_path .. " not found\n")
        tool.stderr("  Run: hull compute build " .. name .. "\n")
        tool.exit(1)
    end

    -- Read and validate WASM magic number
    local data = tool.read_file(wasm_path)
    if not data or #data < 8 then
        tool.stderr("hull compute check: " .. wasm_path .. " is too small or unreadable\n")
        tool.exit(1)
    end

    -- WASM magic: \0asm (0x00 0x61 0x73 0x6d)
    local b1, b2, b3, b4 = string.byte(data, 1, 4)
    if b1 ~= 0x00 or b2 ~= 0x61 or b3 ~= 0x73 or b4 ~= 0x6d then
        tool.stderr("hull compute check: " .. wasm_path .. " is not a valid WASM module\n")
        tool.exit(1)
    end

    -- WASM version (should be 1)
    local v1, v2, v3, v4 = string.byte(data, 5, 8)
    local version = v1 + v2 * 256 + v3 * 65536 + v4 * 16777216
    if version ~= 1 then
        tool.stderr("hull compute check: unexpected WASM version: " .. version .. "\n")
        tool.exit(1)
    end

    -- Spin up a tempdir Hull app, drop in just the module, and run a
    -- tiny smoke test through `compute.call(...)`. If hull test passes,
    -- the module loads correctly in WAMR.
    local tmpdir = setup_harness(name, wasm_path)

    tool.write_file(tmpdir .. "/app.lua", string.format([[
app.manifest({ modules = {"hull/http-server@1","hull/compute@1"} })
local compute = require("hull.compute")

app.get("/check", function(req, res)
    if not compute.available() then
        res:status(500):json({ error = "wasm runtime not available" })
        return
    end
    local out, err = compute.call("%s", "test")
    if err then
        res:status(500):json({ error = err })
        return
    end
    res:json({ ok = true, output_len = #out })
end)
]], name))

    tool.write_file(tmpdir .. "/tests/test_check.lua", string.format([[
test("compute.call('%s') succeeds", function()
    local res = test.get("/check")
    test.eq(res.status, 200)
    test.ok(res.json.ok, "compute.call should succeed")
end)
]], name))

    local ok = run_harness(tmpdir)
    cleanup_harness(tmpdir)

    if not ok then
        tool.stderr("hull compute check: " .. name .. " failed validation\n")
        tool.exit(1)
    end

    print("hull compute check: " .. name .. " OK")
end

-- ── Subcommand: test ───────────────────────────────────────────────────

local function cmd_test(name)
    if not name then
        tool.stderr("Usage: hull compute test <name>\n")
        tool.exit(1)
    end

    validate_module_name(name)

    local wasm_path = "compute/" .. name .. ".wasm"
    if not tool.file_exists(wasm_path) then
        tool.stderr("hull compute test: " .. wasm_path .. " not found\n")
        tool.stderr("  Run: hull compute build " .. name .. "\n")
        tool.exit(1)
    end

    -- Load test fixtures
    local fixtures_path = "compute/" .. name .. "/test_fixtures.json"
    local fixtures_data = tool.read_file(fixtures_path)
    local fixtures
    if fixtures_data then
        fixtures = json.decode(fixtures_data)
    end
    if not fixtures or #fixtures == 0 then
        -- Default fixtures if none provided
        fixtures = {
            { name = "basic", input = "hello", expect_status = 0 },
            { name = "empty input", input = "", expect_status = 0 },
        }
    end

    -- Spin up a tempdir Hull app exposing the module via /call?input=...
    -- and generate one test case per fixture entry.
    local tmpdir = setup_harness(name, wasm_path)

    local app_src = table.concat({
        "-- Auto-generated test app for compute module: " .. name,
        'app.manifest({ modules = {"hull/http-server@1","hull/compute@1"} })',
        'local compute = require("hull.compute")',
        "",
        'app.get("/health", function(req, res) res:json({ ok = true }) end)',
        "",
        'app.get("/call", function(req, res)',
        '    local input = req.query.input or ""',
        string.format('    local out, err = compute.call("%s", input)', name),
        '    if err then',
        '        res:status(500):json({ error = err })',
        '        return',
        '    end',
        '    res:json({ ok = true, output_len = #out, output_bytes = { string.byte(out, 1, #out) } })',
        'end)',
    }, "\n") .. "\n"
    tool.write_file(tmpdir .. "/app.lua", app_src)

    local test_lines = {
        "-- Auto-generated tests for compute module: " .. name,
        "",
    }
    for _, fixture in ipairs(fixtures) do
        local fname = lua_escape(fixture.name or "unnamed")
        local input = fixture.input or ""
        local expect_status = fixture.expect_status or 0
        local escaped = lua_escape(input)
        test_lines[#test_lines + 1] = string.format('test("fixture: %s", function()', fname)
        test_lines[#test_lines + 1] = string.format('    local res = test.get("/call?input=%s")', escaped)
        if expect_status == 0 then
            test_lines[#test_lines + 1] = '    test.eq(res.status, 200)'
            test_lines[#test_lines + 1] = '    test.ok(res.json.ok, "compute.call should succeed")'
        else
            test_lines[#test_lines + 1] = '    test.eq(res.status, 500)'
        end
        test_lines[#test_lines + 1] = "end)"
        test_lines[#test_lines + 1] = ""
    end
    tool.write_file(tmpdir .. "/tests/test_compute.lua", table.concat(test_lines, "\n"))

    local ok = run_harness(tmpdir)
    cleanup_harness(tmpdir)

    if not ok then tool.exit(1) end
end

-- ── Subcommand: refresh-header ─────────────────────────────────────────
--
-- `hull_compute.h` is owned by Hull. The canonical version is embedded
-- in this file (HULL_COMPUTE_H above) and written to each module's dir
-- on `hull compute new`. When Hull bumps the ABI or adds a new helper,
-- existing module directories carry a stale copy.
--
-- `hull compute refresh-header [name]` refreshes the per-module copies of
-- BOTH Hull-owned headers (hull_compute.h + hull_span.h) from the embedded
-- canonical versions, atomically per module (both-or-neither, so a failure
-- never leaves a mismatched pair). A legacy module that predates hull_span.h
-- gains it here. With no name, refreshes every discovered module.

local function cmd_refresh_header(name)
    if name then validate_module_name(name) end

    local modules
    if name then
        if not tool.file_exists("compute/" .. name) then
            tool.stderr("hull compute refresh-header: compute/" .. name ..
                        "/ does not exist\n")
            tool.exit(1)
        end
        modules = { { name = name } }
    else
        modules = cbuild.discover_modules(".")
        if #modules == 0 then
            tool.stderr("hull compute refresh-header: no modules under compute/\n")
            tool.exit(1)
        end
    end

    -- Refresh BOTH Hull-owned headers per module, atomically. A legacy module
    -- that has only hull_compute.h gains hull_span.h here (backward compatible).
    -- On a per-module failure the pair is rolled back (never mismatched) and the
    -- command aborts non-zero; already-refreshed modules stay consistent and a
    -- re-run is idempotent.
    local written = 0
    for _, m in ipairs(modules) do
        local dir = "compute/" .. m.name
        local ok, err = install_headers(dir)
        if not ok then
            tool.stderr("hull compute refresh-header: " .. dir ..
                        ": refresh failed and was rolled back (" .. tostring(err) .. ")\n")
            tool.exit(1)
        end
        print("hull compute refresh-header: " .. dir .. "/{hull_compute.h,hull_span.h}")
        written = written + 1
    end
    print("hull compute refresh-header: refreshed " .. written .. " module header pair(s)")
end

-- ── Usage ──────────────────────────────────────────────────────────────

local function print_usage()
    print("Usage: hull compute <command> [options]")
    print("")
    print("Commands:")
    print("  new <name>             Create a new WASM compute module")
    print("  build [name]           Compile module(s) to .wasm")
    print("  test <name>            Run test fixtures against a module")
    print("  check <name>           Validate a .wasm module loads correctly")
    print("  refresh-header [name]  Refresh per-module hull_compute.h + hull_span.h from the embedded canonical versions (atomic)")
    print("")
    print("Options:")
    print("  --lang c               Language for 'new' (default: c, only c supported)")
    print("")
    print("Examples:")
    print("  hull compute new score")
    print("  hull compute build score")
    print("  hull compute build          # build all modules")
    print("  hull compute test score")
    print("  hull compute check score")
end

-- ── Main ───────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()

    if not opts.subcmd then
        print_usage()
        tool.exit(1)
    end

    if opts.subcmd == "new" then
        cmd_new(opts.name, opts.lang)
    elseif opts.subcmd == "build" then
        cmd_build(opts.name)
    elseif opts.subcmd == "test" then
        cmd_test(opts.name)
    elseif opts.subcmd == "check" then
        cmd_check(opts.name)
    elseif opts.subcmd == "refresh-header" then
        cmd_refresh_header(opts.name)
    else
        tool.stderr("hull compute: unknown command '" .. opts.subcmd .. "'\n\n")
        print_usage()
        tool.exit(1)
    end
end

-- The tool dispatcher (src/hull/tool.c) invokes the returned main() only when
-- this module is the entry command it was asked to run. A module that is
-- require()'d as a dependency (e.g. by an app during manifest extraction in
-- the tool VM) hands its main() back but is never called, so it can't run
-- against the wrong argv.
return main
