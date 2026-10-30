/**
 * @file cap/blob.h
 * @brief App-facing capability layer over the low-level blob store.
 *
 * Thin wrapper around `hull/blob_store.h`. Adds the manifest
 * `fs.write` validation at init time so the per-app cap layer keeps
 * the manifest-gate semantics every other `hl_cap_*` enjoys. Once
 * initialised, the cap-layer handle (`HlBlob`) IS an alias for the
 * underlying `HlBlobStore` - every other entry point is a one-line
 * forwarder. Future audit / per-call permission checks would land
 * here too, not in the store.
 *
 * Runtime infrastructure (Lua bytecode cache, compute AOT cache,
 * template AST cache) skips this header entirely and uses
 * `hull/blob_store.h` directly with paths under
 * `$HOME/.hull/blobs/runtime/<kind>/`.
 *
 * See docs/blob.md for the layered architecture write-up.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_BLOB_H
#define HL_CAP_BLOB_H

#include "hull/shared/blob_store.h"

#include <stddef.h>
#include <stdint.h>

/* Forward decl - pulled in via includes by the implementation. */
typedef struct HlFsConfig HlFsConfig;
typedef struct HlAllocator HlAllocator;

/* ── Compatibility aliases ───────────────────────────────────────── */
/* The cap layer's public surface predates the store/cap split.
 * Existing callers (mod_blob.c bindings, tests/hull/cap/test_blob.c,
 * the documentation, the agent JSON producers) reference `HlBlob`,
 * `HL_BLOB_ID_*`, `HL_BLOB_LRU/FIFO`, etc. - preserve those names as
 * aliases so the refactor is invisible to callers. */

#define HL_BLOB_ID_HEX_LEN   HL_BLOB_STORE_ID_HEX_LEN
#define HL_BLOB_ID_BUF_SIZE  HL_BLOB_STORE_ID_BUF_SIZE

typedef HlBlobStore       HlBlob;
typedef HlBlobStoreWriter HlBlobWriter;
typedef HlBlobStoreReader HlBlobReader;

typedef HlBlobStoreIterCb HlBlobIterCb;

typedef HlBlobStoreCleanupStrategy HlBlobCleanupStrategy;
#define HL_BLOB_LRU  HL_BLOB_STORE_LRU
#define HL_BLOB_FIFO HL_BLOB_STORE_FIFO

typedef HlBlobStoreCleanupOpts HlBlobCleanupOpts;

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * @brief Initialize a blob store rooted at `dir` (manifest-gated).
 *
 * Identical semantics to the legacy entry point: validates `dir`
 * against `fs.write` via `hl_cap_fs_validate`, resolves to an
 * absolute path under `fs_cfg->base_dir`, then opens the store via
 * `hl_blob_store_open`.
 */
int hl_cap_blob_init(HlBlob **out,
                     const HlFsConfig *fs_cfg,
                     HlAllocator *alloc,
                     const char *dir,
                     int shard_depth,
                     uint64_t tmp_max_age_sec);

/** @ref hl_cap_blob_init, reporting the tmp/ sweep's work in @p work_units
 *  (see hl_blob_store_open_ex) for the binding to charge (audit 37). */
int hl_cap_blob_init_ex(HlBlob **out,
                        const HlFsConfig *fs_cfg,
                        HlAllocator *alloc,
                        const char *dir,
                        int shard_depth,
                        uint64_t tmp_max_age_sec,
                        uint64_t *work_units);

/** Free a blob store handle. Safe to pass NULL. */
void hl_cap_blob_free(HlBlob *b);

/* ── Write paths ─────────────────────────────────────────────────── */

int hl_cap_blob_put(HlBlob *b,
                    const uint8_t *buf, size_t len,
                    const char *expected,
                    char *out_id);

int hl_cap_blob_put_durable(HlBlob *b,
                            const uint8_t *buf, size_t len,
                            const char *expected,
                            char *out_id);

/* At most HL_BLOB_MAX_LIVE_HANDLES streaming writers + readers are open in
 * the process at once (a writer holds two descriptors, a reader one; audit
 * 34). An open past the cap returns HL_BLOB_ERR_TOO_MANY and opens nothing:
 * a binding collects garbage once (a dropped handle holds its slot until it
 * is finalized) and retries, as fs.mmap does. The slot is given back by
 * writer_finalize (success or not), writer_abort and reader_close. */
#define HL_BLOB_MAX_LIVE_HANDLES 256
#define HL_BLOB_ERR_TOO_MANY     (-2)

/* Handles currently open (for tests). */
int hl_cap_blob_live_handles(void);

int hl_cap_blob_writer_open(HlBlob *b, const char *expected,
                            HlBlobWriter **out);

int hl_cap_blob_writer_open_durable(HlBlob *b, const char *expected,
                                    HlBlobWriter **out);

int hl_cap_blob_writer_write(HlBlobWriter *w,
                             const uint8_t *buf, size_t len);

int hl_cap_blob_writer_finalize(HlBlobWriter *w,
                                char *out_id, size_t *out_size);

void hl_cap_blob_writer_abort(HlBlobWriter *w);

/* ── Read paths ──────────────────────────────────────────────────── */

int hl_cap_blob_get(HlBlob *b, const char *id, int track_access,
                    uint8_t **out_buf, size_t *out_len);

int hl_cap_blob_reader_open(HlBlob *b, const char *id, int track_access,
                            HlBlobReader **out);

int hl_cap_blob_reader_read(HlBlobReader *r,
                            uint8_t *buf, size_t cap,
                            size_t *out_len);

void hl_cap_blob_reader_close(HlBlobReader *r);

/* ── Metadata ────────────────────────────────────────────────────── */

int hl_cap_blob_exists(HlBlob *b, const char *id);

int hl_cap_blob_stat(HlBlob *b, const char *id,
                     size_t *size, int64_t *atime);

int hl_cap_blob_delete(HlBlob *b, const char *id);

/* ── Enumeration ─────────────────────────────────────────────────── */

int hl_cap_blob_iter(HlBlob *b, HlBlobIterCb cb, void *user);

uint64_t hl_cap_blob_total_size(HlBlob *b);
uint64_t hl_cap_blob_count(HlBlob *b);

/* Budget units of a whole-store walk (hl_blob_store_walk_shards /
 * _last_walk_entries): 1 per shard directory, charged before the walk, and 1
 * per entry the last walk found, charged after it (audit 21). */
uint64_t hl_cap_blob_walk_units_before(HlBlob *b);
uint64_t hl_cap_blob_walk_units_after(HlBlob *b);

/* ── Eviction ────────────────────────────────────────────────────── */

int hl_cap_blob_cleanup(HlBlob *b, const HlBlobCleanupOpts *opts,
                        uint64_t *removed_out, uint64_t *freed_out);

#endif /* HL_CAP_BLOB_H */
