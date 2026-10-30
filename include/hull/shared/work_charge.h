/*
 * hull/shared/work_charge.h - instruction-budget units for bulk binding work
 *
 * One rule for both runtimes (Lua lua_hlwork, JS hl_js_budget_charge): a
 * binding that copies or writes N bytes in one call charges N / 8 units
 * BEFORE it does the work, as fs.write / blob.put / the crypto digests do.
 * These helpers give the unit counts for the sends and reads audits 14 / 15
 * found uncharged, so the two runtimes cannot drift.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_WORK_CHARGE_H
#define HL_SHARED_WORK_CHARGE_H

#include <stddef.h>
#include <stdint.h>

/* Bytes copied, read or hashed in one call: one unit per 8. */
static inline uint64_t hl_work_bytes_units(uint64_t bytes)
{
    return bytes / 8;
}

/* Bytes a call walks or places outside the VM heap without copying them
 * into it (a tar header scan, a GPU buffer or texture create / upload):
 * one unit per 64, the allocator's rate. Charged up front, before the work
 * (audit 17). */
static inline uint64_t hl_work_bulk_units(uint64_t bytes)
{
    return bytes / 64;
}

/* One message of @p len bytes framed and queued to each of @p dests
 * connections (ws send = 1, ws.broadcast = the endpoint's connection count):
 * one unit per 8 bytes plus one per frame, per destination (audit 14). */
static inline uint64_t hl_work_send_units(size_t len, size_t dests)
{
    uint64_t per = (uint64_t)len / 8 + 1;
    if (dests && per > UINT64_MAX / dests) return UINT64_MAX;
    return per * (uint64_t)dests;
}

/* An SSE event / comment: Keel writes every line of @p text as its own field
 * (prefix, value and newline), so a text of newlines is several framing
 * bytes per byte. Since Keel 3.3.1 those pieces are batched (32 per HTTP
 * chunk, coalesced through a 4 KiB buffer into one transport write), so the
 * cost is the splitting and the framing, no longer a write per piece. One
 * unit per 8 bytes plus one per line - each CR or LF starts one (audit 14).
 * @p extra is the length of the event name and id, written as one field
 * each. */
static inline uint64_t hl_work_sse_units(const char *text, size_t len, size_t extra)
{
    uint64_t lines = 1;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n' || text[i] == '\r') lines++;
    return ((uint64_t)len + extra) / 8 + lines + 2;
}

/* ── Filesystem / blob calls (audits 37, 38) ──────────────────────────
 *
 * Some calls cost syscalls whatever their size, and a loop of them was free:
 * an empty fs.write was a temp file, an fsync, a rename and a directory fsync
 * for 0 units, and one fs.list on a directory of 60,000 files took ~43 ms
 * uncharged (audit 37). Path RESOLUTION was the rest (audit 38): fs.read of
 * an empty file charged 0 units for two resolutions, a 27-component path is
 * 26 directory opens, and a 250-deep fs.write mkdir-p'd 250 directories
 * (47 ms per call) under a flat floor. So the resolver charges its own work,
 * step by step, to the thread's charge scope (cap/fs_resolve.h), and the
 * per-call floors below cover only the fixed work that is not resolution.
 *
 * Rate: 64 units per microsecond of measured syscall time (~15.6 ns per
 * unit), so a default 100M budget spent on any one of these calls lasts
 * ~1.6 s - about twice a plain script loop of the same budget (0.7 s on the
 * reference machine), and never unbounded. Measured on macOS 26 / APFS,
 * arm64 (M-series), sandbox on; directory opens are what make macOS slow
 * (openat O_DIRECTORY + close ~12.6 us; fstatat ~1.0 us, readlinkat
 * ~0.9 us, mkdirat ~58 us, a file create ~60 us, rename ~75 us):
 *
 *   resolver, per resolution (checks, root dup, the
 *     call's policy select and audit)  ~1 us  -> HL_WORK_FS_RESOLVE_UNITS    64
 *   resolver, per component token ("." / ".." too)  -> HL_WORK_FS_NAME_UNITS   8
 *   resolver, per lookup (fstatat, or a failed
 *     leaf open)                       ~1 us  -> HL_WORK_FS_LOOKUP_UNITS     64
 *   resolver, directory opened (openat dir +
 *     fstat + close)                ~16-17 us -> HL_WORK_FS_STEP_UNITS     1024
 *   resolver, symlink expansion (readlinkat) ~1 us -> HL_WORK_FS_SYMLINK_UNITS 64
 *   resolver, directory created (mkdirat ~58 us beside
 *     siblings, ~150 us down a fresh chain)  -> HL_WORK_FS_MKDIR_UNITS     8192
 *   resolver, existing leaf opened (open +
 *     fstat + fcntl + close)           ~16 us -> HL_WORK_FS_LEAF_UNITS     1024
 *   resolver, leaf created / truncated ~60 us -> HL_WORK_FS_CREATE_UNITS   4096
 *
 *   fs.write beyond its resolutions
 *     (rename, fsyncs, fstatats)  ~125-200 us -> HL_WORK_FS_WRITE_UNITS  10240
 *   fs.mmap + munmap beyond the open  ~8 us  -> HL_WORK_FS_MMAP_UNITS      512
 *   fs.stat (fstatat, select)       ~2.3 us  -> HL_WORK_FS_STAT_UNITS      256
 *   fs.list beyond its resolution    ~4 us   -> HL_WORK_FS_LIST_UNITS      256
 *   fs.list, per entry read        ~0.6 us   -> 64 + name bytes / 64
 *   fs.list, per entry kept        ~1.5 us   -> + HL_WORK_FS_LIST_STAT_UNITS 256
 *
 * A flat fs.write is then 2 x 64 + 8 + 64 + 4096 + 10240 = 14536 units
 * (~6900 per 100M budget; ~3000 before audit 38's re-tune), an fs.read of
 * a small file 2 x 1160, a fs.stat of a shallow name 320. Deep, symlinked
 * and mkdir-p paths pay per step: a 27-component path ~29k units a
 * resolution, a 250-deep fs.write's mkdir-p ~2.6M units. Measured budget-sized loops of
 * each call (100M) on the reference machine all trip in 0.3-2.0 s; see the
 * audit 38 PR for the table.
 *
 * Blob calls do not go through the resolver (their store paths are built
 * by Hull), so their floors stay whole (audit 37's rate, ~190 units/us):
 *
 *   empty blob.put / finalize   ~170 us  ->  HL_WORK_BLOB_WRITE_UNITS 32768
 *   blob.init (3 mkdir_p, sweep)~125 us  ->  HL_WORK_BLOB_INIT_UNITS  16384
 *   blob.exists / size / atime   ~44 us  ->  HL_WORK_BLOB_STAT_UNITS   8192
 *   blob.delete                  ~42 us  ->  HL_WORK_BLOB_STAT_UNITS   8192
 *
 * Every floor is charged BEFORE the call, on top of the per-byte charge;
 * the resolver charges each step before its syscalls and stops the walk
 * ("budget_exhausted") once the run is over; fs.list charges each directory
 * entry as it reads it (hl_cap_fs_list_ex), so a directory too large for
 * the budget stops part-way. */
#define HL_WORK_FS_RESOLVE_UNITS        64u
#define HL_WORK_FS_NAME_UNITS            8u
#define HL_WORK_FS_LOOKUP_UNITS         64u
#define HL_WORK_FS_STEP_UNITS         1024u
#define HL_WORK_FS_SYMLINK_UNITS        64u
#define HL_WORK_FS_MKDIR_UNITS        8192u
#define HL_WORK_FS_LEAF_UNITS         1024u
#define HL_WORK_FS_CREATE_UNITS       4096u
#define HL_WORK_FS_WRITE_UNITS       10240u
#define HL_WORK_FS_MMAP_UNITS          512u
#define HL_WORK_FS_STAT_UNITS          256u
#define HL_WORK_FS_LIST_UNITS          256u   /* the policy select + fdopendir */
#define HL_WORK_FS_DIRENT_UNITS         64u
#define HL_WORK_FS_LIST_STAT_UNITS     256u
#define HL_WORK_BLOB_WRITE_UNITS     32768u
#define HL_WORK_BLOB_INIT_UNITS      16384u
#define HL_WORK_BLOB_STAT_UNITS       8192u

/* One directory entry read (fs.list, the blob tmp/ sweep). */
static inline uint64_t hl_work_dirent_units(size_t name_len)
{
    return HL_WORK_FS_DIRENT_UNITS + (uint64_t)name_len / 64;
}

#endif /* HL_SHARED_WORK_CHARGE_H */
