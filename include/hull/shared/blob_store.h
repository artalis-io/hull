/**
 * @file blob_store.h
 * @brief Low-level content-addressed blob store.
 *
 * Pure CAS primitive - bytes in → SHA-256-keyed ID; bytes out by ID.
 * Independent of the app capability layer, the manifest, the audit
 * log, and HL_ENABLE_BLOB. Two distinct families of consumer sit on
 * top:
 *
 *   1. `hl_cap_blob_*` (cap/blob.c) - the app-facing capability:
 *      adds manifest fs.write validation, may add audit emission and
 *      per-app permission checks. Backs the `blob.*` Lua/JS bindings.
 *
 *   2. Runtime infrastructure - Lua bytecode cache
 *      (runtime/lua/bytecode_cache.c), compute AOT cache
 *      (stdlib/cli/lua/hull/aot_cache.lua via tool.blob_store_*
 *      bindings), future template-AST cache. These live in the
 *      shared `$HOME/.hull/blobs/runtime/<kind>/` pool, auto-allowed
 *      by the sandbox.
 *
 * Layout (identical to the legacy cap layout):
 *
 *   <root>/
 *   ├── blobs/
 *   │   ├── 00/    (or 00/00/ with shard_depth=2)
 *   │   │   └── <hash>          file contents == sha256-hex
 *   │   └── ...
 *   └── tmp/
 *       └── .blob-<rand>.tmp    in-flight; init sweeps stale ones
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_BLOB_STORE_H
#define HL_BLOB_STORE_H

#include <stddef.h>
#include <stdint.h>

/* Forward decl - defined in hull/alloc.h. */
typedef struct HlAllocator HlAllocator;

/** @brief SHA-256 hex string length (without NUL). */
#define HL_BLOB_STORE_ID_HEX_LEN 64

/** @brief Buffer size for a NUL-terminated SHA-256 hex string. */
#define HL_BLOB_STORE_ID_BUF_SIZE 65

/* ── Lifecycle ───────────────────────────────────────────────────── */

/** Opaque blob-store handle. */
typedef struct HlBlobStore HlBlobStore;

/**
 * @brief Open a blob store rooted at `abs_root`.
 *
 * `abs_root` must be an absolute filesystem path. Caller is
 * responsible for any policy validation (manifest fs.write checks,
 * sandbox unveil, etc.) - this layer does no permission gating.
 *
 * Creates `<abs_root>/blobs/` and `<abs_root>/tmp/` if absent.
 * Sweeps stale `tmp/.blob-*.tmp` files older than `tmp_max_age_sec`
 * (default 3600 if 0; UINT64_MAX disables the sweep).
 *
 * @param out               Receives the new store handle.
 * @param alloc             Allocator for the handle and internal state.
 * @param abs_root          Absolute path for the store root.
 * @param shard_depth       1 (256 subdirs) or 2 (65K subdirs). Values
 *                          outside [1,2] are clamped to 1.
 * @param tmp_max_age_sec   Stale-tmp sweep threshold in seconds.
 *
 * @return 0 on success; -1 on mkdir / allocation failure.
 */
int hl_blob_store_open(HlBlobStore **out,
                       HlAllocator *alloc,
                       const char *abs_root,
                       int shard_depth,
                       uint64_t tmp_max_age_sec);

/** Close + free a store handle. Safe to pass NULL. */
void hl_blob_store_close(HlBlobStore *s);

/* ── Write paths ─────────────────────────────────────────────────── */

/** Buffer-mode put. SHA-256 computed on the fly during temp-file
 *  write; atomic via rename(2). See cap/blob.h for full semantics. */
int hl_blob_store_put(HlBlobStore *s,
                      const uint8_t *buf, size_t len,
                      const char *expected,
                      char *out_id);

/** Durable buffer-mode put. fsync(fd) + fsync(dirfd) on success. */
int hl_blob_store_put_durable(HlBlobStore *s,
                              const uint8_t *buf, size_t len,
                              const char *expected,
                              char *out_id);

/**
 * @brief Caller-keyed put. Writes `bytes` to the slot named by
 *        `key` without computing SHA on the contents.
 *
 * For KEY-VALUE caches (Lua bytecode, compute AOT, template AST)
 * where the cache key is derived from the source/inputs and the
 * artifact's hash is not pre-known. `key` must be 64 lowercase hex
 * characters (same shape as a CAS id, so the existing
 * exists/stat/get/delete entry points work unchanged for both
 * modes).
 *
 * Atomic via tmp + rename, just like the content-keyed put.
 * Tolerates concurrent writes of the same key: identical keys are
 * expected to produce identical bytes (otherwise the cache is
 * misdesigned), so last-write-wins is safe.
 *
 * **Per-store discipline.** Don't mix CAS and keyed puts on the
 * same store - CAS callers rely on "filename IS the SHA", which
 * a keyed put would violate. Use a dedicated store per consumer.
 *
 * @return 0 on success; -1 on invalid key, write failure, or
 *         rename failure.
 */
int hl_blob_store_put_keyed(HlBlobStore *s, const char *key,
                            const uint8_t *bytes, size_t len);

/** Opaque streaming-writer handle. */
typedef struct HlBlobStoreWriter HlBlobStoreWriter;

int hl_blob_store_writer_open(HlBlobStore *s, const char *expected,
                              HlBlobStoreWriter **out);

int hl_blob_store_writer_open_durable(HlBlobStore *s, const char *expected,
                                      HlBlobStoreWriter **out);

int hl_blob_store_writer_write(HlBlobStoreWriter *w,
                               const uint8_t *buf, size_t len);

int hl_blob_store_writer_finalize(HlBlobStoreWriter *w,
                                  char *out_id, size_t *out_size);

void hl_blob_store_writer_abort(HlBlobStoreWriter *w);

/* ── Read paths ──────────────────────────────────────────────────── */

int hl_blob_store_get(HlBlobStore *s, const char *id, int track_access,
                      uint8_t **out_buf, size_t *out_len);

/** Opaque streaming-reader handle. */
typedef struct HlBlobStoreReader HlBlobStoreReader;

int hl_blob_store_reader_open(HlBlobStore *s, const char *id, int track_access,
                              HlBlobStoreReader **out);

int hl_blob_store_reader_read(HlBlobStoreReader *r,
                              uint8_t *buf, size_t cap,
                              size_t *out_len);

void hl_blob_store_reader_close(HlBlobStoreReader *r);

/* ── Metadata ────────────────────────────────────────────────────── */

int hl_blob_store_exists(HlBlobStore *s, const char *id);

int hl_blob_store_stat(HlBlobStore *s, const char *id,
                       size_t *size, int64_t *atime);

int hl_blob_store_delete(HlBlobStore *s, const char *id);

/**
 * @brief Maximum bytes any in-memory `_get` / `verify_read_all`
 *        will materialise from a single blob.
 *
 * Defensive ceiling against a corrupt-stat or planted-huge-file
 * scenario. Comfortably above every legitimate cache entry today
 * (largest stdlib bytecode ~50 KB; largest AOT module a few
 * hundred KB). Callers that need to stream larger payloads use
 * the reader_open / _read API directly.
 *
 * Single source of truth shared by `hl_blob_store_get` (in-process
 * lookup path) and `commands/cache.c::verify_read_all` (cache
 * verify offline path). Bumping this is one edit.
 */
#define HL_BLOB_STORE_MAX_IN_MEMORY_BYTES \
    ((size_t)(256u * 1024u * 1024u))

/**
 * @brief Compose the absolute path of the blob @p id under store @p s.
 *
 * Writes `<root>/blobs/<shard>/<id>` (shard depth determined by
 * the store's open-time configuration). Does NOT touch the
 * filesystem - pure string construction. Validates @p id as a
 * lowercase 64-char hex string before composing.
 *
 * Useful for callers that need direct filesystem access to a blob
 * file (e.g. `cache verify` recomputing the sha; `tools install`
 * chmod'ing the freshly-stored binary). Routing through this helper
 * keeps shard-layout knowledge inside the storage layer - the same
 * code that wrote the file owns the rule for naming it.
 *
 * @return 0 on success, -1 on invalid id, store, or undersized buffer.
 */
int hl_blob_store_compose_path(HlBlobStore *s, const char *id,
                               char *out, size_t out_cap);

/* ── Enumeration ─────────────────────────────────────────────────── */

typedef int (*HlBlobStoreIterCb)(const char *id, size_t size, void *user);

int hl_blob_store_iter(HlBlobStore *s, HlBlobStoreIterCb cb, void *user);

uint64_t hl_blob_store_total_size(HlBlobStore *s);
uint64_t hl_blob_store_count(HlBlobStore *s);

/* ── Eviction ────────────────────────────────────────────────────── */

typedef enum {
    HL_BLOB_STORE_LRU  = 0,
    HL_BLOB_STORE_FIFO = 1,
} HlBlobStoreCleanupStrategy;

typedef struct {
    uint64_t                   max_total_size;
    uint64_t                   max_age_sec;
    HlBlobStoreCleanupStrategy strategy;
    int                        dry_run;
} HlBlobStoreCleanupOpts;

int hl_blob_store_cleanup(HlBlobStore *s,
                          const HlBlobStoreCleanupOpts *opts,
                          uint64_t *removed_out, uint64_t *freed_out);

#endif /* HL_BLOB_STORE_H */
