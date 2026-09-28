/*
 * utils/base64.h - base64 (RFC 4648) encode / decode over byte buffers.
 *
 * A private, dependency-neutral leaf like utils/hex: no Hull-domain knowledge,
 * no crypto, no allocation. It is the single home for base64 in Hull's C code,
 * which previously carried four copies - SMTP AUTH PLAIN, PostgreSQL SCRAM, the
 * terminal's OSC 52 clipboard write, and the base64url pair behind the old
 * script crypto bindings. (Script code has its own single home,
 * hull.encoding; see docs/encoding_consolidation_plan.md.)
 *
 * Deliberately NOT under include/hull/: this is an internal helper, not part of
 * the public embedder API. Consumers include it by relative path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_UTILS_BASE64_H
#define HULL_UTILS_BASE64_H

#include <stddef.h>
#include <stdint.h>

/* Flags. The default (0) is the standard alphabet (+/) with '=' padding. */
#define HL_BASE64_URL    0x1u   /* the url-safe alphabet (-_) */
#define HL_BASE64_NOPAD  0x2u   /* encode: omit '='; decode: refuse '=' */

/*
 * The number of characters encoding `in_len` bytes produces with `flags`,
 * NOT counting the terminator. Returns 0 (and so an unusable size) when the
 * computation would overflow; callers size buffers as this + 1.
 */
size_t hl_base64_encoded_len(size_t in_len, unsigned flags);

/*
 * Encode `in_len` bytes from `in` into `out`, NUL-terminated.
 *
 *   - `in` may be NULL only when `in_len == 0`.
 *   - `out_cap` must be at least hl_base64_encoded_len(in_len, flags) + 1.
 *     On insufficient capacity nothing is written beyond a defensive
 *     `out[0] = '\0'` (when `out_cap > 0`).
 *
 * Returns the encoded length (excluding the terminator), or -1.
 */
int hl_base64_encode(const void *in, size_t in_len, char *out, size_t out_cap,
                     unsigned flags);

/*
 * Decode `in_len` characters from `in` into `out`. Strict:
 *
 *   - every character must be in the selected alphabet - no whitespace, and
 *     no character of the other alphabet;
 *   - '=' padding is optional but, when present, must be exactly right and
 *     only at the end; with HL_BASE64_NOPAD any '=' is refused;
 *   - a length that cannot come from any input (one character left over) is
 *     refused;
 *   - the unused low bits of the last character must be zero, so a value has
 *     exactly one encoding.
 *
 * `*out_len` receives the decoded length on success. Returns 0, or -1 on
 * malformed input or when `out_cap` is too small (the decoded length of `n`
 * characters is at most n * 3 / 4).
 */
int hl_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                     size_t *out_len, unsigned flags);

#endif /* HULL_UTILS_BASE64_H */
