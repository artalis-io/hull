/*
 * signature.c — App signature verification (runtime)
 *
 * Reads package.sig (dual-layer JSON) using sh_json arena-allocated parser,
 * verifies Ed25519 signatures for both platform and app layers, and checks
 * file SHA-256 hashes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/signature.h"
#include "hull/cap/crypto.h"

#include "log.h"

#include <sh_json.h>
#include <sh_arena.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VFS: O(log n) lookups into sorted entry arrays */
#include "hull/vfs.h"

/* ── Hex utilities ────────────────────────────────────────────────── */

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, size_t hex_len,
                      uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", data[i]);
}

/* ── Recursive JSON value serializer ─────────────────────────────── */

/*
 * Serialize an ShJsonValue to a ShJsonWriter (canonical form).
 * Used by verify functions to reconstruct the signed payload.
 * Writes "null" for NULL values.
 */
static void sig_write_value(ShJsonWriter *w, const ShJsonValue *v)
{
    if (!v) { sh_json_write_null(w); return; }

    switch (v->type) {
    case SH_JSON_NULL:
        sh_json_write_null(w);
        break;
    case SH_JSON_BOOL:
        sh_json_write_bool(w, v->u.bool_val);
        break;
    case SH_JSON_NUMBER: {
        double d = v->u.num_val;
        int64_t i = (int64_t)d;
        if (d == (double)i && d >= -9007199254740992.0 && d <= 9007199254740992.0)
            sh_json_write_int(w, i);
        else
            sh_json_write_double(w, d);
        break;
    }
    case SH_JSON_STRING:
        sh_json_write_string_n(w, v->u.string_val.str, v->u.string_val.len);
        break;
    case SH_JSON_ARRAY:
        sh_json_write_array_start(w);
        for (size_t i = 0; i < v->u.array_val.count; i++)
            sig_write_value(w, v->u.array_val.items[i]);
        sh_json_write_array_end(w);
        break;
    case SH_JSON_OBJECT:
        sh_json_write_object_start(w);
        for (size_t i = 0; i < v->u.object_val.count; i++) {
            sh_json_write_key(w, v->u.object_val.members[i].key);
            sig_write_value(w, v->u.object_val.members[i].value);
        }
        sh_json_write_object_end(w);
        break;
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_sig_read(const char *sig_path, HlSignature *sig)
{
    if (!sig_path || !sig) return -1;
    memset(sig, 0, sizeof(*sig));

    /* Read the file */
    FILE *f = fopen(sig_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 1024 * 1024) { /* max 1MB */
        fclose(f);
        return -1;
    }

    char *data = malloc((size_t)fsize + 1);
    if (!data) { fclose(f); return -1; }

    size_t nread = fread(data, 1, (size_t)fsize, f);
    fclose(f);
    data[nread] = '\0';

    /* Parse JSON into arena-allocated DOM.
     * Arena needs ~6x input for nodes + strings + alignment padding. */
    size_t arena_size = nread * 8;
    if (arena_size < 4096) arena_size = 4096;
    SHArena *arena = sh_arena_create(arena_size);
    if (!arena) { free(data); return -1; }

    ShJsonValue *root;
    if (sh_json_parse(data, nread, arena, &root) != SH_JSON_OK) {
        sh_arena_free(arena);
        free(data);
        return -1;
    }
    free(data);

    sig->arena = arena;

    /* ── App-layer fields ─────────────────────────────────────── */

    sig->binary_hash_hex = sh_json_as_string(
        sh_json_get(root, "binary_hash"), NULL);
    sig->trampoline_hash_hex = sh_json_as_string(
        sh_json_get(root, "trampoline_hash"), NULL);
    sig->signature_hex = sh_json_as_string(
        sh_json_get(root, "signature"), NULL);
    sig->public_key_hex = sh_json_as_string(
        sh_json_get(root, "public_key"), NULL);

    /* Parsed DOM nodes for canonical reconstruction in verify */
    sig->build_value = sh_json_get(root, "build");
    sig->files_value = sh_json_get(root, "files");
    sig->manifest_value = sh_json_get(root, "manifest");

    /* Required fields */
    if (!sig->signature_hex || !sig->public_key_hex || !sig->files_value) {
        hl_sig_free(sig);
        return -1;
    }

    /* ── Platform layer (nested object) ───────────────────────── */

    ShJsonValue *platform = sh_json_get(root, "platform");
    if (platform && sh_json_type(platform) == SH_JSON_OBJECT) {
        ShJsonValue *platforms = sh_json_get(platform, "platforms");
        sig->platform.platforms_value = platforms;
        sig->platform.signature_hex = sh_json_as_string(
            sh_json_get(platform, "signature"), NULL);
        sig->platform.public_key_hex = sh_json_as_string(
            sh_json_get(platform, "public_key"), NULL);

        /* Parse platform entries from DOM */
        if (platforms && sh_json_type(platforms) == SH_JSON_OBJECT) {
            size_t n = platforms->u.object_val.count;
            if (n > 0 && n <= SIZE_MAX / sizeof(HlPlatformEntry)) {
                sig->platform.entries = calloc(n, sizeof(HlPlatformEntry));
                if (sig->platform.entries) {
                    sig->platform.entry_count = n;
                    for (size_t i = 0; i < n; i++) {
                        const ShJsonMember *m =
                            &platforms->u.object_val.members[i];
                        sig->platform.entries[i].arch = m->key;
                        sig->platform.entries[i].hash_hex =
                            sh_json_as_string(
                                sh_json_get(m->value, "hash"), NULL);
                        sig->platform.entries[i].canary_hex =
                            sh_json_as_string(
                                sh_json_get(m->value, "canary"), NULL);
                    }
                }
            }
        }
    }

    /* Parse file entries from DOM */
    if (sig->files_value->type != SH_JSON_OBJECT) {
        hl_sig_free(sig);
        return -1;
    }

    size_t nfiles = sig->files_value->u.object_val.count;
    if (nfiles > 0) {
        if (nfiles > SIZE_MAX / sizeof(HlSigFileEntry)) {
            hl_sig_free(sig);
            return -1;
        }
        sig->entries = calloc(nfiles, sizeof(HlSigFileEntry));
        if (!sig->entries) {
            hl_sig_free(sig);
            return -1;
        }
        sig->entry_count = nfiles;
        for (size_t i = 0; i < nfiles; i++) {
            const ShJsonMember *m =
                &sig->files_value->u.object_val.members[i];
            sig->entries[i].name = m->key;
            sig->entries[i].hash_hex = sh_json_as_string(m->value, NULL);
        }
    }

    return 0;
}

int hl_sig_verify(const HlSignature *sig, const uint8_t pubkey[32])
{
    if (!sig || !pubkey) return -1;
    if (!sig->signature_hex || !sig->files_value)
        return -1;

    /* Decode signature hex → 64 bytes */
    size_t sig_hex_len = strlen(sig->signature_hex);
    if (sig_hex_len != 128) return -1;

    uint8_t sig_bytes[64];
    if (hex_decode(sig->signature_hex, sig_hex_len, sig_bytes, 64) != 0)
        return -1;

    /*
     * Build canonical payload using ShJsonWriter.
     *
     * New format signs: {binary_hash, build, files, manifest, platform,
     *                    trampoline_hash}
     * Keys in alphabetical order (canonical JSON).
     *
     * For backwards compatibility with old hull.sig (v1), detect by
     * checking whether binary_hash is present.
     */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    if (sig->binary_hash_hex) {
        /* New package.sig format */
        sh_json_write_object_start(&w);

        sh_json_write_kv_string(&w, "binary_hash", sig->binary_hash_hex);

        sh_json_write_key(&w, "build");
        sig_write_value(&w, sig->build_value);

        sh_json_write_key(&w, "files");
        sig_write_value(&w, sig->files_value);

        sh_json_write_key(&w, "manifest");
        sig_write_value(&w, sig->manifest_value);

        sh_json_write_key(&w, "platform");
        if (sig->platform.platforms_value &&
            sig->platform.signature_hex &&
            sig->platform.public_key_hex) {
            sh_json_write_object_start(&w);
            sh_json_write_key(&w, "platforms");
            sig_write_value(&w, sig->platform.platforms_value);
            sh_json_write_kv_string(&w, "public_key",
                                    sig->platform.public_key_hex);
            sh_json_write_kv_string(&w, "signature",
                                    sig->platform.signature_hex);
            sh_json_write_object_end(&w);
        } else {
            sh_json_write_null(&w);
        }

        sh_json_write_kv_string(&w, "trampoline_hash",
            sig->trampoline_hash_hex ? sig->trampoline_hash_hex : "");

        sh_json_write_object_end(&w);
    } else {
        /* Legacy hull.sig format: {"files":...} or {"files":...,"manifest":...} */
        sh_json_write_object_start(&w);

        sh_json_write_key(&w, "files");
        sig_write_value(&w, sig->files_value);

        if (sig->manifest_value) {
            sh_json_write_key(&w, "manifest");
            sig_write_value(&w, sig->manifest_value);
        }

        sh_json_write_object_end(&w);
    }

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return -1;
    }

    /* Verify Ed25519 signature */
    int rc = hl_cap_crypto_ed25519_verify(
        (const uint8_t *)jb.buf, jb.len, sig_bytes, pubkey);

    sh_json_buf_free(&jb);
    return rc;
}

int hl_sig_verify_platform(const HlSignature *sig, const uint8_t pubkey[32])
{
    if (!sig || !pubkey) return -1;
    if (!sig->platform.signature_hex || !sig->platform.platforms_value)
        return -1;

    /* Decode signature hex → 64 bytes */
    size_t sig_hex_len = strlen(sig->platform.signature_hex);
    if (sig_hex_len != 128) return -1;

    uint8_t sig_bytes[64];
    if (hex_decode(sig->platform.signature_hex, sig_hex_len, sig_bytes, 64) != 0)
        return -1;

    /* Serialize platforms value to canonical JSON */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);
    sig_write_value(&w, sig->platform.platforms_value);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return -1;
    }

    int rc = hl_cap_crypto_ed25519_verify(
        (const uint8_t *)jb.buf, jb.len, sig_bytes, pubkey);

    sh_json_buf_free(&jb);
    return rc;
}

/* Search the VFS for a file matching sig_name.
 * For .lua entries, the embedded name has the extension stripped,
 * so we try both "./sig_name" and "./sig_name_without_lua". */
static int sig_find_in_entries(const HlVfs *vfs, const char *sig_name,
                               const unsigned char **out_data,
                               unsigned int *out_len)
{
    /* Try "./sig_name" first (exact match for .js/.json) */
    char lookup[1024];
    int n = snprintf(lookup, sizeof(lookup), "./%s", sig_name);
    if (n < 0 || (size_t)n >= sizeof(lookup))
        return 0;

    const HlEntry *e = hl_vfs_find(vfs, lookup);

    /* For .lua files, embedded name has extension stripped */
    if (!e) {
        size_t slen = strlen(sig_name);
        if (slen > 4 && strcmp(sig_name + slen - 4, ".lua") == 0) {
            int m = snprintf(lookup, sizeof(lookup), "./%.*s",
                             (int)(slen - 4), sig_name);
            if (m > 0 && (size_t)m < sizeof(lookup))
                e = hl_vfs_find(vfs, lookup);
        }
    }

    if (e) {
        *out_data = e->data;
        *out_len = e->len;
        return 1;
    }
    return 0;
}

/* Check whether an embedded entry name matches any signature entry. */
static int sig_entry_in_sig(const HlSignature *sig, const char *ename)
{
    for (size_t i = 0; i < sig->entry_count; i++) {
        const char *sig_name = sig->entries[i].name;
        size_t slen = strlen(sig_name);
        size_t mlen = slen;
        if (slen > 4 && strcmp(sig_name + slen - 4, ".lua") == 0)
            mlen = slen - 4;

        if (strlen(ename) == mlen && strncmp(ename, sig_name, mlen) == 0)
            return 1;
    }
    return 0;
}

int hl_sig_verify_files_embedded(const HlSignature *sig, const HlVfs *vfs)
{
    if (!sig || !sig->entries || !vfs) return -1;

    for (size_t i = 0; i < sig->entry_count; i++) {
        const char *sig_name = sig->entries[i].name;
        const char *expected_hash = sig->entries[i].hash_hex;

        const unsigned char *data = NULL;
        unsigned int data_len = 0;

        if (!sig_find_in_entries(vfs, sig_name, &data, &data_len)) {
            log_error("[sig] file not found in binary: %s", sig_name);
            return -1;
        }

        uint8_t hash[32];
        if (hl_cap_crypto_sha256(data, data_len, hash) != 0) return -1;

        char hash_hex[65];
        hex_encode(hash, 32, hash_hex);

        if (strcmp(hash_hex, expected_hash) != 0) {
            log_error("[sig] hash mismatch for %s", sig_name);
            return -1;
        }
    }

    /* Check for extra module files in the binary not in the signature */
    const HlEntry *mod_first = NULL;
    size_t mod_count = hl_vfs_prefix(vfs, "./", &mod_first);
    for (size_t i = 0; i < mod_count; i++) {
        const char *ename = mod_first[i].name;
        if (ename[0] == '.' && ename[1] == '/')
            ename += 2;

        if (!sig_entry_in_sig(sig, ename)) {
            log_error("[sig] extra file in binary not in signature: %s",
                      mod_first[i].name);
            return -1;
        }
    }

    return 0;
}

int hl_sig_verify_files_fs(const HlSignature *sig, const char *app_dir)
{
    if (!sig || !sig->entries || !app_dir) return -1;

    for (size_t i = 0; i < sig->entry_count; i++) {
        const char *name = sig->entries[i].name;
        const char *expected_hash = sig->entries[i].hash_hex;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", app_dir, name);

        FILE *f = fopen(path, "rb");
        if (!f) {
            log_error("[sig] cannot open file: %s", path);
            return -1;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fsize < 0 || fsize > 100 * 1024 * 1024) {
            fclose(f);
            log_error("[sig] file too large: %s", path);
            return -1;
        }

        char *data = malloc((size_t)fsize);
        if (!data) { fclose(f); return -1; }

        size_t nread = fread(data, 1, (size_t)fsize, f);
        fclose(f);

        uint8_t hash[32];
        if (hl_cap_crypto_sha256(data, nread, hash) != 0) {
            free(data);
            return -1;
        }
        free(data);

        char hash_hex[65];
        hex_encode(hash, 32, hash_hex);

        if (strcmp(hash_hex, expected_hash) != 0) {
            log_error("[sig] hash mismatch: %s (expected %s, got %s)",
                      name, expected_hash, hash_hex);
            return -1;
        }
    }

    return 0;
}

void hl_sig_free(HlSignature *sig)
{
    if (!sig) return;

    /* Free calloc'd entry arrays (entries point into arena) */
    free(sig->platform.entries);
    free(sig->entries);

    /* Free arena — all parsed strings and DOM nodes live here */
    if (sig->arena)
        sh_arena_free(sig->arena);

    memset(sig, 0, sizeof(*sig));
}

/* ── Full startup verification ────────────────────────────────────── */

int hl_verify_startup(const char *pubkey_path, const char *entry_point,
                      const HlVfs *app_vfs)
{
    if (!pubkey_path || !entry_point) return -1;

    /* 1. Read developer pubkey file (64 hex chars → 32 bytes) */
    FILE *f = fopen(pubkey_path, "r");
    if (!f) {
        log_error("[sig] cannot open pubkey: %s", pubkey_path);
        return -1;
    }

    char pk_hex[128];
    memset(pk_hex, 0, sizeof(pk_hex));
    if (!fgets(pk_hex, sizeof(pk_hex), f)) {
        fclose(f);
        log_error("[sig] cannot read pubkey: %s", pubkey_path);
        return -1;
    }
    fclose(f);

    /* Strip trailing whitespace */
    size_t pk_len = strlen(pk_hex);
    while (pk_len > 0 && (pk_hex[pk_len - 1] == '\n' ||
                           pk_hex[pk_len - 1] == '\r' ||
                           pk_hex[pk_len - 1] == ' '))
        pk_hex[--pk_len] = '\0';

    if (pk_len != 64) {
        log_error("[sig] invalid pubkey length: %zu (expected 64 hex chars)",
                  pk_len);
        return -1;
    }

    uint8_t pubkey[32];
    if (hex_decode(pk_hex, 64, pubkey, 32) != 0) {
        log_error("[sig] invalid pubkey hex");
        return -1;
    }

    /* 2. Derive sig path from entry_point directory */
    const char *slash = strrchr(entry_point, '/');
    char sig_path[PATH_MAX];

    /* Try package.sig first, fall back to hull.sig for backwards compat */
    if (slash) {
        size_t dir_len = (size_t)(slash - entry_point);
        snprintf(sig_path, sizeof(sig_path), "%.*s/package.sig",
                 (int)dir_len, entry_point);
    } else {
        snprintf(sig_path, sizeof(sig_path), "package.sig");
    }

    /* Fall back to hull.sig if package.sig doesn't exist */
    FILE *test_f = fopen(sig_path, "r");
    if (!test_f) {
        if (slash) {
            size_t dir_len = (size_t)(slash - entry_point);
            snprintf(sig_path, sizeof(sig_path), "%.*s/hull.sig",
                     (int)dir_len, entry_point);
        } else {
            snprintf(sig_path, sizeof(sig_path), "hull.sig");
        }
    } else {
        fclose(test_f);
    }

    /* 3. Read and parse signature */
    HlSignature sig;
    if (hl_sig_read(sig_path, &sig) != 0) {
        log_error("[sig] cannot read signature: %s", sig_path);
        return -1;
    }

    /* 4. Verify developer pubkey matches */
    if (sig.public_key_hex) {
        size_t hex_len = strlen(sig.public_key_hex);
        if (hex_len != 64 || strncmp(sig.public_key_hex, pk_hex, 64) != 0) {
            log_error("[sig] pubkey mismatch: --verify-sig key differs from signature");
            hl_sig_free(&sig);
            return -1;
        }
    }

    /* 5. Verify platform signature if present */
    if (sig.platform.signature_hex && sig.platform.public_key_hex) {
        /* Decode platform public key */
        uint8_t platform_pk[32];
        if (strlen(sig.platform.public_key_hex) == 64 &&
            hex_decode(sig.platform.public_key_hex, 64, platform_pk, 32) == 0) {

            /* Pin against compiled-in platform key */
            uint8_t expected_pk[32];
            if (hex_decode(HL_PLATFORM_PUBKEY_HEX, 64, expected_pk, 32) == 0) {
                /* All-zeros means placeholder — skip pinning check */
                int all_zero = 1;
                for (int i = 0; i < 32; i++) {
                    if (expected_pk[i] != 0) { all_zero = 0; break; }
                }
                if (!all_zero &&
                    memcmp(platform_pk, expected_pk, 32) != 0) {
                    log_error("[sig] platform key does not match compiled-in key");
                    hl_sig_free(&sig);
                    return -1;
                }
            }

            if (hl_sig_verify_platform(&sig, platform_pk) != 0) {
                log_error("[sig] platform signature verification failed");
                hl_sig_free(&sig);
                return -1;
            }
        }
    }

    /* 6. Verify app Ed25519 signature */
    if (hl_sig_verify(&sig, pubkey) != 0) {
        log_error("[sig] Ed25519 signature verification failed");
        hl_sig_free(&sig);
        return -1;
    }

    /* 7. Verify file hashes: embedded or filesystem */
    int rc;
    if (app_vfs->count > 0) {
        rc = hl_sig_verify_files_embedded(&sig, app_vfs);
    } else {
        char app_dir[PATH_MAX];
        if (slash) {
            size_t dir_len = (size_t)(slash - entry_point);
            snprintf(app_dir, sizeof(app_dir), "%.*s",
                     (int)dir_len, entry_point);
        } else {
            snprintf(app_dir, sizeof(app_dir), ".");
        }
        rc = hl_sig_verify_files_fs(&sig, app_dir);
    }

    if (rc != 0) {
        log_error("[sig] file hash verification failed");
        hl_sig_free(&sig);
        return -1;
    }

    hl_sig_free(&sig);
    return 0;
}
