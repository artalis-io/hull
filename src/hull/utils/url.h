/*
 * utils/url.h - URL percent-decoding (RFC 3986), the one home for it in C.
 *
 * A private, dependency-neutral leaf like utils/hex and utils/base64: no
 * Hull-domain knowledge, no allocation. It replaced five private decoders -
 * the Lua and JS request-query parsers and the Postgres, MySQL and Valkey DSN
 * parsers. Script code has its own single home, hull.encoding.url, and the two
 * follow the same rules.
 *
 * Deliberately NOT under include/hull/: an internal helper, not public API.
 * Consumers include it by relative path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_UTILS_URL_H
#define HULL_UTILS_URL_H

#include <stddef.h>

/*
 * Form mode (application/x-www-form-urlencoded, query strings): '+' is a
 * space, and a value with a malformed escape - a '%' not followed by two hex
 * digits - is kept as written (with '+' still read as a space) rather than
 * half-decoded. A query string is often not ours to reject.
 *
 * Without it (strict, for DSNs): '+' is itself, and a malformed escape fails.
 */
#define HL_URL_FORM 0x1u

/*
 * Decode `len` bytes at `src` into `dst`, NUL-terminated.
 *
 *   - `dst` may be `src`: decoding never lengthens the value, so it works in
 *     place.
 *   - `cap` must hold the decoded bytes plus the NUL; a value that does not
 *     fit fails. On failure `dst` is not written, so an in-place caller keeps
 *     its input intact.
 *   - `src` may be NULL only when `len == 0`.
 *
 * Returns the decoded length (excluding the NUL), or -1.
 */
long hl_url_decode(const char *src, size_t len, char *dst, size_t cap,
                   unsigned flags);

#endif /* HULL_UTILS_URL_H */
