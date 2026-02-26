/*
 * hull_alloc.h — Unified tracking allocator for Hull
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
void *hl_alloc_realloc(HlAllocator *a, void *ptr,
                       size_t old_size, size_t new_size);
void  hl_alloc_free(HlAllocator *a, void *ptr, size_t size);

/* Return a KlAllocator vtable routing through this tracker. */
KlAllocator hl_alloc_kl(HlAllocator *a);

/* Arena wrappers — track backing buffer on the allocator.
 * If 'a' is NULL, equivalent to sh_arena_create/sh_arena_free. */
SHArena *hl_arena_create(HlAllocator *a, size_t capacity);
void     hl_arena_free(HlAllocator *a, SHArena *arena);

/* Accessors */
static inline size_t hl_alloc_used(const HlAllocator *a) { return a->used; }
static inline size_t hl_alloc_peak(const HlAllocator *a) { return a->peak; }

#endif /* HL_ALLOC_H */
