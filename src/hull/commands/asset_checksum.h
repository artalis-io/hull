/*
 * asset_checksum.h - PRIVATE helper for the installer commands (`hull feature
 * install` / `hull flavor install`).
 *
 * Deliberately NOT in include/hull/ (Hull's public C-header surface): the fixed
 * 64-byte width is an installer-INTERNAL detail, not an external API contract. A
 * public, unbounded-pointer, fixed-read primitive would let future callers violate
 * the "both buffers are 64-byte array-backed" precondition; keeping it private
 * confines that precondition to the two trusted callers that already satisfy it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_COMMANDS_ASSET_CHECKSUM_H
#define HULL_COMMANDS_ASSET_CHECKSUM_H

/*
 * Constant-time equality of two 64-char hex SHA-256 digests. The checksums are
 * PUBLIC values (a manifest entry vs a locally-computed digest), so the
 * constant-time form is belt-and-suspenders, matching the rest of Hull's hash
 * compares. The caller GUARANTEES both arguments are 64-byte array-backed buffers
 * already parsed from the release manifest / computed locally; exactly 64 bytes
 * are compared. Returns 1 if the 64 bytes are equal, 0 otherwise.
 */
int hl_asset_checksum_eq(const char a[64], const char b[64]);

#endif /* HULL_COMMANDS_ASSET_CHECKSUM_H */
