/*
 * hull_compute.h — Hull WASM compute module ABI header
 *
 * Freestanding header for Hull compute plugins. Provides:
 *   - Type definitions (no stdlib dependency)
 *   - Host call interface (logging, data segments)
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
