/*
 * seal_dup.h - copy a struct (or an array of them) into a sealed arena at
 * its type's alignment.
 *
 * sh_seal_arena_memdup allocates at alignment 1: right for bytes, wrong for
 * a struct. After a strdup the bump pointer sits at any offset, so a struct
 * copied next landed misaligned (UBSan: "load of misaligned address" on the
 * sealed HlEnvConfig) - tolerated by arm64 / x86_64, a fault on a strict-
 * alignment target. Copy structs with HL_SEAL_DUP; keep memdup for bytes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SEAL_DUP_H
#define HL_SEAL_DUP_H

#include <sh_seal_arena.h>

#include <stddef.h>
#include <string.h>

static inline void *hl_seal_dup(ShSealArena *arena, const void *src,
                                size_t len, size_t align)
{
    void *dst = sh_seal_arena_alloc(arena, len, align);
    if (dst) memcpy(dst, src, len);
    return dst;
}

/* One T (src points at a T), at T's alignment. */
#define HL_SEAL_DUP(arena, src, T) \
    ((T *)hl_seal_dup((arena), (src), sizeof(T), _Alignof(T)))

#endif /* HL_SEAL_DUP_H */
