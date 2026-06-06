/**
 * @file cap/blob.h
 * @brief Content-addressed blob storage capability.
 *
 * Pure CAS primitive: bytes in → SHA-256-keyed ID; bytes out by ID.
 * Other modules (hull/web/attachment, compute AOT cache, hull tools
 * install, Lua bytecode cache, ...) layer policy (naming, refcounts,
 * MIME validation, auth, eviction) on top. Blob owns none of those.
 *
 * Key properties:
 *   - **On-the-fly SHA-256.** Bytes flow through the hasher in
 *     lockstep with the temp-file write — never buffered just to
 *     hash, never re-read.
 *   - **Atomic writes.** Temp + rename(2). Readers never see partial.
 *   - **Self-verifying.** Filename IS the SHA-256 hex of the contents.
 *   - **Idempotent put.** Concurrent puts of identical bytes are safe.
 *   - **No SQLite dependency.** Compiles and runs under HL_ENABLE_DB=0.
 *   - **No encryption / compression.** Layer them at the consumer
 *     boundary if needed; see docs/blob.md for the encrypt-then-store
 *     pattern.
 *
 * Storage layout:
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

#ifndef HL_CAP_BLOB_H
#define HL_CAP_BLOB_H

#include <stddef.h>
#include <stdint.h>

/* Forward decls — pulled in via includes by the implementation. */
typedef struct HlFsConfig HlFsConfig;
typedef struct HlAllocator HlAllocator;

/** @brief SHA-256 hex string length (without NUL). */
#define HL_BLOB_ID_HEX_LEN 64

/** @brief Buffer size for a NUL-terminated SHA-256 hex string. */
#define HL_BLOB_ID_BUF_SIZE 65

/* ── Lifecycle ───────────────────────────────────────────────────── */

/** Opaque blob-store handle. */
typedef struct HlBlob HlBlob;

/**
 * @brief Initialize a blob store rooted at `dir`.
 *
 * Validates `dir` against the manifest's `fs.write` allowlist via
 * `hl_cap_fs_validate()`. Creates `<dir>/blobs/` and `<dir>/tmp/` if
 * absent. Sweeps any pre-existing files under the tmp/ subdir older
 * than `tmp_max_age_sec` (default 3600 if 0). Allocates the handle
 * via `alloc`.
 *
 * @param out               Receives the new HlBlob*.
 * @param fs_cfg            Filesystem capability config (for path
 *                          validation and base_dir resolution).
 * @param alloc             Allocator for the handle and internal
 *                          state.
 * @param dir               Relative path under fs_cfg->base_dir for
 *                          the blob root.
 * @param shard_depth       1 (default, 256 subdirs) or 2 (65 K dirs).
 *                          Values outside [1,2] are clamped to 1.
 * @param tmp_max_age_sec   Stale-tmp sweep threshold in seconds.
 *                          0 means "use default" (3600). UINT64_MAX
 *                          disables the sweep entirely.
 *
 * @return 0 on success; -1 on validation failure, mkdir failure, or
 *         allocation failure.
 */
int hl_cap_blob_init(HlBlob **out,
                       const HlFsConfig *fs_cfg,
                       HlAllocator *alloc,
                       const char *dir,
                       int shard_depth,
                       uint64_t tmp_max_age_sec);

/** Free a blob store handle and all its state. Safe to pass NULL. */
void hl_cap_blob_free(HlBlob *b);

/* ── Write paths ─────────────────────────────────────────────────── */

/**
 * @brief Write a buffer to the blob store. SHA-256 is computed on the
 *        fly during the temp-file write.
 *
 * Atomic via temp + rename. If the target blob already exists with
 * matching content (i.e. matching SHA), the temp file is deleted and
 * the existing file is left in place — no overwrite needed.
 *
 * @param b         Blob store handle.
 * @param buf       Bytes to store.
 * @param len       Byte count.
 * @param expected  Optional pre-computed SHA-256 hex (64 chars). When
 *                  non-NULL, the call fails if the computed hash
 *                  doesn't match (e.g. for verified-write of a tool
 *                  binary against a signed release manifest).
 * @param out_id    Receives the SHA-256 hex string of the bytes
 *                  (NUL-terminated; pass a buffer of size
 *                  HL_BLOB_ID_BUF_SIZE).
 *
 * @return 0 on success; -1 on I/O failure or SHA mismatch (when
 *         `expected` was supplied).
 */
int hl_cap_blob_put(HlBlob *b,
                      const uint8_t *buf, size_t len,
                      const char *expected,
                      char *out_id);

/* Streaming writer. Open then any number of write() calls then
 * finalize OR abort. The writer holds an open file descriptor for
 * its private tmp file plus an incremental SHA-256 context updated
 * per write(). Finalize closes the fd, atomically renames into
 * place, and returns the SHA hex. Abort closes + unlinks. If
 * neither is called (e.g. caller dropped the writer on the floor),
 * the GC finalizer in the Lua/JS binding silently aborts; in C the
 * caller is responsible.
 */

/** Opaque streaming-writer handle. */
typedef struct HlBlobWriter HlBlobWriter;

/**
 * @brief Open a streaming writer.
 *
 * @param b         Blob store handle.
 * @param expected  Optional pre-computed SHA-256 hex; finalize() will
 *                  fail if the streamed bytes don't hash to this.
 * @param out       Receives the new writer.
 *
 * @return 0 on success; -1 on tmp-file open failure.
 */
int hl_cap_blob_writer_open(HlBlob *b, const char *expected,
                              HlBlobWriter **out);

/**
 * @brief Feed bytes into the writer. May be called any number of
 *        times with any chunk size, including 0.
 *
 * @return 0 on success; -1 on write failure (writer becomes invalid;
 *         next call should be abort()).
 */
int hl_cap_blob_writer_write(HlBlobWriter *w,
                               const uint8_t *buf, size_t len);

/**
 * @brief Finalize the writer: close the tmp fd, compute the final
 *        SHA, atomic-rename to the blob path, and return the hex id.
 *
 * If `expected` was supplied to open() and the computed SHA differs,
 * the tmp file is removed and -1 is returned. If the target already
 * exists with matching bytes, the tmp is removed and 0 returned (the
 * existing file wins; no overwrite). The writer is closed and freed
 * regardless of return — do not use it after.
 *
 * @param w         Writer.
 * @param out_id    Receives SHA-256 hex (HL_BLOB_ID_BUF_SIZE buffer).
 * @param out_size  Receives total bytes written. May be NULL.
 */
int hl_cap_blob_writer_finalize(HlBlobWriter *w,
                                  char *out_id, size_t *out_size);

/**
 * @brief Abort the writer: close the tmp fd and unlink. Safe to call
 *        on an already-finalized writer (no-op). The writer is freed
 *        regardless — do not use it after.
 */
void hl_cap_blob_writer_abort(HlBlobWriter *w);

/* ── Read paths ──────────────────────────────────────────────────── */

/**
 * @brief Read the full blob into a freshly allocated buffer.
 *
 * Caller frees `*out_buf` via the same allocator passed to
 * hl_cap_blob_init().
 *
 * @param b             Blob store handle.
 * @param id            SHA-256 hex (case-insensitive; 64 chars).
 * @param track_access  Non-zero to bump the file's atime via utimes()
 *                      after the read (for LRU policy). Zero to skip
 *                      the syscall (for hot read paths).
 * @param out_buf       Receives the allocated buffer.
 * @param out_len       Receives the byte count.
 *
 * @return 0 on success; -1 on missing blob, I/O failure, or
 *         allocation failure.
 */
int hl_cap_blob_get(HlBlob *b, const char *id, int track_access,
                      uint8_t **out_buf, size_t *out_len);

/** Opaque streaming-reader handle. */
typedef struct HlBlobReader HlBlobReader;

/**
 * @brief Open a streaming reader. Optionally bumps atime on open.
 *
 * @return 0 on success; -1 on missing blob or open failure.
 */
int hl_cap_blob_reader_open(HlBlob *b, const char *id, int track_access,
                              HlBlobReader **out);

/**
 * @brief Read up to `cap` bytes into `buf`.
 *
 * @return Number of bytes read (>= 0), 0 at EOF, -1 on I/O failure.
 *         A short read followed by 0 indicates EOF.
 */
int hl_cap_blob_reader_read(HlBlobReader *r,
                              uint8_t *buf, size_t cap,
                              size_t *out_len);

/** Close + free the reader. Safe to pass NULL or call multiple times. */
void hl_cap_blob_reader_close(HlBlobReader *r);

/* ── Metadata ────────────────────────────────────────────────────── */

/** @return 1 if the blob exists, 0 if not, -1 on stat failure. */
int hl_cap_blob_exists(HlBlob *b, const char *id);

/**
 * @brief Stat a blob.
 *
 * @param size   Receives file size in bytes. May be NULL.
 * @param atime  Receives last-access time (unix seconds). May be NULL.
 *
 * @return 0 on success; -1 on missing blob.
 */
int hl_cap_blob_stat(HlBlob *b, const char *id,
                       size_t *size, int64_t *atime);

/**
 * @brief Delete a blob from disk.
 *
 * @return 1 if removed, 0 if not present, -1 on unlink failure.
 */
int hl_cap_blob_delete(HlBlob *b, const char *id);

/* ── Enumeration ─────────────────────────────────────────────────── */

/**
 * @brief Callback signature for hl_cap_blob_iter.
 *
 * Return non-zero from the callback to stop iteration early.
 */
typedef int (*HlBlobIterCb)(const char *id, size_t size, void *user);

/**
 * @brief Iterate all blobs in the store (snapshot semantics).
 *
 * Walks the directory tree once at call time, collects every (id,
 * size) pair into an internal array, then invokes `cb` for each.
 * Adds / deletes that happen during iteration are not reflected.
 *
 * @return 0 on success (whether or not the callback stopped early);
 *         -1 on directory read failure or allocation failure.
 */
int hl_cap_blob_iter(HlBlob *b, HlBlobIterCb cb, void *user);

/** @return Total bytes across all blobs in the store, or 0 on failure. */
uint64_t hl_cap_blob_total_size(HlBlob *b);

/** @return Number of blobs in the store, or 0 on failure. */
uint64_t hl_cap_blob_count(HlBlob *b);

/* ── Eviction ────────────────────────────────────────────────────── */

/** Eviction strategy for hl_cap_blob_cleanup. */
typedef enum {
    HL_BLOB_LRU = 0,   /**< Evict least-recently-accessed first. */
    HL_BLOB_FIFO = 1,  /**< Evict oldest-by-mtime first. */
} HlBlobCleanupStrategy;

/**
 * @brief Eviction options.
 *
 * Evicts blobs whose access (LRU) or modification (FIFO) time is
 * older than `max_age_sec` AND/OR until total size fits under
 * `max_total_size`. Both bounds are optional (0 = unlimited).
 *
 * `dry_run` reports what WOULD be removed without unlinking anything.
 */
typedef struct {
    uint64_t              max_total_size;  /**< 0 = unlimited */
    uint64_t              max_age_sec;     /**< 0 = unlimited */
    HlBlobCleanupStrategy strategy;
    int                   dry_run;
} HlBlobCleanupOpts;

/**
 * @brief Run a cleanup pass.
 *
 * @param removed_out  Receives the number of blobs removed (or that
 *                     would be removed in dry-run mode). May be NULL.
 * @param freed_out    Receives bytes reclaimed (would-be-reclaimed
 *                     in dry-run). May be NULL.
 *
 * @return 0 on success; -1 on iter or unlink failure (partial work
 *         may have been done — caller should check removed_out).
 */
int hl_cap_blob_cleanup(HlBlob *b, const HlBlobCleanupOpts *opts,
                          uint64_t *removed_out, uint64_t *freed_out);

#endif /* HL_CAP_BLOB_H */
