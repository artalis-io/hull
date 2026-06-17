/*
 * seal_arena.h — Page-backed bump arena with one-way "seal" transition.
 *
 * Hull's compile-time dispatch tables (HlModuleSpec[], HlCspPreset[],
 * HlCmd[], all backend vtables) are already `static const` and live in
 * `.rodata`, which the OS protects as read-only at the page level. This
 * arena is for the OTHER class of security-sensitive data: structures
 * built at BOOT TIME from manifest / config / capability resolution that
 * are then READ-ONLY for the rest of the process lifetime, but currently
 * live in writable malloc'd memory.
 *
 * Canonical example: the per-app HlManifest — its fs_read[]/fs_write[]/
 * hosts[]/env[] allowlists are populated from `app.manifest({...})` once
 * at boot, then read on every request by the capability layer. An
 * attacker with a linear-write memory-corruption bug on the heap could
 * rewrite those allowlists to expand the app's capabilities (add
 * "/etc/shadow" to fs_read, add their C2 host to hosts, etc.). Backing
 * the manifest by an arena that's mprotect()'d read-only after
 * extraction turns that into a SIGSEGV instead.
 *
 * Lifecycle:
 *
 *   hl_seal_arena_init    — reserve N pages RW, ready to allocate
 *   hl_seal_arena_alloc   — bump-allocate a block (alignment-aware)
 *   hl_seal_arena_strdup  — convenience: copy a NUL-terminated string
 *   hl_seal_arena_seal    — mprotect(RO); after this, alloc fails and
 *                            writes to allocated blocks fault
 *   hl_seal_arena_destroy — munmap; safe on sealed or unsealed arenas
 *
 * After sealing the arena is one-way: there is NO "unseal" — that would
 * defeat the purpose. To rebuild, init a fresh arena into a different
 * pointer. The arena struct itself stays mutable (the bookkeeping is not
 * the threat model — overwriting `used` doesn't grant any capability,
 * because no further allocations happen after sealing anyway).
 *
 * Threading: the arena is NOT thread-safe during the init→alloc→seal
 * lifecycle. That's a feature — these operations are by-design single-
 * threaded boot-time activity. AFTER sealing, the OS read-only mapping
 * is trivially thread-safe to read from any number of threads.
 *
 * Portability: pure POSIX (mmap + mprotect). Cosmopolitan provides the
 * same calls via its libc shim, so APE builds work transparently. No
 * Windows path today; if Hull ever ships a native MSVC build, add a
 * `#ifdef _WIN32` branch with VirtualAlloc/VirtualProtect.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SEAL_ARENA_H
#define HL_SEAL_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque-ish: callers may stack-allocate but treat fields as private.
 * Layout is exposed only so the struct can be embedded; do NOT poke
 * `sealed` or `used` from outside the API (the seal flag is checked
 * AND the OS will fault, so a manual unseal isn't possible either way). */
typedef struct HlSealArena {
    void       *base;        /* mmap'd base (page-aligned) */
    size_t      capacity;    /* total bytes (multiple of page size) */
    size_t      used;        /* current high-water (within capacity) */
    size_t      page_size;   /* cached at init */
    int         sealed;      /* 1 after hl_seal_arena_seal */
    const char *name;        /* for diagnostics; not duplicated, caller-owned */
} HlSealArena;

/* Initialize an arena with at least `size` bytes of capacity.
 * Actual capacity is rounded up to a multiple of the system page size,
 * with a minimum of one page. `name` is for logging; the arena stores
 * the pointer (does not copy) — pass a string literal or a buffer that
 * outlives the arena.
 *
 * Returns 0 on success, -1 on failure (mmap failed, size overflow,
 * NULL arena). On failure the arena is left zero-initialized and
 * hl_seal_arena_destroy is a no-op. */
int hl_seal_arena_init(HlSealArena *arena, size_t size, const char *name);

/* Bump-allocate `size` bytes with the given alignment (must be a power
 * of two, typically 1/2/4/8/16). Returns a pointer into the arena, or
 * NULL on failure (sealed, out of space, bad alignment, size overflow,
 * NULL arena). The returned memory is uninitialized.
 *
 * The arena itself stays mutable during init→seal so this call writes
 * `used` and returns a writable pointer. The CALLER then writes the
 * actual data into the returned block before sealing. */
void *hl_seal_arena_alloc(HlSealArena *arena, size_t size, size_t align);

/* Convenience: copy a NUL-terminated string into the arena and return
 * a pointer to the copy. Returns NULL on failure (same conditions as
 * hl_seal_arena_alloc, plus NULL string). The copy IS NUL-terminated. */
char *hl_seal_arena_strdup(HlSealArena *arena, const char *s);

/* Convenience: copy `len` bytes into the arena (binary-safe, NOT
 * NUL-terminated). Returns NULL on failure. */
void *hl_seal_arena_memdup(HlSealArena *arena, const void *src, size_t len);

/* Seal the arena: switch the backing mapping to read-only via
 * mprotect(PROT_READ). After this returns 0:
 *   - hl_seal_arena_alloc returns NULL
 *   - reads from already-allocated blocks continue to work
 *   - writes to those blocks fault (SIGSEGV)
 *
 * Returns 0 on success, -1 on failure (mprotect failed or arena is
 * already sealed). Sealing failure should be treated as FATAL by the
 * caller — proceeding with a half-sealed arena defeats the threat
 * model. */
int hl_seal_arena_seal(HlSealArena *arena);

/* Returns 1 if the arena is sealed, 0 otherwise (including for NULL).
 * Cheap, safe to call from any thread after init returns. */
int hl_seal_arena_is_sealed(const HlSealArena *arena);

/* Tear down the arena: munmap the backing memory. Safe on both sealed
 * and unsealed arenas; the kernel handles the unmapping regardless of
 * the current protection. After destroy the struct is zero-initialized
 * and may be re-init'd. NULL arena is a no-op. */
void hl_seal_arena_destroy(HlSealArena *arena);

/* Diagnostic accessors — read the bookkeeping fields. */
size_t hl_seal_arena_capacity(const HlSealArena *arena);
size_t hl_seal_arena_used(const HlSealArena *arena);
size_t hl_seal_arena_remaining(const HlSealArena *arena);

#ifdef __cplusplus
}
#endif

#endif /* HL_SEAL_ARENA_H */
