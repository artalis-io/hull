/*
 * test_signature.c - Tests for package.sig reading and verification
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/signature.h"
#include "hull/vfs.h"
#include "hull/cap/crypto.h"
#include "test_tmpdir.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "test_tmpdir.h"

/* ── Helpers ──────────────────────────────────────────────────────── */

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", data[i]);
}

/* Temporary directory for test fixtures */
static char test_dir[256];

/* Platform keypair */
static uint8_t plat_pk[32];
static uint8_t plat_sk[64];
static char plat_pk_hex[65];
static char plat_sig_hex[129];
static char platforms_json[512];

/* App developer keypair */
static uint8_t test_pk[32];
static uint8_t test_sk[64];
static char test_pk_hex[65];

/* App file hash */
static char app_hash_hex[65];

/*
 * Sign a string payload with Ed25519, return hex in out_hex (must be 129 bytes).
 */
static void sign_payload(const char *payload, const uint8_t sk[64], char *out_hex)
{
    uint8_t sig[64];
    hl_cap_crypto_ed25519_sign((const uint8_t *)payload, strlen(payload), sk, sig);
    hex_encode(sig, 64, out_hex);
}

/*
 * Create a package.sig with dual-layer signing.
 */
static void create_test_package_sig(const char *dir,
                                     const char *files_json,
                                     const char *manifest_json,
                                     const char *binary_hash,
                                     const char *trampoline_hash)
{
    /* Build the platform.platforms payload and sign it */
    snprintf(platforms_json, sizeof(platforms_json),
             "{\"x86_64-cosmo\":{\"canary\":\"abcd1234\",\"hash\":\"deadbeef\"}}");

    sign_payload(platforms_json, plat_sk, plat_sig_hex);

    /* Build the full app payload (canonical key order) */
    char platform_obj[1024];
    snprintf(platform_obj, sizeof(platform_obj),
             "{\"platforms\":%s,\"public_key\":\"%s\",\"signature\":\"%s\"}",
             platforms_json, plat_pk_hex, plat_sig_hex);

    char payload[4096];
    snprintf(payload, sizeof(payload),
             "{\"binary_hash\":\"%s\","
             "\"build\":{\"cc\":\"cosmocc\",\"cc_version\":\"cosmocc 4.0.2\",\"flags\":\"-std=c11 -O2\"},"
             "\"files\":%s,"
             "\"manifest\":%s,"
             "\"platform\":%s,"
             "\"trampoline_hash\":\"%s\"}",
             binary_hash,
             files_json,
             manifest_json,
             platform_obj,
             trampoline_hash);

    char app_sig_hex[129];
    sign_payload(payload, test_sk, app_sig_hex);

    /* Write package.sig */
    char path[512];
    snprintf(path, sizeof(path), "%s/package.sig", dir);
    FILE *f = fopen(path, "w");
    fprintf(f,
        "{\"binary_hash\":\"%s\","
        "\"build\":{\"cc\":\"cosmocc\",\"cc_version\":\"cosmocc 4.0.2\",\"flags\":\"-std=c11 -O2\"},"
        "\"files\":%s,"
        "\"manifest\":%s,"
        "\"platform\":%s,"
        "\"public_key\":\"%s\","
        "\"signature\":\"%s\","
        "\"trampoline_hash\":\"%s\"}\n",
        binary_hash,
        files_json,
        manifest_json,
        platform_obj,
        test_pk_hex,
        app_sig_hex,
        trampoline_hash);
    fclose(f);
}

/*
 * Create a legacy hull.sig (for backwards compatibility tests).
 */
static void create_legacy_sig(const char *dir, const char *files_json,
                              const char *manifest_json, const uint8_t sk[64],
                              const uint8_t pk[32])
{
    char payload[4096];
    snprintf(payload, sizeof(payload),
             "{\"files\":%s,\"manifest\":%s}", files_json, manifest_json);

    uint8_t sig[64];
    hl_cap_crypto_ed25519_sign((const uint8_t *)payload, strlen(payload), sk, sig);

    char sig_hex[129];
    hex_encode(sig, 64, sig_hex);

    char pk_hex[65];
    hex_encode(pk, 32, pk_hex);

    char path[512];
    snprintf(path, sizeof(path), "%s/hull.sig", dir);
    FILE *f = fopen(path, "w");
    fprintf(f,
        "{\"files\":%s,\"manifest\":%s,\"public_key\":\"%s\","
        "\"signature\":\"%s\",\"version\":1}\n",
        files_json, manifest_json, pk_hex, sig_hex);
    fclose(f);
}

/* ── Setup ────────────────────────────────────────────────────────── */

UTEST(hl_sig, setup)
{
    /* Create temp dir */
    hl_test_path(test_dir, sizeof(test_dir), "hull_test_sig_XXXXXX");
    ASSERT_NE(mkdtemp(test_dir), (char *)NULL);

    /* Generate platform keypair */
    int rc = hl_cap_crypto_ed25519_keypair(plat_pk, plat_sk);
    ASSERT_EQ(rc, 0);
    hex_encode(plat_pk, 32, plat_pk_hex);

    /* Generate app developer keypair */
    rc = hl_cap_crypto_ed25519_keypair(test_pk, test_sk);
    ASSERT_EQ(rc, 0);
    hex_encode(test_pk, 32, test_pk_hex);

    /* Create a test app file */
    char app_path[512];
    snprintf(app_path, sizeof(app_path), "%s/app.lua", test_dir);
    FILE *f = fopen(app_path, "w");
    fprintf(f, "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n");
    fclose(f);

    /* Compute hash of app.lua */
    const char *app_content = "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n";
    uint8_t hash[32];
    hl_cap_crypto_sha256(app_content, strlen(app_content), hash);
    hex_encode(hash, 32, app_hash_hex);

    /* Create package.sig with correct hash */
    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", app_hash_hex);

    create_test_package_sig(test_dir, files_json, "null",
                            "binary0000000000000000000000000000000000000000000000000000000000000000",
                            "trampoline00000000000000000000000000000000000000000000000000000000");
}

/* ── Read tests ────────────────────────────────────────────────────── */

UTEST(hl_sig, read_valid)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    int rc = hl_sig_read(sig_path, &sig);
    ASSERT_EQ(rc, 0);

    ASSERT_TRUE(sig.files_value != NULL);
    ASSERT_TRUE(sig.signature_hex != NULL);
    ASSERT_TRUE(sig.public_key_hex != NULL);
    ASSERT_TRUE(sig.binary_hash_hex != NULL);
    ASSERT_TRUE(sig.trampoline_hash_hex != NULL);
    ASSERT_TRUE(sig.build_value != NULL);
    ASSERT_EQ((int)strlen(sig.signature_hex), 128);
    ASSERT_EQ((int)strlen(sig.public_key_hex), 64);
    ASSERT_TRUE(sig.entry_count > 0);
    ASSERT_STREQ(sig.entries[0].name, "app.lua");

    /* Platform layer */
    ASSERT_TRUE(sig.platform.platforms_value != NULL);
    ASSERT_TRUE(sig.platform.signature_hex != NULL);
    ASSERT_TRUE(sig.platform.public_key_hex != NULL);
    ASSERT_TRUE(sig.platform.entry_count > 0);
    ASSERT_STREQ(sig.platform.entries[0].arch, "x86_64-cosmo");

    hl_sig_free(&sig);
}

UTEST(hl_sig, read_invalid_path)
{
    HlSignature sig;
    int rc = hl_sig_read("/nonexistent/package.sig", &sig);
    ASSERT_EQ(rc, -1);
}

UTEST(hl_sig, read_null_args)
{
    HlSignature sig;
    ASSERT_EQ(hl_sig_read(NULL, &sig), -1);
    ASSERT_EQ(hl_sig_read("/tmp/x", NULL), -1);
}

UTEST(hl_sig, read_invalid_json)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/bad.sig", test_dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "not json at all");
    fclose(f);

    HlSignature sig;
    int rc = hl_sig_read(path, &sig);
    ASSERT_EQ(rc, -1);
}

/* ── App layer verify tests ───────────────────────────────────────── */

UTEST(hl_sig, verify_good)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    int rc = hl_sig_verify(&sig, test_pk);
    ASSERT_EQ(rc, 0);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_wrong_key)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    uint8_t other_pk[32], other_sk[64];
    hl_cap_crypto_ed25519_keypair(other_pk, other_sk);

    int rc = hl_sig_verify(&sig, other_pk);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_bad_sig)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    /* Tamper with the signature (cast away const for test only) */
    ((char *)(uintptr_t)sig.signature_hex)[0] =
        (sig.signature_hex[0] == 'a') ? 'b' : 'a';

    int rc = hl_sig_verify(&sig, test_pk);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);
}

/* ── Platform layer verify tests ──────────────────────────────────── */

UTEST(hl_sig, verify_platform_good)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    int rc = hl_sig_verify_platform(&sig, plat_pk);
    ASSERT_EQ(rc, 0);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_platform_wrong_key)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    uint8_t other_pk[32], other_sk[64];
    hl_cap_crypto_ed25519_keypair(other_pk, other_sk);

    int rc = hl_sig_verify_platform(&sig, other_pk);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_platform_tampered_sig)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    /* Tamper with platform signature (cast away const for test only) */
    ((char *)(uintptr_t)sig.platform.signature_hex)[0] =
        (sig.platform.signature_hex[0] == 'a') ? 'b' : 'a';

    int rc = hl_sig_verify_platform(&sig, plat_pk);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);
}

/* ── File hash verification (filesystem) ──────────────────────────── */

UTEST(hl_sig, verify_files_fs_good)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    int rc = hl_sig_verify_files_fs(&sig, test_dir);
    ASSERT_EQ(rc, 0);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_files_fs_tampered)
{
    /* Tamper with app.lua */
    char app_path[512];
    snprintf(app_path, sizeof(app_path), "%s/app.lua", test_dir);

    FILE *f = fopen(app_path, "a");
    fprintf(f, "-- tampered\n");
    fclose(f);

    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    int rc = hl_sig_verify_files_fs(&sig, test_dir);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);

    /* Restore original content */
    f = fopen(app_path, "w");
    fprintf(f, "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n");
    fclose(f);
}

UTEST(hl_sig, verify_files_fs_missing)
{
    char files_json[] = "{\"nonexistent.lua\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/missing_test", test_dir);
    mkdir(subdir, 0755);

    create_test_package_sig(subdir, files_json, "null", "deadbeef00", "deadbeef00");

    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", subdir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    int rc = hl_sig_verify_files_fs(&sig, subdir);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);
}

/* Every *.sql in migrations/ runs at startup, so in filesystem mode one that
 * is not in the signature is refused - it used to run although --verify-sig
 * passed. */
UTEST(hl_sig, verify_files_fs_rejects_unsigned_migration)
{
    char subdir[512], mdir[600], p[700];
    snprintf(subdir, sizeof(subdir), "%s/mig_test", test_dir);
    mkdir(subdir, 0755);
    snprintf(mdir, sizeof(mdir), "%s/migrations", subdir);
    mkdir(mdir, 0755);

    const char *sql = "CREATE TABLE t (x INTEGER);\n";
    snprintf(p, sizeof(p), "%s/001_init.sql", mdir);
    FILE *f = fopen(p, "w");
    ASSERT_TRUE(f != NULL);
    fputs(sql, f);
    fclose(f);
    uint8_t hash[32];
    char hex[65];
    hl_cap_crypto_sha256(sql, strlen(sql), hash);
    hex_encode(hash, 32, hex);

    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"migrations/001_init.sql\":\"%s\"}", hex);
    create_test_package_sig(subdir, files_json, "null", "deadbeef00", "deadbeef00");

    char sig_path[700];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", subdir);
    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);
    EXPECT_EQ(hl_sig_verify_files_fs(&sig, subdir), 0);

    snprintf(p, sizeof(p), "%s/999_extra.sql", mdir);
    f = fopen(p, "w");
    ASSERT_TRUE(f != NULL);
    fputs("DROP TABLE t;\n", f);
    fclose(f);
    EXPECT_EQ(hl_sig_verify_files_fs(&sig, subdir), -1);
    hl_sig_free(&sig);
}

/* The app signature covers `platform` only when platforms / public_key /
 * signature are all there; otherwise the signed payload says null. A gethull
 * block in that shape is not covered by the signature, so it is refused. */
UTEST(hl_sig, verify_startup_rejects_unsigned_gethull)
{
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/gethull_test", test_dir);
    mkdir(subdir, 0755);

    char app_path[600];
    snprintf(app_path, sizeof(app_path), "%s/app.lua", subdir);
    FILE *f = fopen(app_path, "w");
    ASSERT_TRUE(f != NULL);
    fprintf(f, "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n");
    fclose(f);

    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"binary_hash\":\"b0\","
             "\"build\":{\"cc\":\"cc\",\"cc_version\":\"1\",\"flags\":\"-O2\"},"
             "\"files\":{\"app.lua\":\"%s\"},"
             "\"manifest\":null,"
             "\"platform\":null,"
             "\"trampoline_hash\":\"t0\"}", app_hash_hex);
    char app_sig_hex[129];
    sign_payload(payload, test_sk, app_sig_hex);

    char sig_path[600];
    snprintf(sig_path, sizeof(sig_path), "%s/package.sig", subdir);
    f = fopen(sig_path, "w");
    ASSERT_TRUE(f != NULL);
    fprintf(f,
        "{\"binary_hash\":\"b0\","
        "\"build\":{\"cc\":\"cc\",\"cc_version\":\"1\",\"flags\":\"-O2\"},"
        "\"files\":{\"app.lua\":\"%s\"},"
        "\"manifest\":null,"
        "\"platform\":{\"gethull\":{\"manifest\":\"m\",\"signature\":\"s\"}},"
        "\"public_key\":\"%s\",\"signature\":\"%s\",\"trampoline_hash\":\"t0\"}\n",
        app_hash_hex, test_pk_hex, app_sig_hex);
    fclose(f);

    char pk_path[600];
    snprintf(pk_path, sizeof(pk_path), "%s/test.pub", subdir);
    f = fopen(pk_path, "w");
    ASSERT_TRUE(f != NULL);
    fprintf(f, "%s\n", test_pk_hex);
    fclose(f);

    extern const HlEntry hl_app_entries[];
    HlVfs app_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, subdir);
    EXPECT_EQ(hl_verify_startup(pk_path, app_path, &app_vfs, 1), -1);
}

/* ── Legacy hull.sig backwards compatibility ──────────────────────── */

UTEST(hl_sig, legacy_read_and_verify)
{
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/legacy_test", test_dir);
    mkdir(subdir, 0755);

    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", app_hash_hex);

    create_legacy_sig(subdir, files_json, "null", test_sk, test_pk);

    /* Copy app.lua to subdir */
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/app.lua", test_dir);
    snprintf(dst, sizeof(dst), "%s/app.lua", subdir);
    FILE *fi = fopen(src, "rb");
    FILE *fo = fopen(dst, "wb");
    int ch;
    while ((ch = fgetc(fi)) != EOF) fputc(ch, fo);
    fclose(fi);
    fclose(fo);

    /* Read legacy hull.sig */
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", subdir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    /* No binary_hash → legacy format */
    ASSERT_TRUE(sig.binary_hash_hex == NULL);

    /* Verify app layer (legacy payload) */
    int rc = hl_sig_verify(&sig, test_pk);
    ASSERT_EQ(rc, 0);

    /* Verify file hashes */
    rc = hl_sig_verify_files_fs(&sig, subdir);
    ASSERT_EQ(rc, 0);

    hl_sig_free(&sig);
}

UTEST(hl_sig, legacy_no_manifest)
{
    const char *app_content = "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n";
    uint8_t hash[32];
    hl_cap_crypto_sha256(app_content, strlen(app_content), hash);
    char hash_hex[65];
    hex_encode(hash, 32, hash_hex);

    /* Build payload WITHOUT manifest */
    char payload[1024];
    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", hash_hex);
    snprintf(payload, sizeof(payload), "{\"files\":%s}", files_json);

    uint8_t sig_bytes[64];
    hl_cap_crypto_ed25519_sign((const uint8_t *)payload, strlen(payload),
                                test_sk, sig_bytes);
    char sig_hex[129];
    hex_encode(sig_bytes, 64, sig_hex);

    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/no_manifest", test_dir);
    mkdir(subdir, 0755);

    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", subdir);
    FILE *f = fopen(sig_path, "w");
    fprintf(f,
        "{\"files\":%s,\"public_key\":\"%s\","
        "\"signature\":\"%s\",\"version\":1}\n",
        files_json, test_pk_hex, sig_hex);
    fclose(f);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);
    ASSERT_TRUE(sig.manifest_value == NULL);

    int rc = hl_sig_verify(&sig, test_pk);
    ASSERT_EQ(rc, 0);

    hl_sig_free(&sig);
}

/* ── Full startup verification ────────────────────────────────────── */

UTEST(hl_sig, verify_startup_good)
{
    /* Write pubkey file */
    char pk_path[512];
    snprintf(pk_path, sizeof(pk_path), "%s/test.pub", test_dir);
    FILE *f = fopen(pk_path, "w");
    fprintf(f, "%s\n", test_pk_hex);
    fclose(f);

    /* Re-create package.sig with correct hash */
    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", app_hash_hex);
    create_test_package_sig(test_dir, files_json, "null",
                            "binary0000000000000000000000000000000000000000000000000000000000000000",
                            "trampoline00000000000000000000000000000000000000000000000000000000");

    char entry_point[512];
    snprintf(entry_point, sizeof(entry_point), "%s/app.lua", test_dir);

    extern const HlEntry hl_app_entries[];
    HlVfs app_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, test_dir);

    /* no_verify_platform=1: the test fixture's package.sig has no
     * gethull block (built before C3 added it), and the embedded
     * HL_PLATFORM_PUBKEY_HEX is still the all-zeros placeholder in
     * this commit anyway (C5 restores the real key). Passing 1 keeps
     * the test focused on the developer-key layer regardless of the
     * platform-sig state. */
    int rc = hl_verify_startup(pk_path, entry_point, &app_vfs, 1);
    ASSERT_EQ(rc, 0);
}

UTEST(hl_sig, verify_startup_bad_key)
{
    uint8_t other_pk[32], other_sk[64];
    hl_cap_crypto_ed25519_keypair(other_pk, other_sk);
    char other_pk_hex[65];
    hex_encode(other_pk, 32, other_pk_hex);

    char pk_path[512];
    snprintf(pk_path, sizeof(pk_path), "%s/other.pub", test_dir);
    FILE *f = fopen(pk_path, "w");
    fprintf(f, "%s\n", other_pk_hex);
    fclose(f);

    char entry_point[512];
    snprintf(entry_point, sizeof(entry_point), "%s/app.lua", test_dir);

    extern const HlEntry hl_app_entries[];
    HlVfs app_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, test_dir);

    /* no_verify_platform=1: the test fixture's package.sig has no
     * gethull block (built before C3 added it), and the embedded
     * HL_PLATFORM_PUBKEY_HEX is still the all-zeros placeholder in
     * this commit anyway (C5 restores the real key). Passing 1 keeps
     * the test focused on the developer-key layer regardless of the
     * platform-sig state. */
    int rc = hl_verify_startup(pk_path, entry_point, &app_vfs, 1);
    ASSERT_EQ(rc, -1);
}

/* ── Canary detection test ────────────────────────────────────────── */

UTEST(hl_sig, canary_detection)
{
    /* Simulate a binary with embedded canary */
    uint8_t test_binary[128];
    memset(test_binary, 0, sizeof(test_binary));

    /* Write the magic marker at offset 16 */
    memcpy(test_binary + 16, "HULL_PLATFORM_CANARY", 20);
    /* Pad to 24 bytes (magic field is char[24]) */
    /* bytes 36-39 are already 0 from memset */

    /* Write known integrity hash at offset 40 (16 + 24) */
    uint8_t expected_integrity[32];
    for (int i = 0; i < 32; i++)
        expected_integrity[i] = (uint8_t)(0xab + i);
    memcpy(test_binary + 40, expected_integrity, 32);

    /* Scan for the canary marker */
    const char *marker = "HULL_PLATFORM_CANARY";
    size_t marker_len = 20;
    int found = 0;
    uint8_t found_integrity[32];

    for (size_t i = 0; i <= sizeof(test_binary) - marker_len - 32 - 4; i++) {
        if (memcmp(test_binary + i, marker, marker_len) == 0) {
            /* Extract 32 bytes after the 24-byte magic field */
            memcpy(found_integrity, test_binary + i + 24, 32);
            found = 1;
            break;
        }
    }

    ASSERT_TRUE(found);
    ASSERT_EQ(memcmp(found_integrity, expected_integrity, 32), 0);
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

/* Portable recursive delete (no shell dependency) */
static int rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        rmdir_recursive(child);
    }
    closedir(d);
    return rmdir(path);
}

/* ── Round 5: embedded name normalisation (M1), planted files (H2) ── */

static void sha_hex(const void *data, size_t len, char out[65])
{
    uint8_t h[32];
    hl_cap_crypto_sha256(data, len, h);
    hex_encode(h, 32, out);
}

/* The embedded app of a non-trivial build: a Lua entry ("./app", .lua
 * stripped), a JS module and a JSON file ("./" kept), and templates /
 * static / migrations / compute (wasm + AOT) / shaders under their BARE
 * app-relative names - exactly as build.lua's generate_app_registry names
 * them. Sorted in strcmp order (HlVfs binary search). */
static const char E_APP[]  = "app.get('/', function() end)\n";
static const char E_JSON[] = "{\"a\":1}\n";
static const char E_JS[]   = "export const x = 1;\n";
static const char E_AOT[]  = "AOT-NATIVE-CODE";
static const char E_WASM[] = "\0asm\1\0\0\0";
static const char E_SQL[]  = "CREATE TABLE t (x INTEGER);\n";
static const char E_WGSL[] = "@compute fn main() {}\n";
static const char E_CSS[]  = "body{}\n";
static const char E_HTML[] = "<p>{{ x }}</p>\n";
static const char E_TPLUA[] = "-- a .lua under templates/\n";

#define ENT(n, d) { n, (const unsigned char *)(d), sizeof(d) - 1 }
static const HlEntry g_emb[] = {
    ENT("./app", E_APP),
    ENT("./data.json", E_JSON),
    ENT("./lib/util.js", E_JS),
    ENT("compute/score.aot.x86_64", E_AOT),
    { "compute/score.wasm", (const unsigned char *)E_WASM, sizeof(E_WASM) - 1 },
    ENT("migrations/001_init.sql", E_SQL),
    ENT("shaders/s.wgsl", E_WGSL),
    ENT("static/style.css", E_CSS),
    ENT("templates/base.html", E_HTML),
    ENT("templates/x.lua", E_TPLUA),
    { 0, 0, 0 }
};

/* package.sig for g_emb, every file under its app-relative (signed) name.
 * `skip` leaves one name out; `tamper` signs a wrong hash for one. */
static int emb_sig(const char *dir, const char *skip, const char *tamper,
                   HlSignature *sig)
{
    struct { const char *name; const char *d; size_t n; } f[] = {
        { "app.lua", E_APP, sizeof(E_APP) - 1 },
        { "data.json", E_JSON, sizeof(E_JSON) - 1 },
        { "lib/util.js", E_JS, sizeof(E_JS) - 1 },
        { "compute/score.aot.x86_64", E_AOT, sizeof(E_AOT) - 1 },
        { "compute/score.wasm", E_WASM, sizeof(E_WASM) - 1 },
        { "migrations/001_init.sql", E_SQL, sizeof(E_SQL) - 1 },
        { "shaders/s.wgsl", E_WGSL, sizeof(E_WGSL) - 1 },
        { "static/style.css", E_CSS, sizeof(E_CSS) - 1 },
        { "templates/base.html", E_HTML, sizeof(E_HTML) - 1 },
        { "templates/x.lua", E_TPLUA, sizeof(E_TPLUA) - 1 },
    };
    char files[2048];
    size_t off = 0;
    off += (size_t)snprintf(files + off, sizeof files - off, "{");
    int first = 1;
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (skip && strcmp(skip, f[i].name) == 0) continue;
        char hx[65];
        if (tamper && strcmp(tamper, f[i].name) == 0)
            sha_hex("tampered", 8, hx);
        else
            sha_hex(f[i].d, f[i].n, hx);
        off += (size_t)snprintf(files + off, sizeof files - off, "%s\"%s\":\"%s\"",
                                first ? "" : ",", f[i].name, hx);
        first = 0;
    }
    snprintf(files + off, sizeof files - off, "}");
    mkdir(dir, 0755);
    create_test_package_sig(dir, files, "null", "deadbeef00", "deadbeef00");
    char p[700];
    snprintf(p, sizeof p, "%s/package.sig", dir);
    return hl_sig_read(p, sig);
}

/* M1: every built app with a template, static file, migration, compute
 * module or shader failed embedded --verify-sig ("file not found in
 * binary"): the verifier looked up "./" names only. */
UTEST(hl_sig, embedded_app_with_every_file_kind_verifies)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/emb_ok", test_dir);
    HlSignature sig;
    ASSERT_EQ(emb_sig(dir, NULL, NULL, &sig), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, g_emb, dir);
    EXPECT_EQ(hl_sig_verify_files_embedded(&sig, &vfs), 0);
    hl_sig_free(&sig);
}

UTEST(hl_sig, embedded_unsigned_entry_is_refused)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/emb_extra", test_dir);
    HlSignature sig;
    ASSERT_EQ(emb_sig(dir, "compute/score.aot.x86_64", NULL, &sig), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, g_emb, dir);
    EXPECT_EQ(hl_sig_verify_files_embedded(&sig, &vfs), -1);
    hl_sig_free(&sig);
}

UTEST(hl_sig, embedded_modified_entry_is_refused)
{
    static const char *const names[] = {
        "app.lua", "lib/util.js", "templates/base.html", "static/style.css",
        "migrations/001_init.sql", "compute/score.wasm",
        "compute/score.aot.x86_64", "shaders/s.wgsl", "templates/x.lua", NULL
    };
    for (int k = 0; names[k]; k++) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/emb_mod%d", test_dir, k);
        HlSignature sig;
        ASSERT_EQ(emb_sig(dir, NULL, names[k], &sig), 0);
        HlVfs vfs;
        hl_vfs_init(&vfs, g_emb, dir);
        EXPECT_EQ(hl_sig_verify_files_embedded(&sig, &vfs), -1);
        hl_sig_free(&sig);
    }
}

/* A signed name with nothing embedded under it is refused too. */
UTEST(hl_sig, embedded_missing_signed_file_is_refused)
{
    static const HlEntry fewer[] = {
        ENT("./app", E_APP),
        { 0, 0, 0 }
    };
    char dir[512];
    snprintf(dir, sizeof dir, "%s/emb_missing", test_dir);
    HlSignature sig;
    ASSERT_EQ(emb_sig(dir, NULL, NULL, &sig), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, fewer, dir);
    EXPECT_EQ(hl_sig_verify_files_embedded(&sig, &vfs), -1);
    hl_sig_free(&sig);
}

static void write_file_str(const char *path, const char *s)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(s, f); fclose(f); }
}

/* H2: in filesystem mode a file the runtime would load from disk - an AOT
 * artifact (native code, read before the .wasm), a shader, a template, a
 * static file - planted beside a signed app was never looked at. */
UTEST(hl_sig, fs_planted_loadable_files_are_refused)
{
    static const char *const plant[][2] = {
        { "compute", "compute/score.aot.x86_64" },
        { "compute", "compute/score.wasm" },
        { "shaders", "shaders/evil.wgsl" },
        { "templates", "templates/sub/evil.html" },
        { "static", "static/evil.js" },
    };
    for (size_t k = 0; k < sizeof plant / sizeof plant[0]; k++) {
        char dir[512], p[800];
        snprintf(dir, sizeof dir, "%s/plant%zu", test_dir, k);
        mkdir(dir, 0755);
        snprintf(p, sizeof p, "%s/app.lua", dir);
        write_file_str(p, E_APP);
        char hx[65], files[256];
        sha_hex(E_APP, sizeof(E_APP) - 1, hx);
        snprintf(files, sizeof files, "{\"app.lua\":\"%s\"}", hx);
        create_test_package_sig(dir, files, "null", "deadbeef00", "deadbeef00");
        snprintf(p, sizeof p, "%s/package.sig", dir);
        HlSignature sig;
        ASSERT_EQ(hl_sig_read(p, &sig), 0);
        EXPECT_EQ(hl_sig_verify_files_fs(&sig, dir), 0);

        snprintf(p, sizeof p, "%s/%s", dir, plant[k][0]);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/templates/sub", dir);
        if (k == 3) mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/%s", dir, plant[k][1]);
        write_file_str(p, "planted");
        EXPECT_EQ(hl_sig_verify_files_fs(&sig, dir), -1);
        hl_sig_free(&sig);
    }
}

/* The startup check arms the loaders' disk gate with the signed set: a
 * disk read of a signed file with the signed bytes passes, anything else
 * (an unsigned name, modified bytes) is refused. */
UTEST(hl_sig, verify_startup_arms_the_disk_gate)
{
    char pk_path[512];
    snprintf(pk_path, sizeof(pk_path), "%s/test.pub", test_dir);
    FILE *f = fopen(pk_path, "w");
    ASSERT_TRUE(f != NULL);
    fprintf(f, "%s\n", test_pk_hex);
    fclose(f);

    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", app_hash_hex);
    create_test_package_sig(test_dir, files_json, "null",
                            "binary0000000000000000000000000000000000000000000000000000000000000000",
                            "trampoline00000000000000000000000000000000000000000000000000000000");
    char entry_point[512];
    snprintf(entry_point, sizeof(entry_point), "%s/app.lua", test_dir);
    extern const HlEntry hl_app_entries[];
    HlVfs app_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, test_dir);

    hl_vfs_disk_gate_reset();
    EXPECT_EQ(hl_vfs_disk_gate_check("anything.lua", "x", 1), 0);   /* unarmed */
    ASSERT_EQ(hl_verify_startup(pk_path, entry_point, &app_vfs, 1), 0);
    EXPECT_EQ(hl_vfs_disk_gate_armed(), 1);

    const char *app_content = "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n";
    EXPECT_EQ(hl_vfs_disk_gate_check("app.lua", app_content, strlen(app_content)), 0);
    EXPECT_EQ(hl_vfs_disk_gate_check("./app.lua", app_content, strlen(app_content)), 0);
    EXPECT_EQ(hl_vfs_disk_gate_check("app.lua", "tampered", 8), -1);
    EXPECT_EQ(hl_vfs_disk_gate_check("plugin.lua", app_content, strlen(app_content)), -1);
    EXPECT_EQ(hl_vfs_disk_gate_check("compute/x.aot.x86_64", "x", 1), -1);
    hl_vfs_disk_gate_reset();
}

UTEST(hl_sig, cleanup)
{
    int rc = rmdir_recursive(test_dir);
    ASSERT_EQ(rc, 0);
}

/* Write @p body as package.sig in a fresh directory; read it. */
static int a3_read_raw(const char *body)
{
    /* Its own directory: the fixture's `cleanup` test has removed test_dir
     * by the time these run. */
    char dir[512], path[600];
    if (!hl_test_mkdtemp(dir, sizeof dir, "hull_a3sig")) return -2;
    snprintf(path, sizeof path, "%s/package.sig", dir);
    FILE *f = fopen(path, "w");
    if (!f) return -2;
    fputs(body, f);
    fclose(f);
    HlSignature sig;
    int rc = hl_sig_read(path, &sig);
    if (rc == 0) hl_sig_free(&sig);
    unlink(path);
    rmdir(dir);
    return rc;
}

#define A3_SIG "\"signature\":\"" \
    "00000000000000000000000000000000000000000000000000000000000000000000" \
    "000000000000000000000000000000000000000000000000000000000000\""
#define A3_PK  "\"public_key\":\"" \
    "0000000000000000000000000000000000000000000000000000000000000000\""

/* A legacy (no binary_hash) signature signs only {files, manifest}: a
 * platform block in it is unsigned, and is refused rather than trusted. */
UTEST(hl_sig, legacy_with_a_platform_block_is_refused)
{
    EXPECT_EQ(0, a3_read_raw("{\"files\":{\"app.lua\":\"aa\"}," A3_SIG "," A3_PK "}"));
    EXPECT_EQ(-1, a3_read_raw("{\"files\":{\"app.lua\":\"aa\"},"
                              "\"platform\":{\"platforms\":{}}," A3_SIG "," A3_PK "}"));
    EXPECT_EQ(-1, a3_read_raw("{\"files\":{\"app.lua\":\"aa\"},"
                              "\"modules_resolved\":[]," A3_SIG "," A3_PK "}"));
}

/* A file hash that is not a string was a NULL the file check strcmp'd. */
UTEST(hl_sig, non_string_file_hash_is_refused)
{
    EXPECT_EQ(-1, a3_read_raw("{\"files\":{\"app.lua\":7}," A3_SIG "," A3_PK "}"));
    EXPECT_EQ(-1, a3_read_raw("{\"files\":{\"app.lua\":null}," A3_SIG "," A3_PK "}"));
}

UTEST_MAIN()
