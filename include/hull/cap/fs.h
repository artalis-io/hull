/**
 * @file cap/fs.h
 * @brief Filesystem capability with path validation.
 *
 * All filesystem access from Lua/JS runtimes goes through these
 * functions. Three layers of defense:
 *   1. @ref hl_cap_fs_validate — path-traversal + symlink-escape check.
 *   2. Manifest allowlist (caller-supplied via #HlFsConfig).
 *   3. Kernel sandbox `unveil` (Linux/Cosmo) or Seatbelt profile (macOS),
 *      applied separately by `sandbox.c`.
 *
 * App code never sees raw paths — only `fs.read("data/log.txt")` etc.,
 * which are validated against the manifest's declared `fs.read` /
 * `fs.write` patterns.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_FS_H
#define HL_CAP_FS_H

#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
typedef struct HlAllocator HlAllocator;

/**
 * @brief Filesystem-capability configuration.
 *
 * Hull-allocated; the caller owns the lifetime. `base_dir` is the
 * app's working directory. All paths are validated to resolve to a
 * descendant of `base_dir`.
 */
typedef struct HlFsConfig {
    const char *base_dir;   /**< Absolute path of the app's root directory. */
    size_t      base_len;   /**< `strlen(base_dir)` cached for hot-path prefix checks. */
} HlFsConfig;

/**
 * @brief Validate a relative path against the manifest + traversal rules.
 *
 * Rejection rules (in order):
 *   - Absolute paths (starts with `/` or `\`).
 *   - Empty path.
 *   - Any `..` component (path-traversal).
 *   - Embedded NUL.
 *   - `realpath()` result that escapes `cfg->base_dir` (symlink-escape).
 *
 * @param cfg      Filesystem capability config. Must not be NULL.
 * @param path     Relative path to validate.
 * @param err_msg  Out-parameter: receives a static string explaining the
 *                 rejection reason. May be NULL if caller doesn't care.
 *
 * @return `0` if the path is acceptable, `-1` if rejected.
 *
 * @note Every mutating function (`_read` / `_write` / `_delete` / `_mmap`)
 *       calls this first. Direct callers from C should too.
 */
int hl_cap_fs_validate(const HlFsConfig *cfg, const char *path,
                       const char **err_msg);

/**
 * @brief Read a file's full contents into a caller-supplied buffer.
 *
 * @param cfg       Filesystem config.
 * @param path      Relative path; validated.
 * @param buf       Output buffer.
 * @param buf_size  Capacity of @p buf in bytes.
 * @param err_msg   Out-parameter for error description; may be NULL.
 *
 * @return Number of bytes read on success (≥ 0), or `-1` on failure.
 *         Reads larger than `buf_size` are rejected (caller's buffer
 *         is left untouched). NUL-terminator NOT written — caller
 *         knows the byte count from the return value.
 */
int64_t hl_cap_fs_read(const HlFsConfig *cfg, const char *path,
                         char *buf, size_t buf_size,
                         const char **err_msg);

/**
 * @brief Write bytes to a file. Creates parent directories if absent.
 *
 * @param cfg      Filesystem config.
 * @param path     Relative path; validated. Parent dirs created with `mkdir -p`.
 * @param data     Bytes to write (may contain NULs).
 * @param len      Byte count.
 * @param err_msg  Out-parameter for error description; may be NULL.
 *
 * @return `0` on success, `-1` on failure (validate / mkdir / fopen / fwrite).
 *
 * @note Overwrites existing files atomically (write-temp + rename).
 */
int hl_cap_fs_write(const HlFsConfig *cfg, const char *path,
                      const char *data, size_t len,
                      const char **err_msg);

/**
 * @brief Test whether a file exists at @p path.
 *
 * @param cfg      Filesystem config.
 * @param path     Relative path; validated.
 * @param err_msg  Out-parameter for error description; may be NULL.
 *
 * @return `1` if the file exists, `0` if it does not, `-1` if the path
 *         failed validation.
 */
int hl_cap_fs_exists(const HlFsConfig *cfg, const char *path,
                     const char **err_msg);

/**
 * @brief Delete a file.
 *
 * @param cfg      Filesystem config.
 * @param path     Relative path; validated.
 * @param err_msg  Out-parameter for error description; may be NULL.
 *
 * @return `0` on success or already-absent, `-1` on validation failure
 *         or other I/O error.
 */
int hl_cap_fs_delete(const HlFsConfig *cfg, const char *path,
                     const char **err_msg);

/* ── Memory-mapped file buffer ─────────────────────────────────────── */

/**
 * @brief Read-only memory-mapped file region.
 *
 * Returned by @ref hl_cap_fs_mmap. The mapping is `PROT_READ |
 * MAP_PRIVATE`. Reachable from Lua/JS as a `MappedBuffer` userdata, and
 * can be passed directly to `compute.call` / `gpu.buffer` / `gpu.dispatch`
 * for zero-copy disk-to-WASM/GPU paths (see @ref buffer.h
 * "HlBufferView").
 */
typedef struct HlMappedBuffer {
    void         *addr;   /**< mmap'd region. Read-only on the kernel side. */
    size_t        len;    /**< Mapping length in bytes. */
    int           closed; /**< `1` iff already munmap'd; further `_munmap` is a no-op. */
    HlAllocator  *alloc;  /**< Tracked allocator (NULL = raw malloc). */
} HlMappedBuffer;

/**
 * @brief Memory-map a file for read-only access.
 *
 * @param cfg      Filesystem config.
 * @param path     Relative path; validated.
 * @param alloc    Allocator for the heap-allocated wrapper struct. NULL = raw malloc.
 * @param err_msg  Out-parameter for error description; may be NULL.
 *
 * @return Heap-allocated @ref HlMappedBuffer on success (caller must
 *         eventually free via @ref hl_cap_fs_munmap), or NULL on failure.
 *
 * @par Example:
 * @code
 * HlMappedBuffer *m = hl_cap_fs_mmap(&cfg, "embeddings.bin", alloc, NULL);
 * // ... use m->addr, m->len ...
 * hl_cap_fs_munmap(m);
 * @endcode
 */
HlMappedBuffer *hl_cap_fs_mmap(const HlFsConfig *cfg, const char *path,
                                HlAllocator *alloc, const char **err_msg);

/**
 * @brief Unmap and free a mapped buffer. Idempotent.
 *
 * Calls `munmap(addr, len)` then frees the wrapper struct. Safe to call
 * twice — sets `closed = 1` after the first call and the second is a
 * no-op.
 *
 * @param buf  Buffer to unmap. May be NULL (no-op).
 */
void hl_cap_fs_munmap(HlMappedBuffer *buf);

#endif /* HL_CAP_FS_H */
