/*
 * vfs.c — Unified Virtual Filesystem implementation
 *
 * Binary search over sorted HlEntry arrays for O(log n) exact and
 * prefix lookups. Filesystem path construction for dev-mode fallback.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/vfs.h"

#include <assert.h>
#include <stdio.h>
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
