/*
 * sh_seal_arena.h - Page-backed bump arena with one-way "seal" transition.
 *
 * Provenance: vendored into Hull from Keel v2.7.1
 * (vendor/sh_seal_arena/sh_seal_arena.{h,c} at that tag), MIT-licensed
 * (Copyright (c) 2026 Artalis; SPDX-License-Identifier: MIT, below).
 * Keel v3 no longer ships this primitive, so Hull carries its own copy
 * (sh_* family, alongside sh_arena / sh_json).
 *
 * Hull is now the SOLE consumer: hl_seal_arena builds on it to seal the
 * manifest, the VFS, wasm host_symbols, and CORS config (docs/security.md
 * section 4b). Keel's former uses of it (its sealed-request snapshot and
 * the kl_server_freeze route/middleware tables) were REMOVED in Keel
 * v2.8.0 and no longer apply (docs/security.md section 4e).
 *
 * Lifecycle:
 *
 *   sh_seal_arena_init    — reserve N pages RW, ready to allocate
 *   sh_seal_arena_alloc   — bump-allocate a block (alignment-aware)
 *   sh_seal_arena_strdup  — copy a NUL-terminated string
 *   sh_seal_arena_memdup  — copy raw bytes
 *   sh_seal_arena_seal    — mprotect(RO); after this, alloc fails and
 *                            writes to allocated blocks fault
 *   sh_seal_arena_reset   — mprotect(RW), used=0.  Cycles the arena
 *                            back to the init state for re-use.  ONLY
 *                            for arenas with deliberately ephemeral
 *                            lifecycle (per-request, per-connection)
 *                            owned by a single code path.  Boot-time
 *                            policy arenas must NEVER reset.
 *   sh_seal_arena_destroy — munmap; safe on sealed/unsealed arenas
 *
 * Sealing is intentionally one-way per arena instance under normal
 * lifecycle (boot-time policy).  `_reset` exists as an opt-in escape
 * hatch for arenas whose purpose is to be cycled (a per-request or
 * per-connection snapshot owned by a single code path).  If you find
 * yourself reaching for `_reset` on a policy arena, you almost
 * certainly want a fresh `_init` instead.
 *
 * Threading: NOT thread-safe during the init→alloc→seal lifecycle.
 * That's a feature — these operations are by-design single-threaded
 * boot-time activity.  AFTER sealing, the OS read-only mapping is
 * trivially thread-safe to read from any number of threads.
 *
 * Portability: pure POSIX (mmap + mprotect + sysconf).  Cosmopolitan
 * provides the same calls via its libc shim, so APE builds work
 * unmodified.  No Windows path today; a future native MSVC port
 * would add a #ifdef _WIN32 branch using VirtualAlloc/VirtualProtect.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SH_SEAL_ARENA_H
#define SH_SEAL_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout is exposed only so the struct can be embedded; do NOT poke
 * `sealed` or `used` from outside the API. */
typedef struct ShSealArena {
    void       *base;        /**< mmap'd base (page-aligned) */
    size_t      capacity;    /**< total bytes (multiple of page size) */
    size_t      used;        /**< current high-water (within capacity) */
    size_t      page_size;   /**< cached at init */
    int         sealed;      /**< 1 after sh_seal_arena_seal */
    const char *name;        /**< for diagnostics; caller-owned, NOT copied */
} ShSealArena;

/** Initialize an arena with at least @p size bytes of capacity.
 *  Actual capacity is rounded up to a multiple of the system page size,
 *  with a minimum of one page.  @p name is for logging only (not copied).
 *  Returns 0 on success, -1 on failure (mmap failed, NULL arena). */
int    sh_seal_arena_init(ShSealArena *arena, size_t size, const char *name);

/** Bump-allocate @p size bytes with @p align (power of two: 1/2/4/8/16).
 *  Returns NULL on failure (sealed, OOM, bad alignment, NULL arena). */
void  *sh_seal_arena_alloc(ShSealArena *arena, size_t size, size_t align);

/** Copy a NUL-terminated string into the arena, return pointer to copy.
 *  Returns NULL on failure. */
char  *sh_seal_arena_strdup(ShSealArena *arena, const char *s);

/** Copy @p len bytes (binary-safe, NOT NUL-terminated). NULL on failure. */
void  *sh_seal_arena_memdup(ShSealArena *arena, const void *src, size_t len);

/** Copy @p len bytes followed by a trailing '\0'.  Allocates len+1
 *  bytes, copies the data, appends NUL.  The result is C-string
 *  compatible (`strlen`/`strcmp`/`memcmp` all work) AND length-prefixed
 *  (the NUL is part of the arena allocation, not derived from input).
 *  Use when the source is parser-produced (header values, route param
 *  values, manifest entries) and downstream consumers expect a
 *  NUL-terminated read.  NULL on failure. */
void  *sh_seal_arena_memdup_nt(ShSealArena *arena, const void *src, size_t len);

/** Seal: mprotect(PROT_READ). After this:
 *    - sh_seal_arena_alloc returns NULL
 *    - reads from already-allocated blocks continue to work
 *    - writes to those blocks fault (SIGSEGV)
 *  Returns 0 on success, -1 on failure (treat as FATAL). */
int    sh_seal_arena_seal(ShSealArena *arena);

/** Reset: mprotect(PROT_READ|PROT_WRITE), used = 0, sealed = 0.
 *  Cycles the arena back to the state immediately after init.
 *
 *  ONLY for arenas with deliberately ephemeral lifecycle owned by a
 *  single code path: per-request, per-connection, etc.  After reset
 *  the previous allocations are still mapped at the same addresses
 *  but are now writable; the bump pointer rewinds to zero, so the
 *  next allocation reuses the same memory.  Any pointers callers
 *  cached into the previous fill are dangling — caller must drop
 *  them before reset.
 *
 *  Race window: between reset and the next seal, the pages are RW.
 *  Concurrent readers from another thread reading the old contents
 *  during this window would read torn data.  Single-threaded
 *  callsites (event loop per worker) are safe.
 *
 *  Returns 0 on success, -1 on failure (mprotect failed, NULL arena,
 *  arena never init'd).  Reset on an already-RW (unsealed) arena is
 *  a no-op success: bump pointer rewinds, no mprotect call. */
int    sh_seal_arena_reset(ShSealArena *arena);

/** Returns 1 if sealed, 0 otherwise (including for NULL). */
int    sh_seal_arena_is_sealed(const ShSealArena *arena);

/** Tear down: munmap. Safe on sealed/unsealed/NULL arenas. */
void   sh_seal_arena_destroy(ShSealArena *arena);

/** Diagnostic accessors. */
size_t sh_seal_arena_capacity(const ShSealArena *arena);
size_t sh_seal_arena_used(const ShSealArena *arena);
size_t sh_seal_arena_remaining(const ShSealArena *arena);

#ifdef __cplusplus
}
#endif

#endif /* SH_SEAL_ARENA_H */
