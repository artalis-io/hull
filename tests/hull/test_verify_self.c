/*
 * test_verify_self.c. Unit tests for hull verify-self argument parsing
 * and the helper functions. The end-to-end verification flow is
 * covered by tests/e2e_verify_self.sh against a real release manifest.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/release_io.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SHA256_HEX_BUF 65U

/* ── Asset-name derivation ──────────────────────────────────────────── */

UTEST(verify_self, platform_returns_known_string)
{
    const char *plat = hl_release_io_platform();
    ASSERT_NE(plat, NULL);
    int ok = (strcmp(plat, "linux-x86_64") == 0 ||
              strcmp(plat, "linux-aarch64") == 0 ||
              strcmp(plat, "darwin-arm64") == 0 ||
              strcmp(plat, "cosmo") == 0);
    ASSERT_TRUE_MSG(ok, "platform must be one of the four known targets");
}

/* ── Self-path resolution ───────────────────────────────────────────── */

UTEST(verify_self, self_path_or_fallback)
{
    /* Linux: /proc/self/exe should resolve. macOS: _NSGetExecutablePath.
     * Cosmo: -1 by design. We assert that on Linux/macOS the path is
     * non-empty if the function succeeded; on cosmo we just confirm it
     * signals the fallback case to the caller. */
    char buf[4096];
    int rc = hl_release_io_self_path(buf, sizeof(buf));
    if (rc == 0) {
        ASSERT_GT(strlen(buf), 0u);
        /* Path should be absolute on the supported platforms. */
        ASSERT_EQ_MSG(buf[0], '/', "self path should be absolute");
    } else {
        /* -1 is acceptable: cosmo or unsupported platform. Caller
         * must fall back to argv[0]. */
        ASSERT_EQ(rc, -1);
    }
}

/* ── Checksum lookup in a manifest blob ─────────────────────────────── */

UTEST(verify_self, find_checksum_extracts_hex)
{
    const char manifest[] =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  hull-linux-x86_64\n"
        "0011223344556677889900112233445566778899001122334455667788990011  hull-linux-aarch64\n"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef  hull-darwin-arm64\n";

    char hex[SHA256_HEX_BUF];
    ASSERT_EQ(hl_release_io_find_checksum(manifest, strlen(manifest),
                                          "hull-linux-x86_64", hex), 0);
    ASSERT_STREQ(hex, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");

    ASSERT_EQ(hl_release_io_find_checksum(manifest, strlen(manifest),
                                          "hull-darwin-arm64", hex), 0);
    ASSERT_STREQ(hex, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
}

UTEST(verify_self, find_checksum_returns_error_for_missing)
{
    const char manifest[] =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  hull-linux-x86_64\n";

    char hex[SHA256_HEX_BUF];
    ASSERT_EQ(hl_release_io_find_checksum(manifest, strlen(manifest),
                                          "hull-darwin-arm64", hex), -1);
}

/* ── SHA-256 hashing of a known file ────────────────────────────────── */

UTEST(verify_self, sha256_hex_of_known_buffer)
{
    /* Known vector: SHA-256("abc") = ba7816bf8f01cfea4141... */
    char hex[SHA256_HEX_BUF];
    int rc = hl_release_io_sha256_hex((const unsigned char *)"abc", 3, hex);
    ASSERT_EQ(rc, 0);
    ASSERT_STREQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

UTEST_MAIN()
