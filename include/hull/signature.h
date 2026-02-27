/*
 * signature.h — App signature verification (runtime)
 *
 * Reads package.sig (dual-layer), verifies Ed25519 signatures for both
 * the platform layer and the application layer, and checks file hashes.
 * Used by --verify-sig to refuse startup if the app has been tampered with.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SIGNATURE_H
#define HL_SIGNATURE_H

#include <stddef.h>
#include <stdint.h>

/* ── Hardcoded gethull.dev platform public key (from keys/gethull.dev.pub) ── */
/*
 * This is the trust root for platform signature verification.
 * Override at runtime with --platform-key <file>.
 */
#define HL_PLATFORM_PUBKEY_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"

/* ── Platform signature (inner layer) ─────────────────────────────── */

typedef struct {
    char *arch;         /* e.g. "x86_64-cosmo" */
    char *hash_hex;     /* SHA-256 of platform library */
    char *canary_hex;   /* SHA-256 of platform canary integrity */
} HlPlatformEntry;

typedef struct {
    HlPlatformEntry *entries;
    size_t entry_count;
    char *platforms_json;   /* raw JSON for the platforms object */
    char *signature_hex;    /* 128 hex chars */
    char *public_key_hex;   /* 64 hex chars */
} HlPlatformSig;

/* ── Parsed package.sig ───────────────────────────────────────────── */

typedef struct {
    char *name;       /* relative path (e.g. "app.lua") */
    char *hash_hex;   /* 64-char hex SHA-256 */
} HlSigFileEntry;

typedef struct {
    /* Application layer fields */
    char *binary_hash_hex;      /* SHA-256 of the APE binary */
    char *trampoline_hash_hex;  /* SHA-256 of app_main.c */
    char *build_json;           /* raw JSON for "build" object */
    char *files_json;           /* raw JSON for "files" object (canonical) */
    char *manifest_json;        /* raw JSON for "manifest" (may be "null") */
    char *signature_hex;        /* 128 hex chars — app developer's sig */
    char *public_key_hex;       /* 64 hex chars — app developer's pk */

    /* Platform layer (nested) */
    HlPlatformSig platform;

    /* Parsed file entries for hash checking */
    HlSigFileEntry *entries;
    size_t entry_count;
} HlSignature;

/*
 * Read and parse a package.sig file.
 * Returns 0 on success, -1 on error.
 */
int hl_sig_read(const char *sig_path, HlSignature *sig);

/*
 * Verify the Ed25519 app-layer signature.
 * Reconstructs the canonical payload:
 *   {binary_hash, build, files, manifest, platform, trampoline_hash}
 * `pubkey` is the 32-byte Ed25519 public key.
 * Returns 0 on success (valid), -1 on failure.
 */
int hl_sig_verify(const HlSignature *sig, const uint8_t pubkey[32]);

/*
 * Verify the Ed25519 platform-layer signature.
 * Reconstructs canonicalStringify(platforms).
 * `pubkey` is the 32-byte platform Ed25519 public key.
 * Returns 0 on success (valid), -1 on failure.
 */
int hl_sig_verify_platform(const HlSignature *sig, const uint8_t pubkey[32]);

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
 * Full startup verification: read pubkey, read package.sig, verify both
 * signature layers, verify file hashes (embedded or filesystem).
 *
 * `pubkey_path` — path to the developer .pub file (64 hex chars)
 * `entry_point` — path to the app entry point (used to derive sig location)
 *
 * Returns 0 on success, -1 on any verification failure.
 */
int hl_verify_startup(const char *pubkey_path, const char *entry_point);

#endif /* HL_SIGNATURE_H */
