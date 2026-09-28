/*
 * utils/hex.h - lowercase hex encoding of a byte buffer, and decoding back.
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

/*
 * Decode `hex_len` hex characters (either case, no NUL needed) into `out`.
 *
 *   - `hex_len` must be even and every character a hex digit.
 *   - `out_cap` must be at least `hex_len / 2`.
 *   - `hex` may be NULL only when `hex_len == 0` (which decodes to nothing).
 *
 * Returns the number of bytes written (`hex_len / 2`), or -1 on an odd
 * length, a non-hex character, insufficient capacity, or a NULL argument.
 */
int hl_hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap);

/*
 * The value (0-15) of one hex digit, either case, or -1 for anything else.
 * For parsers that meet hex one escape at a time (the DSN percent-decoders);
 * pass a char as (unsigned char) so a byte >= 0x80 is not sign-extended.
 */
int hl_hex_digit(int c);

#endif /* HULL_UTILS_HEX_H */
