/**
 * @file cap/fs.h
 * @brief Filesystem capability with path validation.
 *
 * All filesystem access from Lua/JS runtimes goes through these
 * functions. Three layers of defense:
 *   1. @ref hl_cap_fs_validate - path-traversal + symlink-escape check.
 *   2. Manifest allowlist (caller-supplied via #HlFsConfig).
 *   3. Kernel sandbox `unveil` (Linux/Cosmo) or Seatbelt profile (macOS),
 *      applied separately by `sandbox.c`.
 *
 * App code never sees raw paths - only `fs.read("data/log.txt")` etc.,
 * which are validated against the manifest's declared `fs.read` /
 * `fs.write` patterns.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_FS_H
#define HL_CAP_FS_H

#include <stddef.h>
#include <stdint.h>
#include "hull/cap/fs_policy.h"   /* HlFsPolicy - path-authorization policy */

/* Forward declaration */
typedef struct HlAllocator HlAllocator;

/**
 * @brief Filesystem-capability configuration.
 *
 * Hull-allocated; the caller owns the lifetime. `base_dir` is the
 * app's working directory. All paths are validated to resolve to a
 * descendant of `base_dir`.
 *
 * `policy` is the compiled path-authorization policy (sec. 6):
 * read/mmap select from its READ set, write from its WRITE set, and the op opens
 * the residual under the selected entry's held anchor fd. When `policy` is NULL
 * the fs capability is DENIED (fail closed) - the config is only wired when the
 * app declares fs.read/fs.write grants, so a no-grant app never reaches here.
 */
typedef struct HlFsConfig {
    const char        *base_dir;   /**< Absolute path of the app's root directory. */
    size_t             base_len;   /**< `strlen(base_dir)` cached for hot-path prefix checks. */
    const HlFsPolicy  *policy;     /**< Compiled read/write authorization policy (may be NULL). */
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
 *         is left untouched). NUL-terminator NOT written - caller
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

/* ── Metadata (stat) + enumeration (list) ─────── */

/**
 * @brief Node type in a stat / list result.
 *
 * Reported via `fstatat(..., AT_SYMLINK_NOFOLLOW)` - lstat semantics. A symlink is
 * #HL_FS_NODE_SYMLINK (its OWN type, NEVER followed), so a metadata op cannot alias
 * a symlink target. Ratified metadata contract for terminal symlinks: an EXACT
 * grant CANNOT name an existing symlink (the policy
 * refuses it at compile), so a reported #HL_FS_NODE_SYMLINK is only ever reached
 * through a SUBTREE/PATTERN grant, or when an authorized regular/absent target was
 * REPLACED by a symlink after compile (TOCTOU) - and even then stat/list only
 * REPORT the link while read/mmap still refuse it (`symlink_denied`).
 */
typedef enum {
    HL_FS_NODE_FILE = 0,
    HL_FS_NODE_DIR,
    HL_FS_NODE_SYMLINK,
    HL_FS_NODE_OTHER,     /**< FIFO, socket, device, ... - reported, never opened. */
} HlFsNodeType;

/**
 * @brief Portable numeric ceiling for a metadata size (2^53 - 1).
 *
 * A file whose `st_size` exceeds this makes stat / list FAIL with
 * `"size_unrepresentable"` rather than return a rounded value, so Lua and JS agree
 * EXACTLY (Lua could hold int64, but is capped identically for parity). 2^53 - 1 is
 * JavaScript's exact-integer limit; no real file approaches 8 PiB, so this is a
 * fail-closed portability guard, never a practical limit.
 */
#define HL_FS_SIZE_MAX ((uint64_t)((1ULL << 53) - 1))

/**
 * @name fs.list resource bounds
 * Exceeding ANY bound FAILS the whole op (freeing everything, no partial result) -
 * a listing is never silently truncated, and a malformed / over-long entry is never
 * silently skipped.
 * @{
 */
#define HL_FS_LIST_MAX_ENTRIES     65536u                /**< over -> "too_many_entries" */
#define HL_FS_LIST_MAX_NAME_BYTES  255u                  /**< NAME_MAX; over -> "name_too_long" */
#define HL_FS_LIST_MAX_TOTAL_BYTES (4u * 1024u * 1024u)  /**< sum of names; over -> "listing_too_large" */
/** @} */

/** @brief Stable metadata schema returned by @ref hl_cap_fs_stat. */
typedef struct HlFsStatInfo {
    HlFsNodeType type;
    uint64_t     size;    /**< bytes (<= #HL_FS_SIZE_MAX, else the op fails). */
    uint32_t     mode;    /**< permission bits (`st_mode & 0777`). */
    int64_t      mtime;   /**< modification time, epoch seconds. */
} HlFsStatInfo;

/** @brief One entry in a @ref hl_cap_fs_list result. */
typedef struct HlFsDirEntry {
    char        *name;    /**< NUL-terminated, allocated by the list allocator. */
    HlFsNodeType type;
    uint64_t     size;    /**< bytes (<= #HL_FS_SIZE_MAX). */
} HlFsDirEntry;

/**
 * @brief Stat a path's metadata (lstat semantics). Requires `fs.read` authority.
 *
 * Selects from the policy READ set, resolves the leaf's PARENT under the selected
 * anchor with the per-kind symlink rule (SUBTREE follows contained; EXACT / CREATE
 * / PATTERN refuse intermediate symlinks), then `fstatat(parent, leaf,
 * AT_SYMLINK_NOFOLLOW)` - the leaf is NEVER opened or followed.
 *
 * @param cfg      Filesystem config. NULL policy (no grants) -> `-1` "permission".
 * @param path     Relative path; selected + resolved through the policy.
 * @param out      Non-NULL; populated only on return 0.
 * @param err_msg  Out-parameter for a stable error token; may be NULL.
 *
 * @return Three-valued:
 *   - `0`  present: `*out` fully populated.
 *   - `1`  ABSENT: the authorized path does not exist. `*out` untouched, `*err_msg`
 *          NULL. (Bindings map this to `nil` / `null`; `stat(p) ~= nil` thus
 *          subsumes a separate `exists` probe.)
 *   - `-1` ERROR: `*err_msg` set (`"permission"`, `"invalid_path"`,
 *          `"symlink_denied"`, `"size_unrepresentable"`, `"io_error"`); `*out`
 *          untouched.
 */
int hl_cap_fs_stat(const HlFsConfig *cfg, const char *path,
                   HlFsStatInfo *out, const char **err_msg);

/**
 * @brief List a directory's immediate entries (non-recursive), deterministic order.
 *        Requires `fs.read` authority.
 *
 * Selects from the policy READ set: a SUBTREE grant lists any descendant directory
 * (all children); a single-terminal PATTERN grant lists its literal-prefix directory
 * exposing ONLY names matching the pattern; an EXACT / CREATE grant is not a listable
 * directory (denied). `.` and `..` are omitted. Each entry's type/size come from
 * `fstatat(dirfd, name, AT_SYMLINK_NOFOLLOW)` - a symlink child is reported as such,
 * never followed. One held directory fd is used throughout (O(1) open fds).
 *
 * ORDERING (documented, cross-platform stable): entries are sorted by `name` in
 * UNSIGNED-BYTE lexicographic order, shorter-prefix-first (`memcmp` over the shared
 * length; on a tie the shorter name precedes) - independent of locale and of the
 * platform `readdir()` order, so Linux / macOS / cosmo return byte-identical lists.
 *
 * OWNERSHIP + RETURN CONTRACT:
 *   - On success returns `0`, sets `*out_entries` (an array the CALLER owns, freed
 *     with @ref hl_cap_fs_list_free) and `*out_count`. An empty directory yields
 *     count 0 with `*out_entries == NULL`. Names + array are allocated via `alloc`
 *     (the tracked allocator; NULL = raw malloc), so the free accounts exactly.
 *   - On ANY failure returns `-1` with `*err_msg` set, `*out_entries == NULL`,
 *     `*out_count == 0`. NO PARTIAL RESULT IS EVER OBSERVABLE: a failure midway
 *     (a bound, a malformed / over-long entry, an allocation failure) frees every
 *     name and the array before returning.
 *   - Bounds each FAIL (never truncate): #HL_FS_LIST_MAX_ENTRIES
 *     (`"too_many_entries"`), #HL_FS_LIST_MAX_TOTAL_BYTES (`"listing_too_large"`),
 *     #HL_FS_LIST_MAX_NAME_BYTES (`"name_too_long"`), any `st_size` > #HL_FS_SIZE_MAX
 *     (`"size_unrepresentable"`).
 *
 * @return `0` on success, `-1` on failure. Stable tokens: `"permission"`,
 *         `"invalid_path"`, `"not_found"`, `"not_a_directory"`, `"symlink_denied"`,
 *         `"too_many_entries"`, `"listing_too_large"`, `"name_too_long"`,
 *         `"size_unrepresentable"`, `"io_error"`.
 */
int hl_cap_fs_list(const HlFsConfig *cfg, const char *path,
                   HlFsDirEntry **out_entries, size_t *out_count,
                   HlAllocator *alloc, const char **err_msg);

/**
 * @brief Free a list returned by @ref hl_cap_fs_list.
 *
 * Frees each entry's `name` then the array, via the SAME `alloc` passed to
 * @ref hl_cap_fs_list (NULL = raw malloc/free). NULL-safe; a count of 0 / NULL
 * array is a no-op. Discard the pointer after return.
 */
void hl_cap_fs_list_free(HlFsDirEntry *entries, size_t count, HlAllocator *alloc);

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
    void         *addr;   /**< Start of the caller-visible window. For a whole-file
                               mmap this equals @ref map_base; for a windowed mmap
                               it is @ref map_base plus the intra-page slop. */
    size_t        len;    /**< Caller-visible window length in bytes. */
    int           closed; /**< `1` iff already munmap'd; further `_munmap` is a no-op. */
    HlAllocator  *alloc;  /**< Tracked allocator (NULL = raw malloc). */
    int           borrow_count;  /**< Live zero-copy borrowers (e.g. images). */
    int           pending_free;  /**< close()/gc was deferred while borrowed. */
    /* Windowing (mapped-spans cut 1). For a whole-file mmap, @ref map_base ==
     * @ref addr, @ref map_len == @ref len, and @ref foffset == 0. For a windowed
     * mmap the mmap is page-aligned: @ref map_base / @ref map_len are the actual
     * `mmap`/`munmap` region (page-aligned base + page-rounded length), while
     * @ref addr / @ref len are the caller's requested sub-window inside it.
     * @ref munmap MUST use @ref map_base / @ref map_len, never @ref addr / @ref len. */
    void         *map_base;  /**< Page-aligned mmap base (the `munmap` address). */
    size_t        map_len;   /**< Page-rounded mmap length (the `munmap` length). */
    uint64_t      foffset;   /**< 64-bit logical file offset of the window's first byte. */
} HlMappedBuffer;

/**
 * @brief Upper bound on a single windowed mapping's LOGICAL length, in bytes
 *        (1 GiB).
 *
 * `hl_cap_fs_mmap_window` rejects a requested `length` above this. The cap is on
 * the caller's LOGICAL window length (the `length` argument), NOT on the
 * page-aligned mapped size: the actual `mmap` region (`map_len`) may exceed this
 * by up to the intra-page slop plus one page of rounding (`< 2 * page_size`).
 * Per the mapped-spans design (docs/wasm_mapped_spans_design.md §3.6) the SAME
 * conservative cap applies on wasm32 and Memory64; the per-invocation TOTAL
 * across multiple spans is a separate (not-yet-implemented) accounting layer.
 */
#define HL_FS_MMAP_MAX_WINDOW_BYTES ((uint64_t)1 << 30)

/**
 * @brief Pure, filesystem-free geometry for a page-aligned windowed mmap.
 *
 * Computes, with fully overflow-safe arithmetic, the `mmap` parameters for a
 * caller window `[offset, offset+length)` over a file of `file_size` bytes with
 * the given `page_size` (a power of two). The window is clamped to end-of-file
 * (a short final window is normal when scanning a large file in fixed windows).
 * No filesystem access, so overflow / boundary cases near `UINT64_MAX` are
 * directly unit-testable.
 *
 * @param offset        Caller's logical file offset of the window's first byte.
 * @param length        Caller's requested window length (bytes). Must be > 0.
 * @param file_size     Total file size in bytes.
 * @param page_size     System page size (power of two, > 0).
 * @param out_map_off   Out: page-aligned file offset to pass to `mmap`.
 * @param out_map_len   Out: page-rounded length to pass to `mmap` / `munmap`.
 * @param out_slop      Out: `offset - *out_map_off` (0 <= slop < page_size);
 *                      the caller window begins this many bytes into the mmap.
 * @param out_eff_len   Out: effective (EOF-clamped) caller window length.
 * @param err           Out: static reason string on failure; may be NULL.
 *
 * @return 0 on success; -1 on any out-of-range / overflow (with `*err` set):
 *         "bad_page_size", "empty_window", "window_too_large",
 *         "offset_past_eof", or "align_overflow".
 */
int hl_cap_fs_mmap_window_geometry(uint64_t offset, uint64_t length,
                                   uint64_t file_size, uint64_t page_size,
                                   uint64_t *out_map_off, uint64_t *out_map_len,
                                   uint64_t *out_slop, uint64_t *out_eff_len,
                                   const char **err);

/**
 * @brief Memory-map a fixed, page-aligned WINDOW of a file for read-only access.
 *
 * Like @ref hl_cap_fs_mmap but maps only `[offset, offset+length)` (clamped to
 * end-of-file). The alignment/overflow math is @ref hl_cap_fs_mmap_window_geometry;
 * the returned buffer's `addr`/`len` are the caller's window while `map_base`/
 * `map_len` hold the page-aligned mapping that teardown unmaps. `foffset` carries
 * the 64-bit logical position so a windowing caller knows where the window sits.
 *
 * @param cfg      Filesystem config.
 * @param path     Relative path; validated.
 * @param offset   Logical file offset of the window's first byte.
 * @param length   Requested window length in bytes (> 0, <= @ref
 *                 HL_FS_MMAP_MAX_WINDOW_BYTES); the effective length is clamped
 *                 to end-of-file.
 * @param alloc    Allocator for the wrapper struct. NULL = raw malloc.
 * @param err_msg  Out-parameter for error description; may be NULL.
 *
 * @return Heap-allocated @ref HlMappedBuffer (free via @ref hl_cap_fs_munmap),
 *         or NULL on failure.
 *
 * @note Truncation-after-map is the standard `mmap` limitation, NOT made safe
 *       here: if the underlying file is truncated (or otherwise shrunk) AFTER the
 *       window is mapped, touching a mapped page that is now wholly beyond the
 *       new end-of-file raises `SIGBUS` on access. This is identical to
 *       @ref hl_cap_fs_mmap and to POSIX `mmap` in general; the EOF clamp only
 *       bounds the window at map time, it cannot track later size changes.
 */
HlMappedBuffer *hl_cap_fs_mmap_window(const HlFsConfig *cfg, const char *path,
                                      uint64_t offset, uint64_t length,
                                      HlAllocator *alloc, const char **err_msg);

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
 * twice - sets `closed = 1` after the first call and the second is a
 * no-op.
 *
 * @param buf  Buffer to unmap. May be NULL (no-op).
 *
 * If the buffer still has live borrowers (`borrow_count > 0`), the actual
 * `munmap` + struct free is deferred: the call records `pending_free` and
 * returns, keeping the mapping valid for the borrowers. The last
 * @ref hl_cap_fs_mmap_release then completes the teardown.
 */
void hl_cap_fs_munmap(HlMappedBuffer *buf);

/**
 * @brief Register a zero-copy borrower of a mapped buffer.
 *
 * Increments `borrow_count` so a subsequent @ref hl_cap_fs_munmap (explicit
 * close or GC) defers the real unmap until the borrower releases. Event-loop
 * thread only; not atomic. @param buf May be NULL (no-op).
 */
void hl_cap_fs_mmap_borrow(HlMappedBuffer *buf);

/**
 * @brief Release a borrow taken with @ref hl_cap_fs_mmap_borrow.
 *
 * Decrements `borrow_count`; if it reaches 0 and a close was deferred
 * (`pending_free`), completes the `munmap` + struct free now. `void *` typed
 * so it can be used directly as an `HlImage::on_free` hook. NULL-safe.
 */
void hl_cap_fs_mmap_release(void *buf);

#endif /* HL_CAP_FS_H */
