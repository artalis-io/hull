/*
 * commands/verify_self.c. `hull verify-self` subcommand.
 *
 * Verify the running hull binary against a release manifest. Single
 * subcommand replaces the three-step manual flow of:
 *   sha256sum hull-linux-x86_64    # compute self-hash
 *   grep hull-linux-x86_64 hull.sha256    # find expected hash
 *   hull verify-release hull.sha256 hull.sha256.sig    # verify sig
 *
 * Usage:
 *   hull verify-self                                . manifest + sig in cwd
 *                                                     or alongside argv[0]
 *   hull verify-self --manifest PATH --signature PATH
 *   hull verify-self --asset NAME                    . override platform asset
 *   hull verify-self --pubkey HEX                    . override embedded key
 *
 * Exit 0 = binary is the one signed in the manifest.
 * Exit 1 = mismatch or any verification step failed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/verify_self.h"
#include "hull/release.h"
#include "hull/release_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HL_SHA256_HEX_BUF 65U
#define HL_VERIFY_SELF_PATH_MAX 4096U
/* Cap on the binary we'll hash for verify-self. Hull binaries are ~5-15 MB
 * across all build flavors; 256 MiB is well past any plausible value and
 * guards against accidentally hashing a giant file if --asset / argv[0]
 * is mis-targeted. */
#define HL_VERIFY_SELF_BINARY_MAX_BYTES (256U * 1024U * 1024U)

#include "mbedtls/sha256.h"

static void usage(FILE *fp)
{
    fprintf(fp,
        "Usage: hull verify-self [options]\n"
        "\n"
        "Verify the running hull binary against an Ed25519-signed release\n"
        "manifest (hull.sha256 + hull.sha256.sig).\n"
        "\n"
        "Options:\n"
        "  --manifest PATH   Manifest file (default: hull.sha256 next to argv[0],\n"
        "                    then cwd).\n"
        "  --signature PATH  Signature file (default: <manifest>.sig).\n"
        "  --asset NAME      Asset name in the manifest to compare against\n"
        "                    (default: derived from this binary's platform,\n"
        "                    e.g. \"hull-linux-x86_64\").\n"
        "  --pubkey HEX      Override the embedded release public key\n"
        "                    (64 hex chars).\n"
        "\n"
        "Exit 0 = binary matches the signed manifest entry.\n"
        "Exit 1 = mismatch or any verification step failed.\n");
}

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode_pk(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return -1;
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int read_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return -1; }
    buf[got] = '\0';
    *out_buf = buf;
    *out_len = got;
    return 0;
}

/* SHA-256 of the file at @p path -> hex. Uses one-shot mbedtls_sha256
 * (same primitive as release_io's sha256_hex; MSan-verified). Reads the
 * whole file into memory rather than streaming because MSan-instrumented
 * mbedTLS misreports uninitialized context state through the
 * _starts/_update/_finish API (see src/hull/sbom.c §0.3.11 fix for the
 * same pattern). Hull binaries are bounded at ~15 MiB so the whole-file
 * malloc is trivially OK. The MAX_BYTES cap guards mis-targeted paths. */
static int sha256_file_hex(const char *path, char hex_out[HL_SHA256_HEX_BUF])
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long sz = ftell(fp);
    if (sz < 0 || (size_t)sz > HL_VERIFY_SELF_BINARY_MAX_BYTES) {
        fclose(fp); return -1;
    }
    rewind(fp);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(buf); return -1; }

    unsigned char digest[32];
    int rc = mbedtls_sha256(buf, got, digest, 0);
    free(buf);
    if (rc != 0) return -1;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i*2]   = hex[(digest[i] >> 4) & 0xf];
        hex_out[i*2+1] = hex[digest[i] & 0xf];
    }
    hex_out[64] = '\0';
    return 0;
}

/* Resolve the running binary's filesystem path: prefer the kernel-provided
 * /proc/self/exe (Linux) or _NSGetExecutablePath (macOS); fall back to
 * argv[0] from env->hull_exe. The latter works if the user ran ./hull or
 * /usr/local/bin/hull but fails for bare-name PATH lookup ("hull"). */
static int resolve_self_path(const HlCommandEnv *env, char *out, size_t out_sz)
{
    if (hl_release_io_self_path(out, out_sz) == 0) return 0;
    if (env && env->hull_exe) {
        size_t len = strlen(env->hull_exe);
        if (len + 1 > out_sz) return -1;
        memcpy(out, env->hull_exe, len + 1);
        return 0;
    }
    return -1;
}

/* Default manifest discovery: try <dir-of-self>/hull.sha256 first,
 * then ./hull.sha256. The first that exists wins. */
static int discover_manifest(const char *self_path, char *out, size_t out_sz)
{
    /* Try directory of self_path */
    char dir[HL_VERIFY_SELF_PATH_MAX];
    size_t len = strlen(self_path);
    if (len >= sizeof(dir)) return -1;
    memcpy(dir, self_path, len + 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        int n = snprintf(out, out_sz, "%s/hull.sha256", dir);
        if (n > 0 && (size_t)n < out_sz) {
            FILE *probe = fopen(out, "rb");
            if (probe) { fclose(probe); return 0; }
        }
    }
    /* Fall back to cwd */
    int n = snprintf(out, out_sz, "hull.sha256");
    if (n <= 0 || (size_t)n >= out_sz) return -1;
    FILE *probe = fopen(out, "rb");
    if (probe) { fclose(probe); return 0; }
    return -1;
}

int hl_cmd_verify_self(int argc, char **argv, const HlCommandEnv *env)
{
    const char *manifest_arg  = NULL;
    const char *signature_arg = NULL;
    const char *asset_arg     = NULL;
    const char *pubkey_hex    = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout); return 0;
        }
        if (strcmp(a, "--manifest") == 0 && i + 1 < argc) {
            manifest_arg = argv[++i];
        } else if (strncmp(a, "--manifest=", 11) == 0) {
            manifest_arg = a + 11;
        } else if (strcmp(a, "--signature") == 0 && i + 1 < argc) {
            signature_arg = argv[++i];
        } else if (strncmp(a, "--signature=", 12) == 0) {
            signature_arg = a + 12;
        } else if (strcmp(a, "--asset") == 0 && i + 1 < argc) {
            asset_arg = argv[++i];
        } else if (strncmp(a, "--asset=", 8) == 0) {
            asset_arg = a + 8;
        } else if (strcmp(a, "--pubkey") == 0 && i + 1 < argc) {
            pubkey_hex = argv[++i];
        } else if (strncmp(a, "--pubkey=", 9) == 0) {
            pubkey_hex = a + 9;
        } else {
            fprintf(stderr, "hull verify-self: unknown argument: %s\n", a);
            usage(stderr); return 1;
        }
    }

    /* Resolve the running binary path. */
    char self_path[HL_VERIFY_SELF_PATH_MAX];
    if (resolve_self_path(env, self_path, sizeof(self_path)) != 0) {
        fprintf(stderr, "hull verify-self: cannot resolve own binary path\n");
        return 1;
    }

    /* Default asset name: <derived-from-platform>. e.g. "hull-linux-x86_64". */
    char default_asset[64];
    if (!asset_arg) {
        const char *plat = hl_release_io_platform();
        int n = snprintf(default_asset, sizeof(default_asset), "hull-%s", plat);
        if (n <= 0 || (size_t)n >= sizeof(default_asset)) {
            fprintf(stderr, "hull verify-self: cannot derive platform asset name\n");
            return 1;
        }
        asset_arg = default_asset;
    }

    /* Default manifest: discover next to self, then cwd. */
    char default_manifest[HL_VERIFY_SELF_PATH_MAX];
    if (!manifest_arg) {
        if (discover_manifest(self_path, default_manifest,
                              sizeof(default_manifest)) != 0) {
            fprintf(stderr,
                "hull verify-self: no manifest found.\n"
                "  Looked for: <dir-of-self>/hull.sha256 and ./hull.sha256\n"
                "  Pass --manifest PATH explicitly, or download hull.sha256\n"
                "  from the GitHub release matching this binary's version.\n");
            return 1;
        }
        manifest_arg = default_manifest;
    }

    /* Default signature: <manifest>.sig */
    char default_sig[HL_VERIFY_SELF_PATH_MAX];
    if (!signature_arg) {
        int n = snprintf(default_sig, sizeof(default_sig),
                         "%s.sig", manifest_arg);
        if (n <= 0 || (size_t)n >= sizeof(default_sig)) {
            fprintf(stderr, "hull verify-self: signature path too long\n");
            return 1;
        }
        signature_arg = default_sig;
    }

    /* Resolve pubkey: --pubkey override or embedded. */
    uint8_t pubkey_buf[32];
    const uint8_t *pubkey = NULL;
    if (pubkey_hex) {
        if (hex_decode_pk(pubkey_hex, pubkey_buf) != 0) {
            fprintf(stderr, "hull verify-self: --pubkey must be 64 hex chars\n");
            return 1;
        }
        pubkey = pubkey_buf;
    } else if (!hl_release_pubkey_configured()) {
        fprintf(stderr,
            "hull verify-self: this build has no embedded release public key\n"
            "                  (HL_RELEASE_PUBKEY_HEX is the placeholder).\n"
            "                  Pass --pubkey <hex> to verify against a specific key.\n");
        return 1;
    }

    /* Step 1: read manifest + signature. */
    char *manifest = NULL, *sig = NULL;
    size_t manifest_len = 0, sig_len = 0;
    if (read_file(manifest_arg, &manifest, &manifest_len) != 0) {
        fprintf(stderr, "hull verify-self: cannot open manifest: %s\n", manifest_arg);
        return 1;
    }
    if (read_file(signature_arg, &sig, &sig_len) != 0) {
        fprintf(stderr, "hull verify-self: cannot open signature: %s\n", signature_arg);
        free(manifest);
        return 1;
    }

    /* Step 2: verify signature over manifest. */
    if (hl_release_verify_manifest_sig(manifest, manifest_len,
                                       sig, sig_len, pubkey) != 0) {
        fprintf(stderr, "hull verify-self: INVALID manifest signature\n");
        free(manifest); free(sig);
        return 1;
    }

    /* Step 3: extract expected hash for this asset from the manifest. */
    char expected_hex[HL_SHA256_HEX_BUF];
    if (hl_release_io_find_checksum(manifest, manifest_len,
                                    asset_arg, expected_hex) != 0) {
        fprintf(stderr, "hull verify-self: asset '%s' not in manifest\n", asset_arg);
        free(manifest); free(sig);
        return 1;
    }

    /* Step 4: compute SHA-256 of the running binary. */
    char actual_hex[HL_SHA256_HEX_BUF];
    if (sha256_file_hex(self_path, actual_hex) != 0) {
        fprintf(stderr, "hull verify-self: cannot hash %s\n", self_path);
        free(manifest); free(sig);
        return 1;
    }

    free(manifest); free(sig);

    /* Step 5: constant-time compare. */
    unsigned diff = 0;
    for (size_t i = 0; i < 64; i++) {
        diff |= (unsigned)(expected_hex[i] ^ actual_hex[i]);
    }
    if (diff != 0) {
        fprintf(stderr,
            "hull verify-self: MISMATCH\n"
            "  asset:    %s\n"
            "  path:     %s\n"
            "  expected: %s\n"
            "  actual:   %s\n"
            "  The running binary differs from the one signed in the manifest.\n"
            "  Either it was tampered with, or the manifest is from a different release.\n",
            asset_arg, self_path, expected_hex, actual_hex);
        return 1;
    }

    printf("hull verify-self: OK\n"
           "  asset:  %s\n"
           "  path:   %s\n"
           "  sha256: %s\n",
           asset_arg, self_path, actual_hex);
    return 0;
}
