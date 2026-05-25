/*
 * test_release.c — Release manifest signing / verification tests
 *
 * Covers the API in include/hull/release.h:
 *   - hl_release_pubkey_configured() with the placeholder
 *   - hl_release_sign_manifest()  + hl_release_verify_manifest_sig() round-trip
 *   - Tamper detection (manifest, signature)
 *   - Hex parsing edge cases (short, long, garbage, trailing newline)
 *   - Wrong public key
 *   - hl_release_load_secret_key()
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/release.h"
#include "hull/cap/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Fixtures ──────────────────────────────────────────────────────── */

static uint8_t g_pk[32];
static uint8_t g_sk[64];
static const char g_manifest[] =
    "abc123def4567890abc123def4567890abc123def4567890abc123def4567890  hull-cosmo\n"
    "1111111122222222333333334444444455555555666666667777777788888888  hull-linux-x86_64\n"
    "aaaaaaaabbbbbbbbccccccccddddddddeeeeeeeeffffffff0000000099999999  hull-darwin-arm64\n";

struct release_fixture { int _unused; };

UTEST_F_SETUP(release_fixture) {
    (void)utest_fixture;
    int rc = hl_cap_crypto_ed25519_keypair(g_pk, g_sk);
    ASSERT_EQ(rc, 0);
}

UTEST_F_TEARDOWN(release_fixture) {
    (void)utest_fixture;
}

/* ── Embedded pubkey ────────────────────────────────────────────────── */

/* Pre-v0.1.0 ship-source had an all-zeros placeholder, and a separate
 * test asserted hl_release_pubkey_configured() == 0 to lock in the
 * "warn-and-skip verification" bypass for unsigned dev builds. With a
 * real v0.1.0 release key embedded the placeholder is gone, so the
 * test inverts: configured() must be 1 (any non-zero key), decode
 * must succeed, and the decoded bytes must be non-zero somewhere. */
UTEST(release, pubkey_is_configured) {
    ASSERT_EQ(hl_release_pubkey_configured(), 1);
}

UTEST(release, pubkey_decode_succeeds_and_is_nonzero) {
    uint8_t pk[32];
    ASSERT_EQ(hl_release_pubkey_decode(pk), 0);
    int any_nonzero = 0;
    for (size_t i = 0; i < 32; i++)
        if (pk[i] != 0) { any_nonzero = 1; break; }
    ASSERT_EQ(any_nonzero, 1);
}

UTEST(release, pubkey_decode_null_arg_rejected) {
    ASSERT_NE(hl_release_pubkey_decode(NULL), 0);
}

/* ── Sign / verify round-trip ──────────────────────────────────────── */

UTEST_F(release_fixture, sign_then_verify_roundtrip) {
    (void)utest_fixture;
    char sig_hex[129];
    int rc = hl_release_sign_manifest(g_manifest, strlen(g_manifest),
                                      g_sk, sig_hex, sizeof(sig_hex));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(strlen(sig_hex), (size_t)128);

    /* Verify with explicit pubkey */
    rc = hl_release_verify_manifest_sig(g_manifest, strlen(g_manifest),
                                        sig_hex, strlen(sig_hex), g_pk);
    ASSERT_EQ(rc, 0);
}

UTEST_F(release_fixture, verify_accepts_trailing_newline) {
    (void)utest_fixture;
    char sig_hex[140];
    int rc = hl_release_sign_manifest(g_manifest, strlen(g_manifest),
                                      g_sk, sig_hex, sizeof(sig_hex));
    ASSERT_EQ(rc, 0);

    /* Append newline + CR, like a file read might produce */
    size_t n = strlen(sig_hex);
    sig_hex[n] = '\n';
    sig_hex[n+1] = '\r';
    sig_hex[n+2] = '\0';

    rc = hl_release_verify_manifest_sig(g_manifest, strlen(g_manifest),
                                        sig_hex, n + 2, g_pk);
    ASSERT_EQ(rc, 0);
}

/* ── Tamper detection ──────────────────────────────────────────────── */

UTEST_F(release_fixture, tampered_manifest_rejected) {
    (void)utest_fixture;
    char sig_hex[129];
    ASSERT_EQ(hl_release_sign_manifest(g_manifest, strlen(g_manifest),
                                       g_sk, sig_hex, sizeof(sig_hex)), 0);

    /* Flip one byte of the manifest */
    char tampered[sizeof(g_manifest)];
    memcpy(tampered, g_manifest, sizeof(g_manifest));
    tampered[0] ^= 0x01;

    int rc = hl_release_verify_manifest_sig(tampered, strlen(tampered),
                                            sig_hex, strlen(sig_hex), g_pk);
    ASSERT_NE(rc, 0);
}

UTEST_F(release_fixture, tampered_signature_rejected) {
    (void)utest_fixture;
    char sig_hex[129];
    ASSERT_EQ(hl_release_sign_manifest(g_manifest, strlen(g_manifest),
                                       g_sk, sig_hex, sizeof(sig_hex)), 0);

    /* Flip the first hex digit */
    sig_hex[0] = (sig_hex[0] == 'a') ? 'b' : 'a';

    int rc = hl_release_verify_manifest_sig(g_manifest, strlen(g_manifest),
                                            sig_hex, strlen(sig_hex), g_pk);
    ASSERT_NE(rc, 0);
}

UTEST_F(release_fixture, wrong_pubkey_rejected) {
    (void)utest_fixture;
    char sig_hex[129];
    ASSERT_EQ(hl_release_sign_manifest(g_manifest, strlen(g_manifest),
                                       g_sk, sig_hex, sizeof(sig_hex)), 0);

    uint8_t other_pk[32], other_sk[64];
    ASSERT_EQ(hl_cap_crypto_ed25519_keypair(other_pk, other_sk), 0);

    int rc = hl_release_verify_manifest_sig(g_manifest, strlen(g_manifest),
                                            sig_hex, strlen(sig_hex), other_pk);
    ASSERT_NE(rc, 0);
}

/* ── Input validation ──────────────────────────────────────────────── */

UTEST(release, verify_null_manifest_rejected) {
    char sig[128] = {0};
    uint8_t pk[32] = {0};
    ASSERT_NE(hl_release_verify_manifest_sig(NULL, 0, sig, 128, pk), 0);
}

UTEST(release, verify_zero_length_manifest_rejected) {
    char sig[128] = {0};
    uint8_t pk[32] = {0};
    ASSERT_NE(hl_release_verify_manifest_sig("x", 0, sig, 128, pk), 0);
}

UTEST(release, verify_null_sig_rejected) {
    uint8_t pk[32] = {0};
    ASSERT_NE(hl_release_verify_manifest_sig("abc", 3, NULL, 0, pk), 0);
}

UTEST(release, verify_short_sig_rejected) {
    uint8_t pk[32] = {0};
    ASSERT_NE(hl_release_verify_manifest_sig("abc", 3, "deadbeef", 8, pk), 0);
}

UTEST(release, verify_non_hex_sig_rejected) {
    uint8_t pk[32] = {0};
    /* 128 chars but contains 'g' which is not hex */
    char bad_sig[129];
    memset(bad_sig, 'g', 128);
    bad_sig[128] = '\0';
    ASSERT_NE(hl_release_verify_manifest_sig("abc", 3, bad_sig, 128, pk), 0);
}

UTEST_F(release_fixture, sign_buf_too_small_rejected) {
    (void)utest_fixture;
    char small[64];  /* less than 129 */
    int rc = hl_release_sign_manifest(g_manifest, strlen(g_manifest),
                                      g_sk, small, sizeof(small));
    ASSERT_NE(rc, 0);
}

UTEST_F(release_fixture, sign_null_args_rejected) {
    (void)utest_fixture;
    char sig[129];
    ASSERT_NE(hl_release_sign_manifest(NULL, 1, g_sk, sig, sizeof(sig)), 0);
    ASSERT_NE(hl_release_sign_manifest(g_manifest, 1, NULL, sig, sizeof(sig)), 0);
    ASSERT_NE(hl_release_sign_manifest(g_manifest, 1, g_sk, NULL, 0), 0);
    ASSERT_NE(hl_release_sign_manifest(g_manifest, 0, g_sk, sig, sizeof(sig)), 0);
}

/* ── Secret key loading ────────────────────────────────────────────── */

static void hex_write(FILE *f, const uint8_t *data, size_t n)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        fputc(d[(data[i] >> 4) & 0xF], f);
        fputc(d[data[i] & 0xF], f);
    }
}

UTEST_F(release_fixture, load_secret_key_roundtrip) {
    (void)utest_fixture;
    char path[] = "/tmp/hull_release_test_XXXXXX.key";
    int fd = mkstemps(path, 4);
    ASSERT_GT(fd, -1);
    FILE *f = fdopen(fd, "w");
    hex_write(f, g_sk, 64);
    fputc('\n', f);
    fclose(f);

    uint8_t sk[64];
    int rc = hl_release_load_secret_key(path, sk);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(memcmp(sk, g_sk, 64), 0);

    unlink(path);
}

UTEST(release, load_secret_key_missing_file) {
    uint8_t sk[64];
    int rc = hl_release_load_secret_key("/tmp/does_not_exist_xyzzy.key", sk);
    ASSERT_NE(rc, 0);
}

UTEST(release, load_secret_key_short_file) {
    char path[] = "/tmp/hull_release_test_XXXXXX.key";
    int fd = mkstemps(path, 4);
    ASSERT_GT(fd, -1);
    FILE *f = fdopen(fd, "w");
    fputs("deadbeef\n", f);  /* 8 hex chars, way too short */
    fclose(f);

    uint8_t sk[64];
    int rc = hl_release_load_secret_key(path, sk);
    ASSERT_NE(rc, 0);
    unlink(path);
}

UTEST(release, load_secret_key_long_file) {
    char path[] = "/tmp/hull_release_test_XXXXXX.key";
    int fd = mkstemps(path, 4);
    ASSERT_GT(fd, -1);
    FILE *f = fdopen(fd, "w");
    /* 130 hex chars (too long after stripping newline) */
    for (int i = 0; i < 130; i++) fputc('a', f);
    fputc('\n', f);
    fclose(f);

    uint8_t sk[64];
    int rc = hl_release_load_secret_key(path, sk);
    ASSERT_NE(rc, 0);
    unlink(path);
}

UTEST(release, load_secret_key_null_args) {
    uint8_t sk[64];
    ASSERT_NE(hl_release_load_secret_key(NULL, sk), 0);
    ASSERT_NE(hl_release_load_secret_key("/tmp/x", NULL), 0);
}

UTEST_MAIN()
