/*
 * hull_alloc.c - Unified tracking allocator implementation
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/utils/alloc.h"
#include <sh_arena.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Accounting (thread-safe) ──────────────────────────────────────── */

/* Count @p size against the allocator, or refuse if that would pass the
 * limit. Reserving BEFORE the allocation (and handing it back if malloc
 * fails) makes check and add one step, so two threads cannot both pass the
 * check for the last bytes under the limit. */
static int account_reserve(HlAllocator *a, size_t size)
{
    size_t cur = atomic_load_explicit(&a->used, memory_order_relaxed);
    for (;;) {
        if (a->limit > 0 && (size > a->limit || cur > a->limit - size))
            return -1;
        if (atomic_compare_exchange_weak_explicit(&a->used, &cur, cur + size,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }
    size_t now = cur + size;
    size_t peak = atomic_load_explicit(&a->peak, memory_order_relaxed);
    while (now > peak &&
           !atomic_compare_exchange_weak_explicit(&a->peak, &peak, now,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
        ;
    return 0;
}

static void account_release(HlAllocator *a, size_t size)
{
    size_t cur = atomic_load_explicit(&a->used, memory_order_relaxed);
    for (;;) {
        size_t next = cur >= size ? cur - size : 0;
        if (atomic_compare_exchange_weak_explicit(&a->used, &cur, next,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            return;
    }
}

/* ── Tracked allocation functions ──────────────────────────────────── */

void *hl_alloc_malloc(HlAllocator *a, size_t size)
{
    if (!a)
        return malloc(size);

    if (account_reserve(a, size) != 0)
        return NULL;
    void *p = malloc(size);
    if (!p)
        account_release(a, size);
    return p;
}

void *hl_alloc_calloc(HlAllocator *a, size_t count, size_t size)
{
    if (!a)
        return calloc(count, size);

    /* Overflow-safe: check count * size before multiplying */
    if (count > 0 && size > SIZE_MAX / count)
        return NULL;

    size_t total = count * size;
    void *p = hl_alloc_malloc(a, total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *hl_alloc_realloc(HlAllocator *a, void *ptr,
                       size_t old_size, size_t new_size)
{
    if (!a)
        return realloc(ptr, new_size);

    if (new_size > old_size) {
        size_t delta = new_size - old_size;
        if (account_reserve(a, delta) != 0)
            return NULL;
        void *p = realloc(ptr, new_size);
        if (!p)
            account_release(a, delta);
        return p;
    }

    void *p = realloc(ptr, new_size);
    if (p)
        account_release(a, old_size - new_size);
    return p;
}

void hl_alloc_free(HlAllocator *a, void *ptr, size_t size)
{
    if (!ptr)
        return;
    if (a)
        account_release(a, size);
    free(ptr);
}

/* ── KlAllocator vtable ───────────────────────────────────────────── */

static void *hl_kl_malloc(void *ctx, size_t size)
{
    return hl_alloc_malloc((HlAllocator *)ctx, size);
}

static void *hl_kl_realloc(void *ctx, void *ptr,
                           size_t old_size, size_t new_size)
{
    return hl_alloc_realloc((HlAllocator *)ctx, ptr, old_size, new_size);
}

static void hl_kl_free(void *ctx, void *ptr, size_t size)
{
    hl_alloc_free((HlAllocator *)ctx, ptr, size);
}

KlAllocator hl_alloc_kl(HlAllocator *a)
{
    return (KlAllocator){
        .malloc  = hl_kl_malloc,
        .realloc = hl_kl_realloc,
        .free    = hl_kl_free,
        .ctx     = a,
    };
}

/* ── Arena wrappers ────────────────────────────────────────────────── */

SHArena *hl_arena_create(HlAllocator *a, size_t capacity)
{
    if (capacity > SIZE_MAX - sizeof(SHArena))
        return NULL;
    size_t total = sizeof(SHArena) + capacity;
    if (a && account_reserve(a, total) != 0)
        return NULL;

    SHArena *arena = sh_arena_create(capacity);
    if (!arena && a)
        account_release(a, total);
    return arena;
}

void hl_arena_free(HlAllocator *a, SHArena *arena)
{
    if (!arena)
        return;

    if (a) {
        account_release(a, sizeof(SHArena) + arena->capacity);
    }
    sh_arena_free(arena);
}

/* ── Scoped scratch (mark / rewind) ────────────────────────────────────
 *
 * sh_arena is a non-growing bump allocator: pointers are stable for the
 * arena's whole lifetime (it never reallocs), so a savepoint is just the
 * current `used` offset and rewinding is restoring it. These wrap the
 * hand-rolled `saved = arena->used; ...; arena->used = saved` idiom into a
 * named, bounds-checked API so a REQUEST arena can host nested SCRATCH
 * scopes without a second arena. Allocations made after the mark become
 * invalid once rewound - do not retain pointers past the rewind.
 */
size_t hl_arena_mark(const SHArena *arena)
{
    return arena ? arena->used : 0;
}

void hl_arena_rewind(SHArena *arena, size_t mark)
{
    if (!arena)
        return;
    /* A mark can only ever be <= the current high-water mark; a larger
     * value means the caller passed a mark from a different arena or a
     * stale one. Clamp rather than expose uninitialised bytes. */
    if (mark <= arena->used)
        arena->used = mark;
}

char *hl_arena_strdup(SHArena *arena, const char *s)
{
    if (!arena || !s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = sh_arena_alloc(arena, n);
    if (p)
        memcpy(p, s, n);
    return p;
}

void *hl_arena_memdup(SHArena *arena, const void *src, size_t n)
{
    if (!arena || (!src && n))
        return NULL;
    void *p = sh_arena_alloc(arena, n);
    if (p && n)
        memcpy(p, src, n);
    return p;
}
