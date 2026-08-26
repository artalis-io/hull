/*
 * hull_alloc.h - Unified tracking allocator for Hull
 *
 * Wraps malloc/realloc/free with byte-level accounting.
 * Optionally enforces a hard process-level memory cap.
 * Implements KlAllocator vtable for routing Keel allocations
 * through the same tracker.
 *
 * When the allocator pointer is NULL, all functions fall back to
 * raw malloc/realloc/free with no tracking (backward compatible).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_ALLOC_H
#define HL_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <keel/allocator.h>

/* Forward declarations */
typedef struct SHArena SHArena;

typedef struct HlAllocator {
    size_t used;
    size_t limit;   /* 0 = unlimited (tracking only) */
    size_t peak;
} HlAllocator;

/* Initialize allocator. limit=0 means tracking only, no cap. */
static inline void hl_alloc_init(HlAllocator *a, size_t limit)
{
    a->used = 0;
    a->limit = limit;
    a->peak = 0;
}

/* Tracked allocation functions.
 * If 'a' is NULL, falls back to raw malloc/realloc/free. */
void *hl_alloc_malloc(HlAllocator *a, size_t size);
void *hl_alloc_calloc(HlAllocator *a, size_t count, size_t size);
void *hl_alloc_realloc(HlAllocator *a, void *ptr,
                       size_t old_size, size_t new_size);
void  hl_alloc_free(HlAllocator *a, void *ptr, size_t size);

/* Free memory the caller OWNS but holds through a const-qualified pointer.
 * Some structs type owned strings as `const char *` for a read-only API
 * (e.g. HlValue.value.s, which is a borrowed query column in one context
 * and a deep-copied owned string in another). When this code owns the
 * bytes, freeing them is correct; the cast to mutable goes through
 * uintptr_t so -Wcast-qual stays clean. The const here is a typing
 * artifact, not an immutability guarantee - never use these on a pointer
 * into sealed / .rodata memory. */
static inline void hl_free_const(const void *p)
{
    free((void *)(uintptr_t)p);
}
static inline void hl_alloc_free_const(HlAllocator *a, const void *p, size_t size)
{
    hl_alloc_free(a, (void *)(uintptr_t)p, size);
}

/* Return a KlAllocator vtable routing through this tracker. */
KlAllocator hl_alloc_kl(HlAllocator *a);

/* Arena wrappers - track backing buffer on the allocator.
 * If 'a' is NULL, equivalent to sh_arena_create/sh_arena_free. */
SHArena *hl_arena_create(HlAllocator *a, size_t capacity);
void     hl_arena_free(HlAllocator *a, SHArena *arena);

/* Scoped scratch within one arena (lifetime helpers).
 *
 * sh_arena never grows or reallocs, so a savepoint is just the current
 * used-offset. hl_arena_mark captures it; hl_arena_rewind restores it,
 * invalidating every allocation made since the mark (do not retain
 * pointers past a rewind). This lets a REQUEST arena host nested SCRATCH
 * scopes without a second arena, replacing the hand-rolled
 * `saved = arena->used; ...; arena->used = saved` idiom with a named,
 * bounds-checked call.
 *
 * hl_arena_strdup / hl_arena_memdup copy into arena memory (lifetime =
 * the arena), replacing repeated alloc+memcpy at call sites. They return
 * NULL on NULL arena or when the arena is out of space. */
size_t hl_arena_mark(const SHArena *arena);
void   hl_arena_rewind(SHArena *arena, size_t mark);
char  *hl_arena_strdup(SHArena *arena, const char *s);
void  *hl_arena_memdup(SHArena *arena, const void *src, size_t n);

/* Accessors */
static inline size_t hl_alloc_used(const HlAllocator *a) { return a->used; }
static inline size_t hl_alloc_peak(const HlAllocator *a) { return a->peak; }

#endif /* HL_ALLOC_H */
