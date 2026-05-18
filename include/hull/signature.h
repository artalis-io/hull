/**
 * @file signature.h
 * @brief Dual-layer Ed25519 signature verification for built apps.
 *
 * Hull packages carry a `package.sig` JSON file with two Ed25519 signatures:
 *
 *   - **Platform layer (inner):** signed by the Hull release/platform key.
 *     Proves `libhull_platform.a` is authentic for each architecture.
 *   - **App layer (outer):** signed by the application developer's key.
 *     Proves the app's source files (entry point, modules, templates,
 *     static assets, migrations, compute, shaders) have not been tampered
 *     with since `hull build`.
 *
 * @ref hl_verify_startup ties this together: read the developer pubkey,
 * parse `package.sig`, verify both signatures, then hash each file (either
 * embedded via #HlVfs or from disk in dev mode) and compare against
 * `files[<name>]`.
 *
 * A third (separate) signature lives in `hull.sha256.sig` for the `hull`
 * binary itself — see `release.h` and `commands/update.h`.
 *
 * @par Canonical payload:
 *   The signed message is the canonical-JSON serialization of
 *   `{binary_hash, build, files, manifest, platform, trampoline_hash}`
 *   for the app layer, and `canonicalStringify(platforms)` for the
 *   platform layer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SIGNATURE_H
#define HL_SIGNATURE_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlVfs HlVfs;
typedef struct SHArena SHArena;
typedef struct ShJsonValue ShJsonValue;

/* ── Hardcoded gethull.dev platform public key (from keys/gethull.dev.pub) ── */
/*
 * This is the trust root for platform signature verification.
 * Override at runtime with --platform-key <file>.
 */
#define HL_PLATFORM_PUBKEY_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"

/* ── Platform signature (inner layer) ─────────────────────────────── */

typedef struct {
    const char *arch;         /* e.g. "x86_64-cosmo" */
    const char *hash_hex;     /* SHA-256 of platform library */
    const char *canary_hex;   /* SHA-256 of platform canary integrity */
} HlPlatformEntry;

typedef struct {
    HlPlatformEntry *entries;
    size_t entry_count;
    ShJsonValue *platforms_value;  /* parsed DOM for canonical reconstruction */
    const char *signature_hex;    /* 128 hex chars */
    const char *public_key_hex;   /* 64 hex chars */
} HlPlatformSig;

/* ── Parsed package.sig ───────────────────────────────────────────── */

typedef struct {
    const char *name;       /* relative path (e.g. "app.lua") */
    const char *hash_hex;   /* 64-char hex SHA-256 */
} HlSigFileEntry;

typedef struct {
    SHArena *arena;  /* lifetime: all const char* and ShJsonValue* live here */

    /* Application layer fields */
    const char *binary_hash_hex;      /* SHA-256 of the APE binary */
    const char *trampoline_hash_hex;  /* SHA-256 of app_main.c */
    const char *signature_hex;        /* 128 hex chars — app developer's sig */
    const char *public_key_hex;       /* 64 hex chars — app developer's pk */

    /* Parsed DOM nodes for canonical JSON reconstruction in verify */
    ShJsonValue *build_value;            /* "build" object (or NULL) */
    ShJsonValue *files_value;            /* "files" object (required) */
    ShJsonValue *manifest_value;         /* "manifest" value (may be null/absent) */
    ShJsonValue *modules_resolved_value; /* "modules_resolved" array (may be null) */

    /* Platform layer (nested) */
    HlPlatformSig platform;

    /* Parsed file entries for hash checking */
    HlSigFileEntry *entries;
    size_t entry_count;
} HlSignature;

/**
 * @brief Read and parse a `package.sig` file.
 *
 * On success, all strings in `*sig` are arena-allocated and freed by
 * @ref hl_sig_free. On failure, `*sig` is left in a partially-initialized
 * state — call @ref hl_sig_free anyway (it's safe on partial state).
 *
 * @return 0 on success, -1 on read/parse error.
 */
int hl_sig_read(const char *sig_path, HlSignature *sig);

/**
 * @brief Verify the app-layer Ed25519 signature.
 *
 * Reconstructs the canonical JSON payload
 * `{binary_hash, build, files, manifest, platform, trampoline_hash}` and
 * verifies the signature against the developer's public key.
 *
 * @param sig    Parsed signature (from @ref hl_sig_read).
 * @param pubkey 32-byte Ed25519 public key.
 * @return 0 if valid, -1 if signature mismatch.
 */
int hl_sig_verify(const HlSignature *sig, const uint8_t pubkey[32]);

/**
 * @brief Verify the platform-layer Ed25519 signature.
 *
 * Reconstructs `canonicalStringify(platforms)` and verifies against the
 * platform pubkey (gethull.dev key or `--platform-key` override).
 *
 * @param sig    Parsed signature.
 * @param pubkey 32-byte Ed25519 public key.
 * @return 0 if valid, -1 if signature mismatch.
 */
int hl_sig_verify_platform(const HlSignature *sig, const uint8_t pubkey[32]);

/**
 * @brief Verify file hashes against embedded entries via VFS (build mode).
 * @return 0 if every `files[<name>]` matches the VFS entry, -1 on
 *   mismatch or missing entry.
 */
int hl_sig_verify_files_embedded(const HlSignature *sig, const HlVfs *vfs);

/**
 * @brief Verify file hashes against on-disk files in `app_dir` (dev mode).
 * @return 0 if every `files[<name>]` hashes to the recorded SHA-256,
 *   -1 on mismatch, missing file, or I/O error.
 */
int hl_sig_verify_files_fs(const HlSignature *sig, const char *app_dir);

/** @brief Free arena and reset signature. Idempotent. */
void hl_sig_free(HlSignature *sig);

/**
 * @brief Full startup verification path.
 *
 * Reads the developer pubkey, reads `package.sig`, verifies both signature
 * layers, then verifies file hashes against embedded entries (build mode)
 * or filesystem files (dev mode, derived from the entry-point path).
 *
 * @param pubkey_path Path to developer `.pub` file (64 hex chars).
 * @param entry_point Path to the app entry point — used to locate sig.
 * @param app_vfs     App VFS for embedded entry lookup.
 * @return 0 on success, -1 on any verification failure.
 */
int hl_verify_startup(const char *pubkey_path, const char *entry_point,
                      const HlVfs *app_vfs);

#endif /* HL_SIGNATURE_H */
