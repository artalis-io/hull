/*
 * seal_arena.c — see include/hull/seal_arena.h.
 *
 * POSIX-only (mmap/mprotect/sysconf). Cosmopolitan's libc shim
 * provides the same surface, so APE builds work unmodified.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/seal_arena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── internals ─────────────────────────────────────────────────────── */

/* Detect the system page size. Cached on first call — the value never
 * changes during a process lifetime. Falls back to 4096 if sysconf
 * fails (defensive; every supported platform reports >= 4096). */
static size_t cached_page_size(void)
{
    static size_t cached = 0;
    if (cached == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (size_t)ps : 4096u;
    }
    return cached;
}

/* Round `n` up to the next multiple of `align`. Caller guarantees
 * `align` is a power of two and > 0. Returns 0 on overflow. */
static size_t align_up(size_t n, size_t align)
{
    if (align == 0) return 0;
    size_t mask = align - 1;
    if (n > SIZE_MAX - mask) return 0; /* overflow */
    return (n + mask) & ~mask;
}

static int is_power_of_two(size_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/* ── lifecycle ─────────────────────────────────────────────────────── */

int hl_seal_arena_init(HlSealArena *arena, size_t size, const char *name)
{
    if (!arena) return -1;
    /* Zero-init first so a failed init leaves the struct in a clean
     * state where destroy is a no-op and is_sealed returns 0. */
    arena->base      = NULL;
    arena->capacity  = 0;
    arena->used      = 0;
    arena->page_size = 0;
    arena->sealed    = 0;
    arena->name      = name;

    size_t ps = cached_page_size();
    size_t cap = (size == 0) ? ps : align_up(size, ps);
    if (cap == 0 || cap < size) return -1; /* overflow on round-up */

    void *base = mmap(NULL, cap, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -1;

    arena->base      = base;
    arena->capacity  = cap;
    arena->page_size = ps;
    return 0;
}

void hl_seal_arena_destroy(HlSealArena *arena)
{
    if (!arena || !arena->base) return;
    /* munmap works regardless of current protection (RW or RO). */
    (void)munmap(arena->base, arena->capacity);
    arena->base      = NULL;
    arena->capacity  = 0;
    arena->used      = 0;
    arena->page_size = 0;
    arena->sealed    = 0;
    arena->name      = NULL;
}

/* ── allocation ────────────────────────────────────────────────────── */

void *hl_seal_arena_alloc(HlSealArena *arena, size_t size, size_t align)
{
    if (!arena || !arena->base) return NULL;
    if (arena->sealed) return NULL;
    if (!is_power_of_two(align)) return NULL;
    if (size == 0) {
        /* Returning the next bump position for a zero-byte alloc is
         * defensible (and matches malloc(0) implementations that
         * return distinct non-NULL pointers), but for simplicity we
         * reject. Callers shouldn't be asking for zero. */
        return NULL;
    }

    size_t start = align_up(arena->used, align);
    if (start == 0 && arena->used != 0) return NULL; /* alignment overflow */
    if (start > SIZE_MAX - size) return NULL;        /* offset overflow */
    size_t end = start + size;
    if (end > arena->capacity) return NULL;          /* OOM */

    char *p = (char *)arena->base + start;
    arena->used = end;
    return p;
}

char *hl_seal_arena_strdup(HlSealArena *arena, const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len == SIZE_MAX) return NULL; /* +1 would overflow */
    char *dst = (char *)hl_seal_arena_alloc(arena, len + 1, 1);
    if (!dst) return NULL;
    memcpy(dst, s, len + 1);
    return dst;
}

void *hl_seal_arena_memdup(HlSealArena *arena, const void *src, size_t len)
{
    if (!src) return NULL;
    /* Alignment 1 keeps the bump tight for byte-string copies. Callers
     * that need a stricter alignment should hl_seal_arena_alloc directly
     * and memcpy themselves. */
    void *dst = hl_seal_arena_alloc(arena, len, 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    return dst;
}

/* ── seal ─────────────────────────────────────────────────────────── */

int hl_seal_arena_seal(HlSealArena *arena)
{
    if (!arena || !arena->base) return -1;
    if (arena->sealed) return -1; /* double-seal is a programming bug */
    /* mprotect applies to whole pages. arena->capacity is already a
     * multiple of page_size from init, so the whole mapping flips
     * to read-only in one call. We do NOT shrink the mapping to the
     * `used` portion — leaving the trailing pages in the mapping is
     * harmless (they're read-only and zero-initialized by mmap). */
    if (mprotect(arena->base, arena->capacity, PROT_READ) != 0) {
        return -1;
    }
    arena->sealed = 1;
    return 0;
}

int hl_seal_arena_is_sealed(const HlSealArena *arena)
{
    return (arena && arena->sealed) ? 1 : 0;
}

/* ── diagnostics ──────────────────────────────────────────────────── */

size_t hl_seal_arena_capacity(const HlSealArena *arena)
{
    return arena ? arena->capacity : 0;
}

size_t hl_seal_arena_used(const HlSealArena *arena)
{
    return arena ? arena->used : 0;
}

size_t hl_seal_arena_remaining(const HlSealArena *arena)
{
    if (!arena) return 0;
    return arena->capacity - arena->used;
}
