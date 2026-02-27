/*
 * test_signature.c — Tests for hull.sig reading and verification
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

/* ── Helpers ──────────────────────────────────────────────────────── */

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", data[i]);
}

/* Temporary directory for test fixtures */
static char test_dir[256];
static uint8_t test_pk[32];
static uint8_t test_sk[64];
static char test_pk_hex[65];
static char test_sk_hex[129];

/* Create a test hull.sig with known content */
static void create_test_sig(const char *dir, const char *files_json,
                            const char *manifest_json, const uint8_t sk[64],
                            const uint8_t pk[32])
{
    /* Build payload: {"files":...,"manifest":...} */
    char payload[4096];
    snprintf(payload, sizeof(payload),
             "{\"files\":%s,\"manifest\":%s}", files_json, manifest_json);

    /* Sign it */
    uint8_t sig[64];
    hl_cap_crypto_ed25519_sign((const uint8_t *)payload, strlen(payload),
                                sk, sig);

    char sig_hex[129];
    hex_encode(sig, 64, sig_hex);

    char pk_hex[65];
    hex_encode(pk, 32, pk_hex);

    /* Write hull.sig */
    char path[512];
    snprintf(path, sizeof(path), "%s/hull.sig", dir);
    FILE *f = fopen(path, "w");
    fprintf(f,
        "{\"files\":%s,\"manifest\":%s,\"public_key\":\"%s\","
        "\"signature\":\"%s\",\"version\":1}\n",
        files_json, manifest_json, pk_hex, sig_hex);
    fclose(f);
}

/* ── Setup / teardown ──────────────────────────────────────────────── */

UTEST(hl_sig, setup)
{
    /* Create temp dir */
    snprintf(test_dir, sizeof(test_dir), "/tmp/hull_test_sig_XXXXXX");
    ASSERT_NE(mkdtemp(test_dir), (char *)NULL);

    /* Generate test keypair */
    int rc = hl_cap_crypto_ed25519_keypair(test_pk, test_sk);
    ASSERT_EQ(rc, 0);

    hex_encode(test_pk, 32, test_pk_hex);
    hex_encode(test_sk, 64, test_sk_hex);

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

    char hash_hex[65];
    hex_encode(hash, 32, hash_hex);

    /* Create hull.sig with correct hash */
    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", hash_hex);

    create_test_sig(test_dir, files_json, "null", test_sk, test_pk);
}

/* ── Read tests ────────────────────────────────────────────────────── */

UTEST(hl_sig, read_valid)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", test_dir);

    HlSignature sig;
    int rc = hl_sig_read(sig_path, &sig);
    ASSERT_EQ(rc, 0);

    ASSERT_EQ(sig.version, 1);
    ASSERT_NE(sig.files_json, (char *)NULL);
    ASSERT_NE(sig.manifest_json, (char *)NULL);
    ASSERT_NE(sig.signature_hex, (char *)NULL);
    ASSERT_NE(sig.public_key_hex, (char *)NULL);
    ASSERT_EQ((int)strlen(sig.signature_hex), 128);
    ASSERT_EQ((int)strlen(sig.public_key_hex), 64);
    ASSERT_TRUE(sig.entry_count > 0);
    ASSERT_STREQ(sig.entries[0].name, "app.lua");

    hl_sig_free(&sig);
}

UTEST(hl_sig, read_invalid_path)
{
    HlSignature sig;
    int rc = hl_sig_read("/nonexistent/hull.sig", &sig);
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
    /* Write garbage to a file */
    char path[512];
    snprintf(path, sizeof(path), "%s/bad.sig", test_dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "not json at all");
    fclose(f);

    HlSignature sig;
    int rc = hl_sig_read(path, &sig);
    ASSERT_EQ(rc, -1);
}

/* ── Verify tests ──────────────────────────────────────────────────── */

UTEST(hl_sig, verify_good)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    /* Verify with correct key */
    int rc = hl_sig_verify(&sig, test_pk);
    ASSERT_EQ(rc, 0);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_wrong_key)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    /* Verify with different key */
    uint8_t other_pk[32], other_sk[64];
    hl_cap_crypto_ed25519_keypair(other_pk, other_sk);

    int rc = hl_sig_verify(&sig, other_pk);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_bad_sig)
{
    /* Create a hull.sig with tampered signature */
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", test_dir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    /* Tamper with the signature */
    sig.signature_hex[0] = (sig.signature_hex[0] == 'a') ? 'b' : 'a';

    int rc = hl_sig_verify(&sig, test_pk);
    ASSERT_EQ(rc, -1);

    hl_sig_free(&sig);
}

/* ── File hash verification (filesystem) ──────────────────────────── */

UTEST(hl_sig, verify_files_fs_good)
{
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", test_dir);

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
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", test_dir);

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
    /* Create sig referencing a file that doesn't exist */
    char files_json[] = "{\"nonexistent.lua\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/missing_test", test_dir);
    mkdir(subdir, 0755);

    create_test_sig(subdir, files_json, "null", test_sk, test_pk);

    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/hull.sig", subdir);

    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);

    int rc = hl_sig_verify_files_fs(&sig, subdir);
    ASSERT_EQ(rc, -1);

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

    /* Re-create hull.sig with correct hash (may have been tampered by previous test) */
    const char *app_content = "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n";
    uint8_t hash[32];
    hl_cap_crypto_sha256(app_content, strlen(app_content), hash);
    char hash_hex[65];
    hex_encode(hash, 32, hash_hex);
    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", hash_hex);
    create_test_sig(test_dir, files_json, "null", test_sk, test_pk);

    /* Verify */
    char entry_point[512];
    snprintf(entry_point, sizeof(entry_point), "%s/app.lua", test_dir);

    int rc = hl_verify_startup(pk_path, entry_point);
    ASSERT_EQ(rc, 0);
}

UTEST(hl_sig, verify_no_manifest)
{
    /* Test hull.sig without manifest field (Lua nil → key absent) */
    const char *app_content = "app.get(\"/\", function(req, res) res:json({ok=true}) end)\n";
    uint8_t hash[32];
    hl_cap_crypto_sha256(app_content, strlen(app_content), hash);
    char hash_hex[65];
    hex_encode(hash, 32, hash_hex);

    /* Build payload WITHOUT manifest (matching Lua's json.encode({files=...})) */
    char payload[1024];
    char files_json[256];
    snprintf(files_json, sizeof(files_json), "{\"app.lua\":\"%s\"}", hash_hex);
    snprintf(payload, sizeof(payload), "{\"files\":%s}", files_json);

    /* Sign it */
    uint8_t sig_bytes[64];
    hl_cap_crypto_ed25519_sign((const uint8_t *)payload, strlen(payload),
                                test_sk, sig_bytes);
    char sig_hex[129];
    hex_encode(sig_bytes, 64, sig_hex);

    /* Write hull.sig WITHOUT manifest key */
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

    /* Read and verify */
    HlSignature sig;
    ASSERT_EQ(hl_sig_read(sig_path, &sig), 0);
    ASSERT_TRUE(sig.manifest_json == NULL); /* absent, not "null" */

    int rc = hl_sig_verify(&sig, test_pk);
    ASSERT_EQ(rc, 0);

    hl_sig_free(&sig);
}

UTEST(hl_sig, verify_startup_bad_key)
{
    /* Write different pubkey */
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

/* ── Cleanup ──────────────────────────────────────────────────────── */

UTEST(hl_sig, cleanup)
{
    /* Remove test files */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    int rc = system(cmd);
    ASSERT_EQ(rc, 0);
}

UTEST_MAIN()
