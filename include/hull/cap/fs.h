/*
 * cap/fs.h — Filesystem capability with path validation
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_FS_H
#define HL_CAP_FS_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlFsConfig {
    const char *base_dir;
    size_t      base_len;
} HlFsConfig;

int hl_cap_fs_validate(const HlFsConfig *cfg, const char *path);

int64_t hl_cap_fs_read(const HlFsConfig *cfg, const char *path,
                         char *buf, size_t buf_size);

int hl_cap_fs_write(const HlFsConfig *cfg, const char *path,
                      const char *data, size_t len);

int hl_cap_fs_exists(const HlFsConfig *cfg, const char *path);

int hl_cap_fs_delete(const HlFsConfig *cfg, const char *path);

/* ── Memory-mapped file buffer ─────────────────────────────────────── */

typedef struct HlMappedBuffer {
    void   *addr;       /* mmap'd region (PROT_READ, MAP_PRIVATE) */
    size_t  len;        /* mapping length */
    int     closed;     /* 1 = already munmap'd */
} HlMappedBuffer;

/* Memory-map a file for read-only access. Path is validated via hl_cap_fs_validate().
 * Returns heap-allocated HlMappedBuffer on success, NULL on failure.
 * Caller must call hl_cap_fs_munmap() when done. */
HlMappedBuffer *hl_cap_fs_mmap(const HlFsConfig *cfg, const char *path);

/* Unmap and free a mapped buffer. Safe to call multiple times (idempotent). */
void hl_cap_fs_munmap(HlMappedBuffer *buf);

#endif /* HL_CAP_FS_H */
