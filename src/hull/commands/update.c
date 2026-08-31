/*
 * commands/update.c - hull self-update
 *
 * Downloads the latest GitHub release for this binary's OS/arch, verifies
 * SHA-256 against hull.sha256 from the same release, and atomically
 * replaces the running binary via rename(2).
 *
 *   hull update                - install latest if newer than current
 *   hull update --check        - print "update available" / "up to date"
 *   hull update --force        - reinstall current version
 *   hull update --repo=org/name - override the GitHub repo
 *
 * No external dependencies - uses keel's HTTPS client + the embedded
 * Mozilla CA bundle (D4) for trust.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/update.h"
#include "hull/release.h"
#include "hull/release_io.h"

#include <keel/allocator.h>
#include <keel_tls_mbedtls.h>
#include <mbedtls/constant_time.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

#define HL_DEFAULT_REPO "artalis-io/hull"

/* All HTTPS / manifest / atomic-write plumbing now lives in
 * hull/release_io.h - shared with `hull tools install`. */

/* ── Main handler ────────────────────────────────────────────────── */

int hl_cmd_update(int argc, char **argv, const HlCommandEnv *env)
{
    int check_only = 0;
    int force = 0;
    const char *repo = HL_DEFAULT_REPO;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check_only = 1;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strncmp(argv[i], "--repo=", 7) == 0) {
            repo = argv[i] + 7;
        } else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stdout,
                "Usage: hull update [options]\n"
                "\n"
                "Options:\n"
                "  --check          Check for an update without installing\n"
                "  --force          Reinstall even if version matches\n"
                "  --repo=ORG/NAME  Override the GitHub repo (default: " HL_DEFAULT_REPO ")\n"
                "\n");
            return 0;
        }
    }

    /* ── 1. Set up TLS using the embedded CA bundle ─────────────── */
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *tls = hl_release_io_open_tls(&alloc);
    if (!tls) {
        fprintf(stderr, "hull update: no CA bundle available - cannot verify HTTPS\n");
        return 1;
    }

    /* ── 2. Fetch the latest release metadata ───────────────────── */
    char api_url[256];
    snprintf(api_url, sizeof(api_url),
             "https://api.github.com/repos/%s/releases/latest", repo);
    fprintf(stdout, "hull update: checking %s …\n", repo);

    char *meta_body = NULL;
    size_t meta_len = 0;
    if (hl_release_io_get(api_url, &meta_body, &meta_len, &alloc, tls,
                          "hull-update") != 0) {
        fprintf(stderr, "hull update: failed to fetch release metadata\n");
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    char latest_tag[64];
    if (hl_release_io_json_str(meta_body, "tag_name",
                               latest_tag, sizeof(latest_tag)) != 0) {
        fprintf(stderr, "hull update: could not parse release metadata\n");
        kl_free(&alloc, meta_body, meta_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    kl_free(&alloc, meta_body, meta_len);

    /* Strip leading 'v' from tag for comparison */
    const char *latest_ver = latest_tag;
    if (latest_ver[0] == 'v') latest_ver++;

    fprintf(stdout, "hull update: current  %s\n", HL_VERSION);
    fprintf(stdout, "hull update: latest   %s\n", latest_tag);

    if (!force && strcmp(HL_VERSION, latest_ver) == 0) {
        fprintf(stdout, "hull update: already up to date\n");
        kl_tls_mbedtls_ctx_destroy(tls);
        return 0;
    }

    if (check_only) {
        fprintf(stdout, "hull update: update available (run `hull update` to install)\n");
        kl_tls_mbedtls_ctx_destroy(tls);
        return 0;
    }

    /* ── 3. Download the asset ──────────────────────────────────── */
    const char *plat = hl_release_io_platform();
    char asset_name[64];
    snprintf(asset_name, sizeof(asset_name), "hull-%s", plat);

    char asset_url[256];
    snprintf(asset_url, sizeof(asset_url),
             "https://github.com/%s/releases/download/%s/%s",
             repo, latest_tag, asset_name);
    fprintf(stdout, "hull update: downloading %s …\n", asset_name);

    char *binary = NULL;
    size_t binary_len = 0;
    if (hl_release_io_get(asset_url, &binary, &binary_len, &alloc, tls,
                          "hull-update") != 0) {
        fprintf(stderr, "hull update: failed to download %s\n", asset_url);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    fprintf(stdout, "hull update: downloaded %zu bytes\n", binary_len);

    /* ── 4. Verify SHA-256 against the release manifest ─────────── */
    char sha_url[256];
    snprintf(sha_url, sizeof(sha_url),
             "https://github.com/%s/releases/download/%s/hull.sha256",
             repo, latest_tag);

    char *manifest = NULL;
    size_t manifest_len = 0;
    if (hl_release_io_get(sha_url, &manifest, &manifest_len, &alloc, tls,
                          "hull-update") != 0) {
        fprintf(stderr, "hull update: failed to download checksum manifest\n");
        kl_free(&alloc, binary, binary_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    /* ── 4b. Verify Ed25519 signature over the manifest (D-Phase) ── */
    /*
     * If this build has a configured release public key (the v0.1.0
     * release shipped a non-zero HL_RELEASE_PUBKEY_HEX), download and
     * verify hull.sha256.sig. Missing signature or verification failure
     * is a hard error - we never substitute an unverified manifest.
     *
     * Pre-v0.1.0 builds that ship the all-zeros placeholder skip this
     * step with a one-time warning.
     */
    if (hl_release_pubkey_configured()) {
        char sig_url[256];
        snprintf(sig_url, sizeof(sig_url),
                 "https://github.com/%s/releases/download/%s/hull.sha256.sig",
                 repo, latest_tag);

        char *sig_hex = NULL;
        size_t sig_len = 0;
        if (hl_release_io_get(sig_url, &sig_hex, &sig_len, &alloc, tls,
                              "hull-update") != 0) {
            fprintf(stderr,
                "hull update: failed to download release signature (hull.sha256.sig)\n"
                "             - this release is not signed; refusing to install\n");
            kl_free(&alloc, binary, binary_len);
            kl_free(&alloc, manifest, manifest_len);
            kl_tls_mbedtls_ctx_destroy(tls);
            return 1;
        }

        int sig_rc = hl_release_verify_manifest_sig(manifest, manifest_len,
                                                    sig_hex, sig_len, NULL);
        kl_free(&alloc, sig_hex, sig_len);
        if (sig_rc != 0) {
            fprintf(stderr,
                "hull update: release signature verification FAILED\n"
                "             - manifest does not match the embedded release public key\n");
            kl_free(&alloc, binary, binary_len);
            kl_free(&alloc, manifest, manifest_len);
            kl_tls_mbedtls_ctx_destroy(tls);
            return 1;
        }
        fprintf(stdout, "hull update: release signature verified\n");
    } else {
        fprintf(stderr,
            "hull update: WARNING - this hull build has no embedded release public key,\n"
            "             skipping Ed25519 signature check (SHA-256 only).\n");
    }

    char expected[65];
    if (hl_release_io_find_checksum(manifest, manifest_len,
                                    asset_name, expected) != 0) {
        fprintf(stderr, "hull update: no checksum entry for %s in hull.sha256\n",
                asset_name);
        kl_free(&alloc, binary, binary_len);
        kl_free(&alloc, manifest, manifest_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    kl_free(&alloc, manifest, manifest_len);

    char actual[65];
    if (hl_release_io_sha256_hex((const unsigned char *)binary,
                                 binary_len, actual) != 0) {
        fprintf(stderr, "hull update: SHA-256 computation failed\n");
        kl_free(&alloc, binary, binary_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }

    /* Constant-time compare. The values are public (manifest checksum
     * + computed digest of the downloaded binary), so a timing leak
     * doesn't reveal anything sensitive - using mbedtls_ct_memcmp for
     * uniformity with the rest of the hash-compare codepaths. Both
     * buffers are exactly 64 hex chars + NUL; compare the 64 chars. */
    if (mbedtls_ct_memcmp(expected, actual, 64) != 0) {
        fprintf(stderr, "hull update: SHA-256 mismatch\n");
        fprintf(stderr, "  expected: %s\n  actual:   %s\n", expected, actual);
        kl_free(&alloc, binary, binary_len);
        kl_tls_mbedtls_ctx_destroy(tls);
        return 1;
    }
    fprintf(stdout, "hull update: SHA-256 verified (%s)\n", actual);

    kl_tls_mbedtls_ctx_destroy(tls);

    /* ── 5. Atomic replace ──────────────────────────────────────── */
    char self_path[PATH_MAX];
    /* cppcheck-suppress knownConditionTrueFalse */
    if (hl_release_io_self_path(self_path, sizeof(self_path)) != 0) {
        /* Fall back to env->hull_exe (argv[0] from the dispatch layer). */
        if (env && env->hull_exe) {
            char resolved[PATH_MAX];
            if (realpath(env->hull_exe, resolved))
                snprintf(self_path, sizeof(self_path), "%s", resolved);
            else
                snprintf(self_path, sizeof(self_path), "%s", env->hull_exe);
        } else {
            fprintf(stderr, "hull update: cannot determine own binary path\n");
            kl_free(&alloc, binary, binary_len);
            return 1;
        }
    }

    /* Self-replace: atomic rename on POSIX; deferred rename-aside swap with
     * rollback on Windows, where a running .exe can't be overwritten in place. */
    if (hl_release_io_self_replace(self_path, binary, binary_len, 0755) != 0) {
        kl_free(&alloc, binary, binary_len);
        return 1;
    }
    kl_free(&alloc, binary, binary_len);

    fprintf(stdout, "hull update: installed %s → %s\n", latest_tag, self_path);
    return 0;
}
