/*
 * signature.h — App signature verification (runtime)
 *
 * Reads hull.sig, verifies Ed25519 signature, and checks file hashes.
 * Used by --verify-sig to refuse startup if the app has been tampered with.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SIGNATURE_H
#define HL_SIGNATURE_H

#include <stddef.h>
#include <stdint.h>

/* ── Parsed hull.sig ──────────────────────────────────────────────── */

typedef struct {
    char *name;       /* relative path (e.g. "app.lua") */
    char *hash_hex;   /* 64-char hex SHA-256 */
} HlSigFileEntry;

typedef struct {
    char *files_json;       /* raw JSON for "files" object (canonical) */
    char *manifest_json;    /* raw JSON for "manifest" object (canonical, may be "null") */
    char *signature_hex;    /* 128 hex chars */
    char *public_key_hex;   /* 64 hex chars */
    int   version;

    /* Parsed file entries for hash checking */
    HlSigFileEntry *entries;
    size_t entry_count;
} HlSignature;

/*
 * Read and parse a hull.sig file.
 * Returns 0 on success, -1 on error.
 */
int hl_sig_read(const char *sig_path, HlSignature *sig);

/*
 * Verify the Ed25519 signature over {files, manifest}.
 * `pubkey` is the 32-byte Ed25519 public key.
 * Returns 0 on success (valid), -1 on failure.
 */
int hl_sig_verify(const HlSignature *sig, const uint8_t pubkey[32]);

/*
 * Verify file hashes against embedded app entries (hl_app_lua_entries[]).
 * Returns 0 if all files match, -1 on mismatch or missing files.
 */
int hl_sig_verify_files_embedded(const HlSignature *sig);

/*
 * Verify file hashes against filesystem files in app_dir.
 * Returns 0 if all files match, -1 on mismatch or missing files.
 */
int hl_sig_verify_files_fs(const HlSignature *sig, const char *app_dir);

/*
 * Free all resources in an HlSignature.
 */
void hl_sig_free(HlSignature *sig);

/*
 * Full startup verification: read pubkey, read hull.sig, verify signature,
 * verify file hashes (embedded or filesystem).
 *
 * `pubkey_path` — path to the .pub file (64 hex chars)
 * `entry_point` — path to the app entry point (used to derive hull.sig location)
 *
 * Returns 0 on success, -1 on any verification failure.
 */
int hl_verify_startup(const char *pubkey_path, const char *entry_point);

#endif /* HL_SIGNATURE_H */
