/*
 * utils/hex.h - lowercase hex encoding of a byte buffer.
 *
 * A private, dependency-neutral leaf: no Hull-domain knowledge, no crypto, no
 * allocation. It is the single home for the byte->hex-BUFFER transform that was
 * previously copied verbatim across signature.c, release.c, sbom.c,
 * blob_store.c, db_postgres.c, verify_self.c, and mod_tool.c (H1 / S2b - see
 * docs/h1_s2b_hex_ownership.md).
 *
 * Deliberately NOT under include/hull/: this is an internal helper, not part of
 * the public embedder API. Consumers include it by relative path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_UTILS_HEX_H
#define HULL_UTILS_HEX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Encode `in_len` bytes from `in` as lowercase hex into `out`.
 *
 * Bounded contract:
 *   - Input:  `in_len` bytes at `in`. `in` may be NULL only when `in_len == 0`.
 *   - Output: writes exactly `in_len * 2` lowercase hex characters followed by a
 *     single NUL terminator, i.e. `in_len * 2 + 1` bytes total.
 *   - Capacity: `out_cap` must be at least `in_len * 2 + 1`. On insufficient
 *     capacity the call fails and writes nothing beyond a defensive
 *     `out[0] = '\0'` (when `out_cap > 0`), so the destination is never left
 *     holding a partial, unterminated string.
 *   - Encoding: lowercase (`0-9a-f`).
 *   - Termination: the result is always NUL-terminated on success.
 *
 * Returns 0 on success, -1 on a NULL/zero-capacity destination, a NULL input
 * with a non-zero length, insufficient capacity, or overflow of the
 * `in_len * 2 + 1` size computation.
 */
int hl_hex_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

#endif /* HULL_UTILS_HEX_H */
