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

/* ── Filesystem / blob calls: a floor per call (audit 37) ─────────────
 *
 * Some calls cost syscalls - an fsync, a rename, a readdir - whatever their
 * size, and a loop of them was free: an empty fs.write is a temp file, an
 * fsync, a rename and a directory fsync for 0 units (95,010 calls in 20 s
 * with no trip), and one fs.list on a directory of 60,000 files took ~43 ms
 * uncharged. Measured on macOS / APFS (arm64): the default 100M budget of a
 * plain Lua loop runs ~1.4 s; with the units below a 100M budget of a loop
 * of any one of these calls trips in 0.6-1.0 s - no longer than plain script
 * - while one call still costs a small fraction of the budget (100M buys
 * ~3000 empty writes or ~12000 blob.exists):
 *
 *   empty fs.write              ~200 us  ->  HL_WORK_FS_WRITE_UNITS  32768
 *   empty blob.put / finalize   ~170 us  ->  HL_WORK_FS_WRITE_UNITS  32768
 *   blob.init (3 mkdir_p, sweep)~125 us  ->  HL_WORK_BLOB_INIT_UNITS 16384
 *   blob.exists / size / atime   ~44 us  ->  HL_WORK_BLOB_STAT_UNITS  8192
 *   blob.delete                  ~42 us  ->  HL_WORK_BLOB_STAT_UNITS  8192
 *   fs.mmap + close              ~21 us  ->  HL_WORK_FS_MMAP_UNITS    4096
 *   fs.stat (2 components)      ~2.2 us  ->  256 + 64 per component
 *   fs.list, per entry read     ~0.6 us  ->  64 + name bytes / 64
 *   fs.list, per entry kept     ~1.5 us  ->  + HL_WORK_FS_LIST_STAT_UNITS 256
 *
 * Every floor is charged BEFORE the call, on top of the per-byte charge;
 * fs.list charges each directory entry as it reads it (hl_cap_fs_list_ex),
 * so a directory too large for the budget stops part-way. */
#define HL_WORK_FS_WRITE_UNITS       32768u
#define HL_WORK_BLOB_INIT_UNITS      16384u
#define HL_WORK_BLOB_STAT_UNITS       8192u
#define HL_WORK_FS_MMAP_UNITS         4096u
#define HL_WORK_FS_STAT_UNITS          256u
#define HL_WORK_FS_COMPONENT_UNITS      64u
#define HL_WORK_FS_LIST_UNITS         1024u   /* the open + policy select */
#define HL_WORK_FS_DIRENT_UNITS         64u
#define HL_WORK_FS_LIST_STAT_UNITS     256u

/* fs.stat: the floor plus one charge per path component resolved. */
static inline uint64_t hl_work_fs_stat_units(const char *path)
{
    uint64_t comps = 1;
    if (path)
        for (const char *p = path; *p; p++)
            if (*p == '/') comps++;
    return HL_WORK_FS_STAT_UNITS + comps * HL_WORK_FS_COMPONENT_UNITS;
}

/* One directory entry read (fs.list, the blob tmp/ sweep). */
static inline uint64_t hl_work_dirent_units(size_t name_len)
{
    return HL_WORK_FS_DIRENT_UNITS + (uint64_t)name_len / 64;
}

#endif /* HL_SHARED_WORK_CHARGE_H */
