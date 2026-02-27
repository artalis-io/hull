/*
 * hull_alloc.c — Unified tracking allocator implementation
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/alloc.h"
#include <sh_arena.h>
#include <stdint.h>
#include <stdlib.h>

/* ── Tracked allocation functions ──────────────────────────────────── */

void *hl_alloc_malloc(HlAllocator *a, size_t size)
{
    if (!a)
        return malloc(size);

    if (a->limit > 0 && (size > a->limit || a->used > a->limit - size))
        return NULL;

    void *p = malloc(size);
    if (p) {
        a->used += size;
        if (a->used > a->peak)
            a->peak = a->used;
    }
    return p;
}

void *hl_alloc_realloc(HlAllocator *a, void *ptr,
                       size_t old_size, size_t new_size)
{
    if (!a)
        return realloc(ptr, new_size);

    if (new_size > old_size) {
        size_t delta = new_size - old_size;
        if (a->limit > 0 && (delta > a->limit || a->used > a->limit - delta))
            return NULL;
    }

    void *p = realloc(ptr, new_size);
    if (p) {
        if (new_size > old_size) {
            a->used += new_size - old_size;
        } else {
            size_t delta = old_size - new_size;
            a->used = (a->used >= delta) ? a->used - delta : 0;
        }
        if (a->used > a->peak)
            a->peak = a->used;
    }
    return p;
}

void hl_alloc_free(HlAllocator *a, void *ptr, size_t size)
{
    if (!ptr)
        return;
    if (a)
        a->used = (a->used >= size) ? a->used - size : 0;
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
    if (a && a->limit > 0 && (total > a->limit || a->used > a->limit - total))
        return NULL;

    SHArena *arena = sh_arena_create(capacity);
    if (!arena)
        return NULL;

    if (a) {
        a->used += total;
        if (a->used > a->peak)
            a->peak = a->used;
    }
    return arena;
}

void hl_arena_free(HlAllocator *a, SHArena *arena)
{
    if (!arena)
        return;

    if (a) {
        size_t total = sizeof(SHArena) + arena->capacity;
        a->used = (a->used >= total) ? a->used - total : 0;
    }
    sh_arena_free(arena);
}
