/*
 * test_signature.c — Tests for package.sig reading and verification
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/signature.h"
#include "hull/cap/crypto.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

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
    snprintf(test_dir, sizeof(test_dir), "/tmp/hull_test_sig_XXXXXX");
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

    ASSERT_NE(sig.files_json, (char *)NULL);
    ASSERT_NE(sig.signature_hex, (char *)NULL);
    ASSERT_NE(sig.public_key_hex, (char *)NULL);
    ASSERT_NE(sig.binary_hash_hex, (char *)NULL);
    ASSERT_NE(sig.trampoline_hash_hex, (char *)NULL);
    ASSERT_NE(sig.build_json, (char *)NULL);
    ASSERT_EQ((int)strlen(sig.signature_hex), 128);
    ASSERT_EQ((int)strlen(sig.public_key_hex), 64);
    ASSERT_TRUE(sig.entry_count > 0);
    ASSERT_STREQ(sig.entries[0].name, "app.lua");

    /* Platform layer */
    ASSERT_NE(sig.platform.platforms_json, (char *)NULL);
    ASSERT_NE(sig.platform.signature_hex, (char *)NULL);
    ASSERT_NE(sig.platform.public_key_hex, (char *)NULL);
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

    /* Tamper with the signature */
    sig.signature_hex[0] = (sig.signature_hex[0] == 'a') ? 'b' : 'a';

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

    /* Tamper with platform signature */
    sig.platform.signature_hex[0] =
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
    ASSERT_TRUE(sig.manifest_json == NULL);

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

    int rc = hl_verify_startup(pk_path, entry_point);
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

    int rc = hl_verify_startup(pk_path, entry_point);
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

UTEST(hl_sig, cleanup)
{
    int rc = rmdir_recursive(test_dir);
    ASSERT_EQ(rc, 0);
}

UTEST_MAIN()
