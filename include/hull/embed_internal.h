/*
 * embed_internal.h — internal accessors for the embedding ABI.
 *
 * NOT part of the stable ABI (hull/embed.h). These exist only so Hull's
 * own tests can inspect an HlEmbed handle — in particular so the
 * fork+SIGSEGV death test can reach the sealed base_dir mapping and prove
 * it is read-only. External embedders must not rely on these symbols.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_EMBED_INTERNAL_H
#define HL_EMBED_INTERNAL_H

#include "hull/embed.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Effective filesystem base directory the handle resolves against. After
 * a successful hl_embed_seal() this points into the read-only sealed
 * arena; before seal it is the heap copy. NULL if @p e is NULL.
 */
const char *hl_embed_base_dir(const HlEmbed *e);

/* 1 if the handle has been successfully sealed, else 0. */
int hl_embed_is_sealed(const HlEmbed *e);

#ifdef __cplusplus
}
#endif

#endif /* HL_EMBED_INTERNAL_H */
