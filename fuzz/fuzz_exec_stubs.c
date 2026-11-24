/*
 * fuzz_exec_stubs.c: the allocator cap/exec.c builds with, over libc, for
 * fuzz_exec_policy / fuzz_exec_slot. The real one (utils/alloc.c) pulls in
 * the arena and Keel's allocator interface. No Hull header is included, so
 * the signatures cannot conflict.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>

typedef struct HlAllocator HlAllocator;

void *hl_alloc_malloc(HlAllocator *a, size_t size);
void *hl_alloc_realloc(HlAllocator *a, void *p, size_t old_size, size_t new_size);
void  hl_alloc_free(HlAllocator *a, void *p, size_t size);

void *hl_alloc_malloc(HlAllocator *a, size_t size)
{
    (void)a;
    return malloc(size);
}

void *hl_alloc_realloc(HlAllocator *a, void *p, size_t old_size, size_t new_size)
{
    (void)a; (void)old_size;
    return realloc(p, new_size);
}

void hl_alloc_free(HlAllocator *a, void *p, size_t size)
{
    (void)a; (void)size;
    free(p);
}
