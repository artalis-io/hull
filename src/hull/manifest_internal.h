/*
 * manifest_internal.h — Private helpers shared between manifest.c and
 *                       the per-runtime extractors (manifest_lua.c / _js.c).
 *
 * Not installed; internal to the manifest split (roadmap item G).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MANIFEST_INTERNAL_H
#define HL_MANIFEST_INTERNAL_H

#include "hull/manifest.h"

/* Reject CSP strings containing CR/LF (header injection guard).
 * Returns 1 if valid, 0 if rejected. */
int hl_manifest_csp_is_valid(const char *s);

/* Copy a string into the manifest's allocator. Returns NULL on failure. */
const char *hl_manifest_strdup(HlAllocator *alloc, const char *s);

/* Free a single manifest-owned string and NULL-out the pointer. */
void hl_manifest_str_free(HlAllocator *alloc, const char **sp);

#endif /* HL_MANIFEST_INTERNAL_H */
