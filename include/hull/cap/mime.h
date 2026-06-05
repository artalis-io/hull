/**
 * @file cap/mime.h
 * @brief Content-MIME sniffer — magic-byte + shape inspection of a
 *        buffer's leading bytes.
 *
 * Used by the attachment subsystem for defense-in-depth: callers accept
 * a client-declared `Content-Type` on the fast path against a manifest
 * allowlist, then call hl_cap_mime_sniff() on the first ~4 KiB of the
 * stored bytes and record the sniffed result as truth-by-bytes. If the
 * sniffed MIME disagrees with the declared header on a stored part,
 * the storage layer's policy is "trust the bytes" — the declared
 * header is retained for audit (`declared_mime`) but `mime` is the
 * sniffed value.
 *
 * Coverage (in detection priority order):
 *   - image/png, image/jpeg, image/gif, image/webp  (binary magic)
 *   - application/pdf                                (binary magic)
 *   - image/svg+xml, text/html                       (text shape)
 *   - text/plain                                     (UTF-8 fallback)
 *
 * JSON / CSV are NOT distinguished from text/plain — they're valid
 * UTF-8 text by definition, so the fallback correctly classifies them
 * as text/plain. Callers that need finer JSON/CSV discrimination
 * should use the client-declared Content-Type instead.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_MIME_H
#define HL_CAP_MIME_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Sniff the MIME type of a buffer by inspecting its leading
 *        bytes.
 *
 * Reads at most the first `min(len, 4096)` bytes; never reads past
 * `len`. Returns a canonical MIME string drawn from a static table —
 * do NOT free the returned pointer.
 *
 * @param buf  Pointer to buffer bytes. May be NULL (returns NULL).
 * @param len  Buffer length. Zero returns NULL.
 *
 * @return Canonical MIME string on match, NULL otherwise. Match
 *         priority: binary-magic signatures first, then text-shape
 *         heuristics, then the UTF-8 plain-text fallback.
 */
const char *hl_cap_mime_sniff(const uint8_t *buf, size_t len);

#endif /* HL_CAP_MIME_H */
