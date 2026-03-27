/*
 * hull_compute.h — Hull WASM compute module ABI header
 *
 * Freestanding header for Hull compute plugins. Provides:
 *   - Type definitions (no stdlib dependency)
 *   - Host call interface (logging, data segments)
 *   - Minimal libc replacements (memcpy, memset, memcmp, strlen)
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

#endif /* HULL_COMPUTE_H */
