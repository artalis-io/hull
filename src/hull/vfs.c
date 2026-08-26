/*
 * vfs.c - Unified Virtual Filesystem implementation
 *
 * Binary search over sorted HlEntry arrays for O(log n) exact and
 * prefix lookups. Filesystem path construction for dev-mode fallback.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/vfs.h"

#include <sh_seal_arena.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hl_vfs_init(HlVfs *vfs, const HlEntry *entries, const char *root_dir)
{
    assert(vfs);
    assert(entries);

    vfs->entries  = entries;
    vfs->root_dir = root_dir;

    /* Count entries */
    size_t n = 0;
    while (entries[n].name)
        n++;
    vfs->count = n;

    /* Debug-assert sorted order */
#ifndef NDEBUG
    for (size_t i = 1; i < n; i++) {
        assert(strcmp(entries[i - 1].name, entries[i].name) < 0 &&
               "HlVfs entries must be sorted by name");
    }
#endif
}

static int hl_entry_name_cmp(const void *a, const void *b)
{
    const HlEntry *ea = (const HlEntry *)a;
    const HlEntry *eb = (const HlEntry *)b;
    return strcmp(ea->name, eb->name);
}

static size_t hl_entry_count(const HlEntry *a)
{
    size_t n = 0;
    while (a && a[n].name)
        n++;
    return n;
}

void hl_vfs_init_composed(HlVfs *vfs, const HlEntry *base,
                          const HlEntry *const *feat_arrays, size_t feat_count,
                          const char *root_dir, void **out_owned)
{
    assert(vfs);
    assert(base);
    if (out_owned) *out_owned = NULL;

    /* Total feature entries across all composed runtime archives. */
    size_t nf = 0;
    for (size_t i = 0; i < feat_count; i++)
        nf += hl_entry_count(feat_arrays ? feat_arrays[i] : NULL);

    /* Fast path: nothing composed -> borrow the static base, no allocation.
     * This is the common base build and stays byte-identical to hl_vfs_init. */
    if (nf == 0) {
        hl_vfs_init(vfs, base, root_dir);
        return;
    }

    size_t nb    = hl_entry_count(base);
    size_t total = nb + nf;

    /* Overflow guard on the byte size (total is bounded by embedded module
     * counts, so this is defensive; degrade to base if it ever trips). */
    if (total + 1 > SIZE_MAX / sizeof(HlEntry)) {
        hl_vfs_init(vfs, base, root_dir);
        return;
    }
    size_t bytes = (total + 1) * sizeof(HlEntry);

    /* Build the merged table in a SEALED arena (RW while filling, mprotect RO
     * after): this module-lookup table is boot-built + read on every module
     * load, so it earns the same read-only protection as the manifest. */
    ShSealArena *arena = malloc(sizeof(*arena));
    HlEntry *merged = NULL;
    if (arena && sh_seal_arena_init(arena, bytes, "platform_vfs") == 0)
        merged = sh_seal_arena_alloc(arena, bytes, _Alignof(HlEntry));
    if (!merged) {
        /* Degrade to the base set rather than crash (OOM / mmap failure). */
        if (arena) { sh_seal_arena_destroy(arena); free(arena); }
        hl_vfs_init(vfs, base, root_dir);
        return;
    }

    size_t k = 0;
    for (size_t i = 0; i < nb; i++)
        merged[k++] = base[i];
    for (size_t i = 0; i < feat_count; i++) {
        const HlEntry *a = feat_arrays ? feat_arrays[i] : NULL;
        for (size_t j = 0; a && a[j].name; j++)
            merged[k++] = a[j];
    }
    merged[k].name = NULL;
    merged[k].data = NULL;
    merged[k].len  = 0;

    /* Sort by name in C strcmp order - the same total order the Makefile's
     * LC_ALL=C sort produces, so hl_vfs_init's sorted-order assert holds and
     * binary search stays correct. Sort BEFORE sealing (the seal makes it RO). */
    qsort(merged, total, sizeof(*merged), hl_entry_name_cmp);

    /* Seal RO. If mprotect ever fails the table is still valid (just unsealed) -
     * strictly no worse than the pre-seal heap version; do not fail the build. */
    (void)sh_seal_arena_seal(arena);

    if (out_owned) *out_owned = arena;
    hl_vfs_init(vfs, merged, root_dir);
}

void hl_vfs_composed_free(void *owned)
{
    if (!owned) return;
    ShSealArena *arena = (ShSealArena *)owned;
    sh_seal_arena_destroy(arena);
    free(arena);
}

const HlEntry *hl_vfs_find(const HlVfs *vfs, const char *name)
{
    if (!vfs || !name || vfs->count == 0)
        return NULL;

    const HlEntry *base = vfs->entries;
    size_t lo = 0;
    size_t hi = vfs->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(base[mid].name, name);
        if (cmp < 0)
            lo = mid + 1;
        else if (cmp > 0)
            hi = mid;
        else
            return &base[mid];
    }

    return NULL;
}

/*
 * Find the lower bound: the first entry whose name >= prefix.
 * Returns vfs->count if all entries are < prefix.
 */
static size_t lower_bound(const HlVfs *vfs, const char *prefix, size_t prefix_len)
{
    size_t lo = 0;
    size_t hi = vfs->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strncmp(vfs->entries[mid].name, prefix, prefix_len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }

    return lo;
}

size_t hl_vfs_prefix(const HlVfs *vfs, const char *prefix,
                     const HlEntry **first)
{
    if (!vfs || !prefix || !first) {
        if (first) *first = NULL;
        return 0;
    }

    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0 || vfs->count == 0) {
        *first = NULL;
        return 0;
    }

    size_t idx = lower_bound(vfs, prefix, prefix_len);

    /* Check if the first candidate actually matches */
    if (idx >= vfs->count ||
        strncmp(vfs->entries[idx].name, prefix, prefix_len) != 0) {
        *first = NULL;
        return 0;
    }

    *first = &vfs->entries[idx];

    /* Scan forward to count contiguous matches */
    size_t count = 0;
    while (idx + count < vfs->count &&
           strncmp(vfs->entries[idx + count].name, prefix, prefix_len) == 0)
        count++;

    return count;
}

int hl_vfs_has_prefix(const HlVfs *vfs, const char *prefix)
{
    if (!vfs || !prefix || vfs->count == 0)
        return 0;

    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0)
        return 0;

    size_t idx = lower_bound(vfs, prefix, prefix_len);

    return (idx < vfs->count &&
            strncmp(vfs->entries[idx].name, prefix, prefix_len) == 0);
}

int hl_vfs_path(const HlVfs *vfs, const char *name,
                char *buf, size_t buf_size)
{
    if (!vfs || !vfs->root_dir || !name || !buf || buf_size == 0)
        return -1;

    int n = snprintf(buf, buf_size, "%s/%s", vfs->root_dir, name);
    if (n < 0 || (size_t)n >= buf_size)
        return -1;

    return n;
}
