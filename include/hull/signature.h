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

/* ── Hardcoded gethull.dev platform public key ────────────────────── */
/*
 * Trust root for platform-layer signature verification (inner layer of
 * package.sig — see docs/security.md). The matching secret half lives
 * in two places only: the maintainer's ~/.hull/keys/platform.key (with
 * offline backup) and the HULL_PLATFORM_KEY GitHub Actions secret on
 * artalis-io/hull (set when sign-platform is wired into CI). Never in
 * the repo, never in any log.
 *
 * Distinct from HL_RELEASE_PUBKEY_HEX in include/hull/release.h: that
 * key signs hull.sha256 for `hull update`, this one signs the platform
 * .a library when an app is built with `hull build`. Separate keys so
 * one compromise doesn't taint the other.
 *
 * Override at runtime with `--platform-key <file>` for local
 * development against an unsigned platform library.
 */
/* Real gethull.dev platform Ed25519 public key (v0.1.3+).
 *
 * The matching secret half lives only in:
 *   - ~/.hull/keys/platform.key on the maintainer's machine (with
 *     offline backup), and
 *   - the HULL_PLATFORM_KEY GitHub Actions secret on artalis-io/hull,
 *     used by .github/workflows/release.yml's sign-platform-manifest
 *     job to sign the per-arch SHA-256 manifest of
 *     libhull_platform.a at release time.
 *
 * Forks override this at compile time:
 *   make CFLAGS_EXTRA=-DHL_PLATFORM_PUBKEY_HEX=\"<your hex>\"
 * to pin their own platform-signing key. The all-zeros sentinel still
 * works as an "I am a dev hull without a pinned key" signal — both
 * `hl_verify_startup` and `tool.platform_pubkey()` treat all-zeros as
 * "no pinned key, skip the gethull layer with a one-shot warning". */
#ifndef HL_PLATFORM_PUBKEY_HEX
#define HL_PLATFORM_PUBKEY_HEX \
    "2a5461235aa51bbbe1e9cbc462e6a63f37d099f5ad17646a8f3a67db2f3a4fad"
#endif

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

    /* v0.1.3: gethull.dev-signed manifest blob inherited from the
     * hull binary that built the app. The runtime verifier checks
     * `gethull_signature_hex` against `HL_PLATFORM_PUBKEY_HEX` over
     * the `gethull_manifest` bytes. NULL fields when the building
     * hull had no embedded blob (placeholder mode / dev hull). */
    const char *gethull_manifest;
    size_t      gethull_manifest_len;
    const char *gethull_signature_hex;
    size_t      gethull_signature_hex_len;
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
 * layers (including the v0.1.3 gethull platform-sig layer), then verifies
 * file hashes against embedded entries (build mode) or filesystem files
 * (dev mode, derived from the entry-point path).
 *
 * @param pubkey_path        Path to developer `.pub` file (64 hex chars).
 * @param entry_point        Path to the app entry point — used to locate sig.
 * @param app_vfs            App VFS for embedded entry lookup.
 * @param no_verify_platform If non-zero, skip the v0.1.3 gethull
 *                           platform-sig layer (`package.sig.platform.gethull`).
 *                           Pass 1 for dev hulls (no embedded manifest) or
 *                           forks signing with their own platform key. The
 *                           older `platform.{platforms,public_key,signature}`
 *                           layer is always checked when present (only the
 *                           gethull layer is gated by this flag).
 * @return 0 on success, -1 on any verification failure.
 */
int hl_verify_startup(const char *pubkey_path, const char *entry_point,
                      const HlVfs *app_vfs, int no_verify_platform);

#endif /* HL_SIGNATURE_H */
