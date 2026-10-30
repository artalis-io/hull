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
#include "hull/cap/fs_policy.h"
#include "hull/utils/alloc.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>   /* lstat: root_has_symlink */

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

/* The store creates files of its own naming anywhere beneath its directory,
 * so the manifest must grant fs.write over that directory: a SUBTREE grant at
 * or above it, a not-yet-existing directory granted with a trailing slash
 * ("data/blobs/"), or a grant naming exactly the directory ("data/blobs",
 * which for a path not made yet compiles as "create this one name" - but the
 * app named the store's own directory, so it granted the store). Without this
 * the directory only had to lie inside the app root, so blob.init made a
 * writable store with no fs.write grant at all. */
static int blob_dir_granted(const HlFsConfig *cfg, const char *dir)
{
    if (!cfg->policy)
        return 0;
    char probe[PATH_MAX];
    int n = snprintf(probe, sizeof probe, "%s/.blob-grant-probe", dir);
    if (n < 0 || (size_t)n >= sizeof probe)
        return 0;
    char scratch[PATH_MAX + 2];   /* the selection writes the residual here */
    HlFsSelection sel = hl_fs_policy_select(cfg->policy, probe, HL_FS_OPEN_WRITE,
                                            scratch, sizeof scratch);
    if (sel.entry &&
        (sel.entry->kind == HL_FS_ENTRY_SUBTREE ||
         (sel.entry->kind == HL_FS_ENTRY_CREATE &&
          sel.entry->terminal == HL_FS_TERMINAL_SUBTREE)))
        return 1;
    /* A grant naming the directory itself. */
    sel = hl_fs_policy_select(cfg->policy, dir, HL_FS_OPEN_WRITE,
                              scratch, sizeof scratch);
    return sel.entry && sel.entry->kind == HL_FS_ENTRY_CREATE;
}

/* 1 when a component of @p abs_root below the app directory (@p base_len
 * bytes of it) is a symbolic link. The store opens its root by absolute path,
 * following links, while the fs policy that granted the directory confines a
 * path to its grant's anchor: with `data/` granted and a planted
 * `data/blobs -> ../migrations`, blob.init("data/blobs") put the store - and
 * its 64-hex files, blobs/ and tmp/ - outside the grant (audit 20). A
 * component that does not exist yet is the store's to make, not a link. */
static int root_has_symlink(const char *abs_root, size_t base_len)
{
    char prefix[PATH_MAX];
    size_t len = strlen(abs_root);
    if (len >= sizeof prefix) return 1;
    memcpy(prefix, abs_root, len + 1);
    for (size_t i = base_len + 1; i <= len; i++) {
        if (prefix[i] != '/' && prefix[i] != '\0') continue;
        char c = prefix[i];
        prefix[i] = '\0';
        struct stat st;
        int r = lstat(prefix, &st);
        prefix[i] = c;
        if (r != 0) return 0;              /* the rest does not exist yet */
        if (S_ISLNK(st.st_mode)) return 1;
    }
    return 0;
}

int hl_cap_blob_init(HlBlob **out,
                     const HlFsConfig *fs_cfg,
                     HlAllocator *alloc,
                     const char *dir,
                     int shard_depth,
                     uint64_t tmp_max_age_sec)
{
    return hl_cap_blob_init_ex(out, fs_cfg, alloc, dir, shard_depth,
                               tmp_max_age_sec, NULL);
}

int hl_cap_blob_init_ex(HlBlob **out,
                        const HlFsConfig *fs_cfg,
                        HlAllocator *alloc,
                        const char *dir,
                        int shard_depth,
                        uint64_t tmp_max_age_sec,
                        uint64_t *work_units)
{
    if (work_units) *work_units = 0;
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
    if (!blob_dir_granted(fs_cfg, trimmed)) return -1;

    char abs_root[PATH_MAX];
    if (resolve_root(fs_cfg, trimmed, abs_root, sizeof(abs_root)) != 0)
        return -1;
    if (root_has_symlink(abs_root, fs_cfg->base_len))
        return -1;

    return hl_blob_store_open_ex(out, alloc, abs_root,
                                 shard_depth, tmp_max_age_sec, work_units);
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

/* Live streaming handles (HL_BLOB_MAX_LIVE_HANDLES). A writer holds two
 * descriptors and a reader one until it is finalized / aborted / closed or
 * collected, and opening one costs the app almost nothing: 61433 writers
 * exhausted macOS's per-process descriptor limit, and with it accept(), the
 * SQLite temp files and every TLS connection (audit 34). Counted here, where
 * each handle is made and consumed, so a slot is given back exactly once
 * whichever path consumes it. Atomic: a test harness or an embedder may open
 * from more than one thread. */
static atomic_int g_blob_live;

static int blob_slot_take(void)
{
    int n = atomic_load(&g_blob_live);
    do {
        if (n >= HL_BLOB_MAX_LIVE_HANDLES) return -1;
    } while (!atomic_compare_exchange_weak(&g_blob_live, &n, n + 1));
    return 0;
}

static void blob_slot_give(void) { atomic_fetch_sub(&g_blob_live, 1); }

int hl_cap_blob_live_handles(void) { return atomic_load(&g_blob_live); }

int hl_cap_blob_writer_open(HlBlob *b, const char *expected,
                            HlBlobWriter **out)
{
    if (blob_slot_take() != 0) return HL_BLOB_ERR_TOO_MANY;
    int rc = hl_blob_store_writer_open(b, expected, out);
    if (rc != 0) blob_slot_give();
    return rc;
}

int hl_cap_blob_writer_open_durable(HlBlob *b, const char *expected,
                                    HlBlobWriter **out)
{
    if (blob_slot_take() != 0) return HL_BLOB_ERR_TOO_MANY;
    int rc = hl_blob_store_writer_open_durable(b, expected, out);
    if (rc != 0) blob_slot_give();
    return rc;
}

int hl_cap_blob_writer_write(HlBlobWriter *w,
                             const uint8_t *buf, size_t len)
{
    return hl_blob_store_writer_write(w, buf, len);
}

int hl_cap_blob_writer_finalize(HlBlobWriter *w,
                                char *out_id, size_t *out_size)
{
    if (!w) return -1;
    int rc = hl_blob_store_writer_finalize(w, out_id, out_size);
    blob_slot_give();          /* finalize consumes the writer either way */
    return rc;
}

void hl_cap_blob_writer_abort(HlBlobWriter *w)
{
    if (!w) return;
    hl_blob_store_writer_abort(w);
    blob_slot_give();
}

int hl_cap_blob_get(HlBlob *b, const char *id, int track_access,
                    uint8_t **out_buf, size_t *out_len)
{
    return hl_blob_store_get_verified(b, id, track_access, out_buf, out_len);
}

int hl_cap_blob_reader_open(HlBlob *b, const char *id, int track_access,
                            HlBlobReader **out)
{
    if (blob_slot_take() != 0) return HL_BLOB_ERR_TOO_MANY;
    int rc = hl_blob_store_reader_open(b, id, track_access, out);
    if (rc != 0) blob_slot_give();
    return rc;
}

int hl_cap_blob_reader_read(HlBlobReader *r,
                            uint8_t *buf, size_t cap, size_t *out_len)
{
    return hl_blob_store_reader_read(r, buf, cap, out_len);
}

void hl_cap_blob_reader_close(HlBlobReader *r)
{
    if (!r) return;
    hl_blob_store_reader_close(r);
    blob_slot_give();
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

uint64_t hl_cap_blob_walk_units_before(HlBlob *b)
{
    return hl_blob_store_walk_shards(b);
}

uint64_t hl_cap_blob_walk_units_after(HlBlob *b)
{
    return hl_blob_store_last_walk_entries(b);
}

int hl_cap_blob_cleanup(HlBlob *b, const HlBlobCleanupOpts *opts,
                        uint64_t *removed_out, uint64_t *freed_out)
{
    return hl_blob_store_cleanup(b, opts, removed_out, freed_out);
}
