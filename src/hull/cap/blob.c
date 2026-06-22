/**
 * @file cap/blob.c
 * @brief App-facing capability wrapper around hull/blob_store.
 *
 * The CAS implementation moved to src/hull/blob_store.c (shared with
 * runtime infrastructure: bytecode cache, AOT cache, etc.). This
 * file is now a thin policy layer that only enforces the per-app
 * manifest gate on init; every other entry point forwards directly
 * to the store. Future audit emission or per-call permission checks
 * land here, not in blob_store.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/blob.h"
#include "hull/cap/fs.h"
#include "hull/utils/alloc.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ── Init: manifest validation + path resolution ─────────────────── */

/* Resolve `dir` (relative to fs_cfg->base_dir) to an absolute path
 * the store can open. Allocated via `alloc`; caller frees on
 * fall-through but `hl_blob_store_open` takes ownership of a copy
 * (the store keeps its own buffer). */
static int resolve_root(const HlFsConfig *fs_cfg, const char *dir,
                        char *out, size_t out_cap)
{
    if (!fs_cfg || !fs_cfg->base_dir || !dir) return -1;
    size_t base_len = fs_cfg->base_len;
    size_t dir_len  = strlen(dir);
    while (dir_len > 0 && dir[dir_len - 1] == '/') dir_len--;
    if (dir_len == 0) return -1;

    if (base_len + 1 + dir_len + 1 > out_cap) return -1;
    memcpy(out, fs_cfg->base_dir, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, dir, dir_len);
    out[base_len + 1 + dir_len] = '\0';
    return 0;
}

int hl_cap_blob_init(HlBlob **out,
                     const HlFsConfig *fs_cfg,
                     HlAllocator *alloc,
                     const char *dir,
                     int shard_depth,
                     uint64_t tmp_max_age_sec)
{
    if (!out || !fs_cfg || !dir) return -1;

    /* Trim trailing slashes so "data/blobs/" works the same as
     * "data/blobs". hl_cap_fs_validate rejects internal "//" but
     * accepts both forms here after the trim. */
    char trimmed[PATH_MAX];
    size_t dir_len = strlen(dir);
    while (dir_len > 0 && dir[dir_len - 1] == '/') dir_len--;
    if (dir_len == 0 || dir_len >= sizeof(trimmed)) return -1;
    memcpy(trimmed, dir, dir_len);
    trimmed[dir_len] = '\0';

    const char *err = NULL;
    if (hl_cap_fs_validate(fs_cfg, trimmed, &err) != 0) return -1;

    char abs_root[PATH_MAX];
    if (resolve_root(fs_cfg, trimmed, abs_root, sizeof(abs_root)) != 0)
        return -1;

    return hl_blob_store_open(out, alloc, abs_root,
                              shard_depth, tmp_max_age_sec);
}

void hl_cap_blob_free(HlBlob *b)
{
    hl_blob_store_close(b);
}

/* ── Forwarders (HlBlob is a typedef alias of HlBlobStore) ───────── */

int hl_cap_blob_put(HlBlob *b, const uint8_t *buf, size_t len,
                    const char *expected, char *out_id)
{
    return hl_blob_store_put(b, buf, len, expected, out_id);
}

int hl_cap_blob_put_durable(HlBlob *b, const uint8_t *buf, size_t len,
                            const char *expected, char *out_id)
{
    return hl_blob_store_put_durable(b, buf, len, expected, out_id);
}

int hl_cap_blob_writer_open(HlBlob *b, const char *expected,
                            HlBlobWriter **out)
{
    return hl_blob_store_writer_open(b, expected, out);
}

int hl_cap_blob_writer_open_durable(HlBlob *b, const char *expected,
                                    HlBlobWriter **out)
{
    return hl_blob_store_writer_open_durable(b, expected, out);
}

int hl_cap_blob_writer_write(HlBlobWriter *w,
                             const uint8_t *buf, size_t len)
{
    return hl_blob_store_writer_write(w, buf, len);
}

int hl_cap_blob_writer_finalize(HlBlobWriter *w,
                                char *out_id, size_t *out_size)
{
    return hl_blob_store_writer_finalize(w, out_id, out_size);
}

void hl_cap_blob_writer_abort(HlBlobWriter *w)
{
    hl_blob_store_writer_abort(w);
}

int hl_cap_blob_get(HlBlob *b, const char *id, int track_access,
                    uint8_t **out_buf, size_t *out_len)
{
    return hl_blob_store_get(b, id, track_access, out_buf, out_len);
}

int hl_cap_blob_reader_open(HlBlob *b, const char *id, int track_access,
                            HlBlobReader **out)
{
    return hl_blob_store_reader_open(b, id, track_access, out);
}

int hl_cap_blob_reader_read(HlBlobReader *r,
                            uint8_t *buf, size_t cap, size_t *out_len)
{
    return hl_blob_store_reader_read(r, buf, cap, out_len);
}

void hl_cap_blob_reader_close(HlBlobReader *r)
{
    hl_blob_store_reader_close(r);
}

int hl_cap_blob_exists(HlBlob *b, const char *id)
{
    return hl_blob_store_exists(b, id);
}

int hl_cap_blob_stat(HlBlob *b, const char *id,
                     size_t *size, int64_t *atime)
{
    return hl_blob_store_stat(b, id, size, atime);
}

int hl_cap_blob_delete(HlBlob *b, const char *id)
{
    return hl_blob_store_delete(b, id);
}

int hl_cap_blob_iter(HlBlob *b, HlBlobIterCb cb, void *user)
{
    return hl_blob_store_iter(b, cb, user);
}

uint64_t hl_cap_blob_total_size(HlBlob *b)
{
    return hl_blob_store_total_size(b);
}

uint64_t hl_cap_blob_count(HlBlob *b)
{
    return hl_blob_store_count(b);
}

int hl_cap_blob_cleanup(HlBlob *b, const HlBlobCleanupOpts *opts,
                        uint64_t *removed_out, uint64_t *freed_out)
{
    return hl_blob_store_cleanup(b, opts, removed_out, freed_out);
}
