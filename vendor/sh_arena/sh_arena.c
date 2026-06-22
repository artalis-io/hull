/*
 * sh_arena.c - Arena allocator implementation
 *
 * ── Hull local modification ─────────────────────────────────────────
 * This vendored file carries two deliberate, documented Hull changes
 * (kept minimal so it stays easy to diff against upstream):
 *
 *   1. Unconditional integer-overflow guard in sh_arena_alloc(), for
 *      parity with sh_seal_arena_alloc() which already guards it. The
 *      original `(size + ALIGN-1)` round-up and `used + size` bounds
 *      check could both wrap for a near-SIZE_MAX size and hand back a
 *      pointer to an undersized region. Now they fail closed (NULL).
 *
 *   2. AddressSanitizer-integrated poisoning (active only when this TU
 *      is built with -fsanitize=address — Hull's `make debug`). The
 *      arena is one malloc, so ASan can't see sub-allocation lifetimes
 *      on its own. We poison the whole buffer on create/reset, unpoison
 *      each sub-allocation, and unpoison before free — turning a
 *      dangling read into a reset/freed arena region (from instrumented
 *      Hull code) into a hard ASan error. Compiled to nothing without
 *      ASan, so release is unaffected.
 * ────────────────────────────────────────────────────────────────────
 */

#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>  /* For SIZE_MAX */

/* ASan manual poisoning — no-ops unless this TU is ASan-instrumented.
 * __has_feature is clang-only; define a 0 fallback so the #if parses on
 * GCC (where an undefined __has_feature(x) would otherwise be a syntax
 * error, not just false). GCC reports ASan via __SANITIZE_ADDRESS__. */
#ifndef __has_feature
#  define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#  include <sanitizer/asan_interface.h>
#  define SH_ARENA_POISON(p, n)   __asan_poison_memory_region((p), (n))
#  define SH_ARENA_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#  define SH_ARENA_POISON(p, n)   ((void)(p), (void)(n))
#  define SH_ARENA_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

SHArena *sh_arena_create(size_t capacity)
{
    SHArena *arena = malloc(sizeof(SHArena));
    if (!arena) return NULL;

    arena->buffer = malloc(capacity);
    if (!arena->buffer) {
        free(arena);
        return NULL;
    }

    arena->capacity = capacity;
    arena->used = 0;
    /* Nothing handed out yet: the whole buffer is off-limits until a
     * sub-allocation unpoisons its region. */
    SH_ARENA_POISON(arena->buffer, capacity);
    return arena;
}

void *sh_arena_alloc(SHArena *arena, size_t size)
{
    if (!arena || !arena->buffer) return NULL;

    /* Guard the alignment round-up against wrap (a near-SIZE_MAX size
     * would otherwise round to a tiny value and pass the bounds check). */
    if (size > SIZE_MAX - (SH_ARENA_ALIGN - 1)) return NULL;

    /* Align to SH_ARENA_ALIGN bytes for double/pointer alignment */
    size = (size + SH_ARENA_ALIGN - 1) & ~(size_t)(SH_ARENA_ALIGN - 1);

    /* Non-wrapping bounds check: capacity - used cannot underflow because
     * used <= capacity is an invariant. */
    if (size > arena->capacity - arena->used) {
        return NULL;  /* Out of space */
    }

    void *ptr = arena->buffer + arena->used;
    arena->used += size;
    SH_ARENA_UNPOISON(ptr, size);  /* this region is now live */
    return ptr;
}

void *sh_arena_calloc(SHArena *arena, size_t count, size_t size)
{
    /* Check for integer overflow before multiplication */
    if (size > 0 && count > SIZE_MAX / size) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = sh_arena_alloc(arena, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void sh_arena_reset(SHArena *arena)
{
    if (arena) {
        /* Re-poison everything: every prior sub-allocation is now invalid,
         * so a retained pointer read traps under ASan instead of silently
         * reading reusable bytes. */
        SH_ARENA_POISON(arena->buffer, arena->capacity);
        arena->used = 0;
    }
}

void sh_arena_free(SHArena *arena)
{
    if (arena) {
        /* Unpoison before free so the allocator's own bookkeeping (and
         * ASan's free interceptor) see a clean region. */
        SH_ARENA_UNPOISON(arena->buffer, arena->capacity);
        free(arena->buffer);
        free(arena);
    }
}

size_t sh_arena_remaining(const SHArena *arena)
{
    if (!arena) return 0;
    return arena->capacity - arena->used;
}

size_t sh_arena_used(const SHArena *arena)
{
    if (!arena) return 0;
    return arena->used;
}
