/*
 * test_release_io.c - Unit tests for the shared HTTPS / manifest /
 * atomic-install helpers extracted into hull/release_io.{h,c}.
 *
 * Most of `release_io` is network-bound (TLS, HTTPS GET) and covered by
 * the live e2e suite (tests/e2e_update.sh). What CAN be unit-tested:
 *
 *   - hl_release_io_platform()        - string contents per OS/arch
 *   - hl_release_io_sha256_hex()      - hash + hex encoding
 *   - hl_release_io_json_str()        - tiny JSON-string extractor
 *   - hl_release_io_find_checksum()   - manifest line lookup, with
 *                                       particular care around the
 *                                       no-trailing-newline edge case
 *                                       (audit finding H1).
 *   - hl_release_io_atomic_write()    - temp file → fsync → rename
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/release_io.h"
#include "hull/release.h"   /* hl_release_pubkey_configured */

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── verify_local (build-time re-verify of an installed asset) ─────── */

static void vl_write(const char *dir, const char *name, const char *data) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(data, 1, strlen(data), f); fclose(f); }
}
static void vl_rm(const char *dir, const char *name) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    unlink(p);
}

/* No cached manifest in the dir -> refuse (cannot verify). */
UTEST(verify_local, missing_manifest_fails) {
    char dir[] = "/tmp/hlvlXXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    ASSERT_EQ(hl_release_io_verify_local_asset(dir, "libhull_platform-x.a"), -1);
    rmdir(dir);
}

/* With a real embedded pubkey: a manifest lacking (or with a bad) signature
 * fails closed. With a placeholder pubkey: the SHA-only path verifies a
 * matching lib and rejects a tampered one. Adapts to the build's key. */
UTEST(verify_local, tamper_and_signature_fail_closed) {
    char dir[] = "/tmp/hlvlXXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);

    const char *payload = "flavor platform lib bytes";
    char hex[65];
    ASSERT_EQ(hl_release_io_sha256_hex((const unsigned char *)payload,
                                       strlen(payload), hex), 0);
    char manifest[128];
    snprintf(manifest, sizeof(manifest), "%s  libhull_platform-x.a\n", hex);
    vl_write(dir, "hull.sha256", manifest);
    vl_write(dir, "libhull_platform-x.a", payload);

    if (hl_release_pubkey_configured()) {
        /* No signature file -> refuse. */
        ASSERT_EQ(hl_release_io_verify_local_asset(dir, "libhull_platform-x.a"), -1);
        /* Garbage signature -> refuse. */
        vl_write(dir, "hull.sha256.sig", "deadbeefdeadbeef");
        ASSERT_EQ(hl_release_io_verify_local_asset(dir, "libhull_platform-x.a"), -1);
        vl_rm(dir, "hull.sha256.sig");
    } else {
        /* Placeholder pubkey: SHA-only. Matching payload verifies. */
        ASSERT_EQ(hl_release_io_verify_local_asset(dir, "libhull_platform-x.a"), 0);
        /* Tampered payload -> reject. */
        vl_write(dir, "libhull_platform-x.a", "TAMPERED");
        ASSERT_EQ(hl_release_io_verify_local_asset(dir, "libhull_platform-x.a"), -1);
    }
    /* Unknown asset -> not in manifest (or fails earlier on sig). Either way, -1. */
    ASSERT_EQ(hl_release_io_verify_local_asset(dir, "libhull_platform-nope.a"), -1);

    vl_rm(dir, "hull.sha256");
    vl_rm(dir, "libhull_platform-x.a");
    rmdir(dir);
}

/* ── platform ─────────────────────────────────────────────────────── */

UTEST(release_io, platform_nonempty_and_known) {
    const char *p = hl_release_io_platform();
    ASSERT_TRUE(p != NULL);
    ASSERT_GT(strlen(p), 0u);
    /* Must be one of the four asset suffixes the release workflow ships. */
    int known = (strcmp(p, "linux-x86_64") == 0) ||
                (strcmp(p, "linux-aarch64") == 0) ||
                (strcmp(p, "darwin-arm64") == 0) ||
                (strcmp(p, "cosmo") == 0);
    ASSERT_EQ(known, 1);
}

/* ── SHA-256 ──────────────────────────────────────────────────────── */

UTEST(release_io, sha256_hex_empty) {
    /* SHA-256("") = e3b0c442… */
    char hex[65];
    ASSERT_EQ(hl_release_io_sha256_hex((const unsigned char *)"", 0, hex), 0);
    ASSERT_STREQ(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    /* NUL terminator. */
    ASSERT_EQ(hex[64], '\0');
}

UTEST(release_io, sha256_hex_known_vector) {
    /* SHA-256("abc") = ba7816bf… */
    char hex[65];
    ASSERT_EQ(hl_release_io_sha256_hex((const unsigned char *)"abc", 3, hex), 0);
    ASSERT_STREQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

/* ── JSON string extractor ───────────────────────────────────────── */

UTEST(release_io, json_str_extracts_value) {
    char out[64];
    ASSERT_EQ(hl_release_io_json_str(
        "{\"tag_name\":\"v0.1.2\",\"name\":\"hull v0.1.2\"}",
        "tag_name", out, sizeof(out)), 0);
    ASSERT_STREQ(out, "v0.1.2");
}

UTEST(release_io, json_str_missing_key_returns_negative) {
    char out[64];
    ASSERT_EQ(hl_release_io_json_str(
        "{\"tag_name\":\"v0.1.2\"}",
        "nope", out, sizeof(out)), -1);
}

UTEST(release_io, json_str_overflow_returns_negative) {
    char tiny[4];
    /* "v0.1.2" needs 7 bytes including NUL; tiny[4] overflows. */
    ASSERT_EQ(hl_release_io_json_str(
        "{\"tag_name\":\"v0.1.2\"}",
        "tag_name", tiny, sizeof(tiny)), -1);
}

UTEST(release_io, json_str_null_inputs_safe) {
    char out[16];
    ASSERT_EQ(hl_release_io_json_str(NULL, "k", out, sizeof(out)), -1);
    ASSERT_EQ(hl_release_io_json_str("{}", NULL, out, sizeof(out)), -1);
    ASSERT_EQ(hl_release_io_json_str("{}", "k", NULL, 16), -1);
}

/* ── Manifest checksum lookup ────────────────────────────────────── */

/* A well-formed multi-line manifest with trailing newlines. */
static const char *G_MANIFEST_OK =
    "0000000000000000000000000000000000000000000000000000000000000001  hull-linux-x86_64\n"
    "0000000000000000000000000000000000000000000000000000000000000002  hull-linux-aarch64\n"
    "0000000000000000000000000000000000000000000000000000000000000003  hull-darwin-arm64\n"
    "0000000000000000000000000000000000000000000000000000000000000004  hull-cosmo\n";

UTEST(find_checksum, finds_each_entry) {
    char hex[65];
    ASSERT_EQ(hl_release_io_find_checksum(G_MANIFEST_OK, strlen(G_MANIFEST_OK),
                                          "hull-linux-x86_64", hex), 0);
    ASSERT_STREQ(hex,
        "0000000000000000000000000000000000000000000000000000000000000001");

    ASSERT_EQ(hl_release_io_find_checksum(G_MANIFEST_OK, strlen(G_MANIFEST_OK),
                                          "hull-darwin-arm64", hex), 0);
    ASSERT_STREQ(hex,
        "0000000000000000000000000000000000000000000000000000000000000003");

    ASSERT_EQ(hl_release_io_find_checksum(G_MANIFEST_OK, strlen(G_MANIFEST_OK),
                                          "hull-cosmo", hex), 0);
    ASSERT_STREQ(hex,
        "0000000000000000000000000000000000000000000000000000000000000004");
}

UTEST(find_checksum, misses_unknown_asset) {
    char hex[65];
    ASSERT_EQ(hl_release_io_find_checksum(G_MANIFEST_OK, strlen(G_MANIFEST_OK),
                                          "hull-freebsd", hex), -1);
}

UTEST(find_checksum, exact_match_no_prefix_collision) {
    /* "hull-linux-x86" must not match the "hull-linux-x86_64" line. */
    char hex[65];
    ASSERT_EQ(hl_release_io_find_checksum(G_MANIFEST_OK, strlen(G_MANIFEST_OK),
                                          "hull-linux-x86", hex), -1);
}

/* Audit finding H1: when the matching line has no trailing newline AND
 * the asset name ends exactly at the end of the manifest buffer, the
 * old code dereferenced one byte past `end`. The fix reorders the OR
 * chain so the bounds check runs first. This test exercises that
 * exact shape - a buffer where the last byte of the asset name is the
 * last byte of the buffer (no newline, no NUL). */
UTEST(find_checksum, no_trailing_newline_last_line) {
    static const char manifest_no_nl[] =
        "0000000000000000000000000000000000000000000000000000000000000001  hull-linux-x86_64\n"
        "0000000000000000000000000000000000000000000000000000000000000002  hull-cosmo";
    /* strlen excludes the implicit NUL - we deliberately pass the raw
     * length so the helper sees no NUL terminator after "cosmo". */
    size_t mlen = sizeof(manifest_no_nl) - 1;
    char hex[65];
    ASSERT_EQ(hl_release_io_find_checksum(manifest_no_nl, mlen,
                                          "hull-cosmo", hex), 0);
    ASSERT_STREQ(hex,
        "0000000000000000000000000000000000000000000000000000000000000002");
}

UTEST(find_checksum, empty_manifest) {
    char hex[65];
    ASSERT_EQ(hl_release_io_find_checksum("", 0, "anything", hex), -1);
}

UTEST(find_checksum, null_args_safe) {
    char hex[65];
    ASSERT_EQ(hl_release_io_find_checksum(NULL, 0, "a", hex), -1);
    ASSERT_EQ(hl_release_io_find_checksum("x", 1, NULL, hex), -1);
    ASSERT_EQ(hl_release_io_find_checksum("x", 1, "a", NULL), -1);
}

/* ── Atomic write ────────────────────────────────────────────────── */

UTEST(atomic_write, creates_file_and_replaces) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/tmp/hull-aw-%d", getpid());
    unlink(tmp);
    const char *payload1 = "first\n";
    ASSERT_EQ(hl_release_io_atomic_write(tmp, payload1, strlen(payload1), 0644), 0);

    struct stat st;
    ASSERT_EQ(stat(tmp, &st), 0);
    ASSERT_EQ((size_t)st.st_size, strlen(payload1));
    /* Owner-read at minimum (umask may strip group/other). */
    ASSERT_NE((st.st_mode & 0400), (mode_t)0);

    /* Overwrite - the rename is atomic; new file replaces old. */
    const char *payload2 = "second-payload-bytes\n";
    ASSERT_EQ(hl_release_io_atomic_write(tmp, payload2, strlen(payload2), 0755), 0);

    ASSERT_EQ(stat(tmp, &st), 0);
    ASSERT_EQ((size_t)st.st_size, strlen(payload2));

    FILE *f = fopen(tmp, "rb");
    ASSERT_TRUE(f != NULL);
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    ASSERT_EQ(n, strlen(payload2));
    ASSERT_STREQ(buf, payload2);

    /* No leftover .new sidecar. */
    char sidecar[PATH_MAX];
    snprintf(sidecar, sizeof(sidecar), "%s.new", tmp);
    ASSERT_EQ(access(sidecar, F_OK), -1);

    unlink(tmp);
}

UTEST(atomic_write, zero_byte_payload_ok) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/tmp/hull-aw-zero-%d", getpid());
    unlink(tmp);
    ASSERT_EQ(hl_release_io_atomic_write(tmp, "", 0, 0644), 0);
    struct stat st;
    ASSERT_EQ(stat(tmp, &st), 0);
    ASSERT_EQ((size_t)st.st_size, 0u);
    unlink(tmp);
}

UTEST(atomic_write, null_args_rejected) {
    ASSERT_EQ(hl_release_io_atomic_write(NULL, "data", 4, 0644), -1);
    ASSERT_EQ(hl_release_io_atomic_write("/tmp/x", NULL, 4, 0644), -1);
}

UTEST(atomic_write, refuses_unwritable_parent) {
    /* /this/does/not/exist has no parent - open(2) returns ENOENT. */
    ASSERT_EQ(hl_release_io_atomic_write("/this/does/not/exist/file",
                                         "x", 1, 0644), -1);
}

/* ── self_replace: self-update-safe replace incl. Windows deferred swap ── */

static int seed(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(s, f);
    return fclose(f);
}

static int first_bytes(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, n - 1, f);
    buf[r] = '\0';
    fclose(f);
    return 0;
}

/* 1 if a `.new` or `.old` sidecar was left behind next to @p tmp. */
static int has_sidecars(const char *tmp)
{
    char side[PATH_MAX];
    snprintf(side, sizeof(side), "%s.new", tmp);
    if (access(side, F_OK) == 0) return 1;
    snprintf(side, sizeof(side), "%s.old", tmp);
    if (access(side, F_OK) == 0) return 1;
    return 0;
}

UTEST(self_replace, atomic_path_replaces) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/tmp/hull-sr-%d", getpid());
    unlink(tmp);
    ASSERT_EQ(seed(tmp, "OLD-BINARY"), 0);

    const char *nw = "NEW-BINARY-BYTES";
    ASSERT_EQ(hl_release_io_self_replace(tmp, nw, strlen(nw), 0755), 0);

    char buf[64];
    ASSERT_EQ(first_bytes(tmp, buf, sizeof(buf)), 0);
    ASSERT_STREQ(buf, nw);
    ASSERT_EQ(has_sidecars(tmp), 0);
    unlink(tmp);
}

UTEST(self_replace, deferred_swap_replaces) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/tmp/hull-sr-def-%d", getpid());
    unlink(tmp);
    ASSERT_EQ(seed(tmp, "OLD"), 0);

    /* Force the Windows-style deferred swap on POSIX. */
    setenv("HULL_FORCE_DEFERRED_SWAP", "1", 1);
    const char *nw = "NEW-VIA-DEFERRED-SWAP";
    int rc = hl_release_io_self_replace(tmp, nw, strlen(nw), 0755);
    unsetenv("HULL_FORCE_DEFERRED_SWAP");

    ASSERT_EQ(rc, 0);
    char buf[64];
    ASSERT_EQ(first_bytes(tmp, buf, sizeof(buf)), 0);
    ASSERT_STREQ(buf, nw);
    /* On POSIX the aside copy unlinks immediately; no sidecars remain. */
    ASSERT_EQ(has_sidecars(tmp), 0);
    unlink(tmp);
}

UTEST(self_replace, deferred_swap_rolls_back_on_failure) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/tmp/hull-sr-rb-%d", getpid());
    unlink(tmp);
    ASSERT_EQ(seed(tmp, "ORIGINAL-BINARY"), 0);

    /* Force the deferred swap AND simulate the install step failing mid-swap:
     * the original must be rolled back from the aside copy. */
    setenv("HULL_FORCE_DEFERRED_SWAP", "1", 1);
    setenv("HULL_TEST_SWAP_FAIL", "1", 1);
    int rc = hl_release_io_self_replace(tmp, "SHOULD-NOT-LAND", 15, 0755);
    unsetenv("HULL_TEST_SWAP_FAIL");
    unsetenv("HULL_FORCE_DEFERRED_SWAP");

    ASSERT_EQ(rc, -1);
    /* Original intact - a failed update never leaves hull missing. */
    char buf[64];
    ASSERT_EQ(first_bytes(tmp, buf, sizeof(buf)), 0);
    ASSERT_STREQ(buf, "ORIGINAL-BINARY");
    ASSERT_EQ(has_sidecars(tmp), 0);
    unlink(tmp);
}

UTEST(self_replace, path_with_spaces) {
    /* A running hull at a path with spaces (the reporter's Windows scenario). */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "/tmp/hull sr dir-%d", getpid());
    mkdir(dir, 0755);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/my hull.exe", dir);
    unlink(tmp);
    ASSERT_EQ(seed(tmp, "OLD"), 0);

    setenv("HULL_FORCE_DEFERRED_SWAP", "1", 1);
    const char *nw = "NEW-WITH-SPACES";
    int rc = hl_release_io_self_replace(tmp, nw, strlen(nw), 0755);
    unsetenv("HULL_FORCE_DEFERRED_SWAP");

    ASSERT_EQ(rc, 0);
    char buf[64];
    ASSERT_EQ(first_bytes(tmp, buf, sizeof(buf)), 0);
    ASSERT_STREQ(buf, nw);
    ASSERT_EQ(has_sidecars(tmp), 0);
    unlink(tmp);
    rmdir(dir);
}

UTEST(self_replace, cleanup_removes_stale_old) {
    /* cleanup resolves the running binary (this test exe) and removes its
     * `<self>.old`. Plant one next to the test exe and assert it's swept. */
    char self[PATH_MAX];
    if (hl_release_io_self_path(self, sizeof(self)) != 0)
        return;   /* no self-path on this platform (cosmo) - skip */
    char oldp[PATH_MAX + 8];
    snprintf(oldp, sizeof(oldp), "%s.old", self);
    if (seed(oldp, "leftover") != 0)
        return;   /* dir not writable - skip rather than fail spuriously */
    ASSERT_EQ(access(oldp, F_OK), 0);

    setenv("HULL_FORCE_DEFERRED_SWAP", "1", 1);   /* enable the sweep on POSIX */
    hl_release_io_cleanup_stale_self(NULL);
    unsetenv("HULL_FORCE_DEFERRED_SWAP");

    ASSERT_EQ(access(oldp, F_OK), -1);            /* .old swept */
    ASSERT_EQ(access(self, F_OK), 0);             /* self untouched */
}

UTEST(self_replace, null_args_rejected) {
    ASSERT_EQ(hl_release_io_self_replace(NULL, "x", 1, 0755), -1);
    ASSERT_EQ(hl_release_io_self_replace("/tmp/x", NULL, 4, 0755), -1);
    hl_release_io_cleanup_stale_self(NULL);   /* must not crash */
}

UTEST_MAIN()
