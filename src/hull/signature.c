/*
 * signature.c - App signature verification (runtime)
 *
 * Reads package.sig (dual-layer JSON) using sh_json arena-allocated parser,
 * verifies Ed25519 signatures for both platform and app layers, and checks
 * file SHA-256 hashes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/signature.h"
#include "hull/cap/crypto.h"
#include "hull/platform_sig.h"
#include "utils/hex.h"

#include "log.h"

#include <sh_json.h>
#include <sh_arena.h>

#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

/* VFS: O(log n) lookups into sorted entry arrays */
#include "hull/vfs.h"

/* ── Hex utilities ────────────────────────────────────────────────── */

/* hex decode goes through the canonical hl_hex_decode (utils/hex.h).
 * It returns the byte count written (out_size treated as a capacity), so a
 * success check is `rc == N` where N is the expected byte count: that requires
 * hex_len/2 == N exactly (a short input returns <N; a long one returns -1 for
 * insufficient capacity), matching the exact-length fail-closed contract these
 * Ed25519 verify paths need.
 *
 * hex encode goes through the shared byte->hex leaf hl_hex_encode (utils/hex.h). */

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

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long fsize = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    if (fsize <= 0 || fsize > 1024 * 1024) { /* max 1MB */
        fclose(f);
        return -1;
    }

    char *data = malloc((size_t)fsize + 1);
    if (!data) { fclose(f); return -1; }

    size_t nread = fread(data, 1, (size_t)fsize, f);
    /* L3: capture read error before fclose() invalidates the stream. */
    int read_err = ferror(f);
    fclose(f);
    if (read_err || nread != (size_t)fsize) {
        free(data);
        return -1;
    }
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
    sig->modules_resolved_value = sh_json_get(root, "modules_resolved");

    /* Required fields */
    if (!sig->signature_hex || !sig->public_key_hex || !sig->files_value) {
        hl_sig_free(sig);
        return -1;
    }

    /* A legacy hull.sig (no binary_hash) signs only {files, manifest}. Any
     * field of the newer format in it - a platform block, the composed
     * attestation inside it, modules_resolved, a trampoline hash - is not
     * covered by its signature, yet was read and trusted: a platform block
     * lifted from another app, attached to an old signature, passed for
     * this one. Refused. */
    if (!sig->binary_hash_hex &&
        (sh_json_get(root, "platform") || sh_json_get(root, "modules_resolved") ||
         sig->trampoline_hash_hex || sig->build_value)) {
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

        /* v0.1.3: gethull-signed manifest blob (written by `hull build`
         * when the building hull had an embedded platform-sig). Strings
         * point into the arena; safe across the function's lifetime.
         * The whole DOM subtree is also retained so the app-sig
         * canonical-JSON reconstruction (hl_sig_verify) can emit the
         * gethull block byte-identically to what build.lua signed. */
        ShJsonValue *gethull = sh_json_get(platform, "gethull");
        if (gethull && sh_json_type(gethull) == SH_JSON_OBJECT) {
            sig->platform.gethull_value = gethull;
            sig->platform.gethull_manifest = sh_json_as_string(
                sh_json_get(gethull, "manifest"), NULL);
            if (sig->platform.gethull_manifest) {
                sig->platform.gethull_manifest_len =
                    strlen(sig->platform.gethull_manifest);
            }
            sig->platform.gethull_signature_hex = sh_json_as_string(
                sh_json_get(gethull, "signature"), NULL);
            if (sig->platform.gethull_signature_hex) {
                sig->platform.gethull_signature_hex_len =
                    strlen(sig->platform.gethull_signature_hex);
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
            /* A hash that is not a string was NULL here, and the file check
             * strcmp'd against it - a crash on a hostile package.sig. */
            if (!sig->entries[i].name || !sig->entries[i].hash_hex) {
                hl_sig_free(sig);
                return -1;
            }
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
    if (hl_hex_decode(sig->signature_hex, sig_hex_len, sig_bytes, 64) != 64)
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

        /* modules_resolved: array of {name, api_major, intrinsic} entries
         * captured at build time. Optional - when absent (built with an
         * older hull), the canonical reconstruction omits the key
         * entirely so existing signatures still verify. */
        if (sig->modules_resolved_value) {
            sh_json_write_key(&w, "modules_resolved");
            sig_write_value(&w, sig->modules_resolved_value);
        }

        sh_json_write_key(&w, "platform");
        if (sig->platform.platforms_value &&
            sig->platform.signature_hex &&
            sig->platform.public_key_hex) {
            sh_json_write_object_start(&w);
            /* v0.1.3: gethull subtree MUST emit first (alphabetical
             * canonical order: gethull < platforms < public_key <
             * signature). build.lua signs the whole platform object
             * including the gethull block, so the verifier has to
             * reconstruct byte-identical input. Walking the parsed
             * DOM (sig_write_value) preserves whatever internal
             * shape the build wrote - arch_hashes (object), manifest
             * (string), signature (string) - without the verifier
             * needing to know those names. */
            if (sig->platform.gethull_value) {
                sh_json_write_key(&w, "gethull");
                sig_write_value(&w, sig->platform.gethull_value);
            }
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
    if (hl_hex_decode(sig->platform.signature_hex, sig_hex_len, sig_bytes, 64) != 64)
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

/* The signed entry an embedded VFS entry corresponds to, or -1.
 *
 * hull build signs every file under its app-relative path ("app.lua",
 * "lib/x.lua", "data.json", "templates/a.html", "compute/s.wasm",
 * "compute/s.aot.x86_64") but embeds them under two naming schemes:
 *   - Lua modules as "./<path without .lua>", JS / JSON as "./<path>";
 *   - everything else (templates/, static/, migrations/, compute/ incl.
 *     AOT, shaders/) under the bare app-relative path.
 * So a "./" entry matches its rest exactly or with ".lua" appended, and a
 * bare entry matches only exactly. (The verifier used to look up "./" forms
 * only, so every built app with a template, static file, migration, compute
 * module or shader failed --verify-sig with "file not found in binary".) */
static long sig_signed_index(const HlSignature *sig, const char *ename)
{
    if (ename[0] == '.' && ename[1] == '/') {
        const char *rest = ename + 2;
        size_t rl = strlen(rest);
        for (size_t i = 0; i < sig->entry_count; i++)
            if (strcmp(sig->entries[i].name, rest) == 0)
                return (long)i;
        for (size_t i = 0; i < sig->entry_count; i++) {
            const char *sn = sig->entries[i].name;
            if (strlen(sn) == rl + 4 && strncmp(sn, rest, rl) == 0 &&
                strcmp(sn + rl, ".lua") == 0)
                return (long)i;
        }
        return -1;
    }
    for (size_t i = 0; i < sig->entry_count; i++)
        if (strcmp(sig->entries[i].name, ename) == 0)
            return (long)i;
    return -1;
}

int hl_sig_verify_files_embedded(const HlSignature *sig, const HlVfs *vfs)
{
    if (!sig || !sig->entries || !vfs) return -1;
    if (sig->entry_count > (size_t)LONG_MAX) return -1;

    /* Driven by the EMBEDDED entries: each must map to a signed name and
     * hash to it, so no embedded byte goes unchecked (two entries that map
     * to one signed name are both hashed). Then every signed name must have
     * been seen. */
    unsigned char *seen = calloc(sig->entry_count ? sig->entry_count : 1, 1);
    if (!seen) return -1;

    int rc = 0;
    for (size_t i = 0; i < vfs->count; i++) {
        const HlEntry *e = &vfs->entries[i];
        if (!e->name) continue;

        long si = sig_signed_index(sig, e->name);
        if (si < 0) {
            log_error("[sig] extra file in binary not in signature: %s", e->name);
            rc = -1;
            break;
        }

        uint8_t hash[32];
        if (hl_cap_crypto_sha256(e->data ? (const void *)e->data : "",
                                 e->len, hash) != 0) {
            rc = -1;
            break;
        }
        char hash_hex[65];
        hl_hex_encode(hash, 32, hash_hex, sizeof(hash_hex));
        if (strcmp(hash_hex, sig->entries[si].hash_hex) != 0) {
            log_error("[sig] hash mismatch for %s", sig->entries[si].name);
            rc = -1;
            break;
        }
        seen[si] = 1;
    }

    for (size_t i = 0; rc == 0 && i < sig->entry_count; i++) {
        if (!seen[i]) {
            log_error("[sig] file not found in binary: %s", sig->entries[i].name);
            rc = -1;
        }
    }

    free(seen);
    return rc;
}

/* Every file under app_dir/<rel_dir> (recursively when `recurse`) whose name
 * passes `want` must be a signed one. Returns 0, or -1 (logged). */
static int sig_scan_unsigned(const HlSignature *sig, const char *app_dir,
                             const char *rel_dir, int recurse,
                             int (*want)(const char *name), int depth)
{
    if (depth > 16) {
        log_error("[sig] %s: directory nesting too deep", rel_dir);
        return -1;
    }
    char dir[PATH_MAX];
    if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", app_dir, rel_dir) >= sizeof(dir))
        return -1;
    DIR *d = opendir(dir);
    if (!d)
        return 0;   /* no such directory: nothing to load from it */

    int bad = 0;
    struct dirent *de;
    while (!bad && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char rel[PATH_MAX];
        if ((size_t)snprintf(rel, sizeof(rel), "%s/%s", rel_dir, de->d_name) >= sizeof(rel)) {
            bad = 1;
            break;
        }
        char full[PATH_MAX];
        if ((size_t)snprintf(full, sizeof(full), "%s/%s", app_dir, rel) >= sizeof(full)) {
            bad = 1;
            break;
        }
        struct stat st;
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (recurse && sig_scan_unsigned(sig, app_dir, rel, recurse, want,
                                             depth + 1) != 0)
                bad = 1;
            continue;
        }
        if (want && !want(de->d_name))
            continue;
        int signed_one = 0;
        for (size_t i = 0; i < sig->entry_count; i++)
            if (strcmp(sig->entries[i].name, rel) == 0) { signed_one = 1; break; }
        if (!signed_one) {
            log_error("[sig] file not in signature: %s", rel);
            bad = 1;
        }
    }
    closedir(d);
    return bad ? -1 : 0;
}

static int sig_want_sql(const char *n)
{
    size_t l = strlen(n);
    return l >= 5 && strcmp(n + l - 4, ".sql") == 0;
}

/* compute/<name>.wasm and compute/<name>.aot.<arch>: what hl_cap_wasm_load
 * reads from disk. (compute/<name>/ holds the module's C source.) */
static int sig_want_compute(const char *n)
{
    size_t l = strlen(n);
    return (l >= 6 && strcmp(n + l - 5, ".wasm") == 0) || strstr(n, ".aot.") != NULL;
}

/* Arm the loaders' disk gate (hl_vfs_disk_gate_*) with the signed set:
 * from here on, every app file read from disk is re-hashed against it. */
static int sig_arm_disk_gate(const HlSignature *sig)
{
    size_t n = sig->entry_count;
    const char **names = n ? calloc(n, sizeof(*names)) : NULL;
    uint8_t (*digests)[32] = n ? calloc(n, sizeof(*digests)) : NULL;
    int rc = -1;
    if (n == 0 || (names && digests)) {
        rc = 0;
        for (size_t i = 0; i < n; i++) {
            names[i] = sig->entries[i].name;
            if (!sig->entries[i].hash_hex ||
                strlen(sig->entries[i].hash_hex) != 64 ||
                hl_hex_decode(sig->entries[i].hash_hex, 64, digests[i], 32) != 32) {
                rc = -1;
                break;
            }
        }
    }
    if (rc == 0)
        rc = hl_vfs_disk_gate_arm(names, (const uint8_t (*)[32])digests, n,
                                  hl_cap_crypto_sha256);
    else
        (void)hl_vfs_disk_gate_arm(NULL, NULL, 0, hl_cap_crypto_sha256); /* armed empty */
    free(names);
    free(digests);
    return rc;
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
            /* An AOT artifact is made in the build's tmpdir and only ever
             * embedded: an unbuilt app's tree does not have it. */
            if (strncmp(name, "compute/", 8) == 0 && strstr(name, ".aot.") &&
                errno == ENOENT)
                continue;
            log_error("[sig] cannot open file: %s", path);
            return -1;
        }

        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
        long fsize = ftell(f);
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

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
        hl_hex_encode(hash, 32, hash_hex, sizeof(hash_hex));

        if (strcmp(hash_hex, expected_hash) != 0) {
            log_error("[sig] hash mismatch: %s (expected %s, got %s)",
                      name, expected_hash, hash_hex);
            return -1;
        }
    }

    /* Every file the runtime would load from these directories must be a
     * signed one. The migration runner applies EVERY *.sql in migrations/,
     * and the compute loader reads compute/<name>.aot.<arch> before
     * compute/<name>.wasm, so a file dropped there beside a signed app ran
     * (AOT: native code outside the WASM sandbox) although --verify-sig
     * passed. Templates, static files and shaders are named at run time,
     * possibly from input. The loaders also re-check each file they read
     * against the signature (hl_vfs_disk_gate_check, armed by the caller),
     * which covers modules and closes the hash-then-reload window; this
     * scan refuses a planted file up front. */
    if (sig_scan_unsigned(sig, app_dir, "migrations", 0, sig_want_sql, 0) != 0 ||
        sig_scan_unsigned(sig, app_dir, "compute", 0, sig_want_compute, 0) != 0 ||
        sig_scan_unsigned(sig, app_dir, "shaders", 1, NULL, 0) != 0 ||
        sig_scan_unsigned(sig, app_dir, "templates", 1, NULL, 0) != 0 ||
        sig_scan_unsigned(sig, app_dir, "static", 1, NULL, 0) != 0)
        return -1;

    return 0;
}

void hl_sig_free(HlSignature *sig)
{
    if (!sig) return;

    /* Free calloc'd entry arrays (entries point into arena) */
    free(sig->platform.entries);
    free(sig->entries);

    /* Free arena - all parsed strings and DOM nodes live here */
    if (sig->arena)
        sh_arena_free(sig->arena);

    memset(sig, 0, sizeof(*sig));
}

/* ── Full startup verification ────────────────────────────────────── */

/* ── The verified policy (M13) ─────────────────────────────────────
 *
 * What --verify-sig verified, kept for the runtime check that follows app
 * load: the signed `manifest` and `modules_resolved`, as canonical JSON.
 * Captured from the very signature that was verified (re-reading
 * package.sig later would be a race), once per process. */
static char *g_verified_manifest;   /* NULL: no manifest was signed */
static size_t g_verified_manifest_len;
static char *g_verified_modules;
static size_t g_verified_modules_len;
static int   g_verified_captured;

static char *sig_value_json(const ShJsonValue *v, size_t *out_len)
{
    *out_len = 0;
    if (!v || sh_json_type(v) == SH_JSON_NULL) return NULL;
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);
    sig_write_value(&w, v);
    if (sh_json_writer_error(&w) || !jb.buf) { sh_json_buf_free(&jb); return NULL; }
    char *out = malloc(jb.len + 1);
    if (out) { memcpy(out, jb.buf, jb.len); out[jb.len] = '\0'; *out_len = jb.len; }
    sh_json_buf_free(&jb);
    return out;
}

static void sig_capture_policy(const HlSignature *sig)
{
    free(g_verified_manifest);
    free(g_verified_modules);
    g_verified_manifest = sig_value_json(sig->manifest_value, &g_verified_manifest_len);
    g_verified_modules  = sig_value_json(sig->modules_resolved_value, &g_verified_modules_len);
    g_verified_captured = 1;
}

/* Structural JSON equality: objects compare by key set, not order. */
static int json_equal(const ShJsonValue *a, const ShJsonValue *b, int depth)
{
    if (depth > 64) return 0;
    ShJsonType ta = sh_json_type(a), tb = sh_json_type(b);
    if (ta != tb) return 0;
    switch (ta) {
    case SH_JSON_NULL:   return 1;
    case SH_JSON_BOOL:   return a->u.bool_val == b->u.bool_val;
    case SH_JSON_NUMBER: return a->u.num_val == b->u.num_val;
    case SH_JSON_STRING:
        return a->u.string_val.len == b->u.string_val.len &&
               memcmp(a->u.string_val.str, b->u.string_val.str,
                      a->u.string_val.len) == 0;
    case SH_JSON_ARRAY:
        if (a->u.array_val.count != b->u.array_val.count) return 0;
        for (size_t i = 0; i < a->u.array_val.count; i++)
            if (!json_equal(a->u.array_val.items[i], b->u.array_val.items[i], depth + 1))
                return 0;
        return 1;
    case SH_JSON_OBJECT:
        if (a->u.object_val.count != b->u.object_val.count) return 0;
        for (size_t i = 0; i < a->u.object_val.count; i++) {
            const ShJsonMember *m = &a->u.object_val.members[i];
            ShJsonValue *o = sh_json_get_n(b, m->key, m->key_len);
            if (!o || !json_equal(m->value, o, depth + 1)) return 0;
        }
        return 1;
    }
    return 0;
}

int hl_sig_check_runtime_policy(const char *manifest_json, size_t manifest_len,
                                HlSigHasModuleFn has_module, void *ud,
                                int module_count, char *err, size_t err_size)
{
    if (!g_verified_captured) {
        snprintf(err, err_size, "no verified signature to check the policy against");
        return -1;
    }
    size_t asz = 4096 + 8 * (manifest_len + g_verified_manifest_len +
                             g_verified_modules_len);
    SHArena *arena = sh_arena_create(asz);
    if (!arena) { snprintf(err, err_size, "out of memory"); return -1; }
    int rc = -1;

    /* 1. The manifest the app runs with is the one that was signed. */
    if (!manifest_json != !g_verified_manifest) {
        snprintf(err, err_size, "the app %s a manifest but the signed package %s",
                 manifest_json ? "declares" : "does not declare",
                 g_verified_manifest ? "has one" : "has none");
        goto out;
    }
    if (manifest_json) {
        ShJsonValue *rt = NULL, *sg = NULL;
        if (sh_json_parse(manifest_json, manifest_len, arena, &rt) != SH_JSON_OK ||
            sh_json_parse(g_verified_manifest, g_verified_manifest_len, arena, &sg) != SH_JSON_OK) {
            snprintf(err, err_size, "manifest JSON unreadable");
            goto out;
        }
        if (!json_equal(rt, sg, 0)) {
            snprintf(err, err_size, "the manifest the app declared at run time "
                     "differs from the signed one");
            goto out;
        }
    }

    /* 2. And so is its resolved module surface. */
    if (g_verified_modules && has_module) {
        ShJsonValue *mods = NULL;
        if (sh_json_parse(g_verified_modules, g_verified_modules_len, arena, &mods) != SH_JSON_OK ||
            sh_json_type(mods) != SH_JSON_ARRAY) {
            snprintf(err, err_size, "signed modules_resolved unreadable");
            goto out;
        }
        size_t nsig = mods->u.array_val.count;
        for (size_t i = 0; i < nsig; i++) {
            ShJsonValue *e = mods->u.array_val.items[i];
            const char *name = sh_json_type(e) == SH_JSON_OBJECT
                ? sh_json_as_string(sh_json_get(e, "name"), NULL)
                : sh_json_as_string(e, NULL);
            if (!name || !has_module(ud, name)) {
                snprintf(err, err_size, "signed module '%s' is not resolved at run time",
                         name ? name : "?");
                goto out;
            }
        }
        if (module_count < 0 || (size_t)module_count != nsig) {
            snprintf(err, err_size, "the run-time module set (%d) differs from the "
                     "signed modules_resolved (%zu)", module_count, nsig);
            goto out;
        }
    }
    rc = 0;
out:
    sh_arena_free(arena);
    return rc;
}

int hl_verify_startup(const char *pubkey_path, const char *entry_point,
                      const HlVfs *app_vfs, int no_verify_platform)
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
    if (hl_hex_decode(pk_hex, 64, pubkey, 32) != 32) {
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

    /* 4b. The app signature, before anything below reads the platform blocks.
     * It covers `platform` only when all of platforms / public_key /
     * signature are present; otherwise the signed payload says null. A
     * package.sig without them but WITH a gethull block (or its composed
     * attestation) carried that block unsigned - anyone could paste in a
     * genuine release's gethull manifest - so that shape is refused. */
    if (sig.platform.gethull_value &&
        !(sig.platform.platforms_value && sig.platform.signature_hex &&
          sig.platform.public_key_hex)) {
        log_error("[sig] platform.gethull is outside the app signature "
                  "(platform has no platforms/public_key/signature)");
        hl_sig_free(&sig);
        return -1;
    }
    if (hl_sig_verify(&sig, pubkey) != 0) {
        log_error("[sig] Ed25519 signature verification failed");
        hl_sig_free(&sig);
        return -1;
    }

    /* 5. v0.1.2 per-app platform layer (self-consistency only).
     *
     * The developer's `hull sign-platform` workflow signed the
     * platforms object with whatever key the developer chose locally.
     * We verify that signature is self-consistent (sig matches
     * platform.public_key) but no longer pin platform.public_key
     * against HL_PLATFORM_PUBKEY_HEX - that pinning moved to §5b
     * (v0.1.3 gethull layer), which uses a signed manifest the gethull
     * release pipeline produces. Forks and self-signing devs continue
     * to work without conflicting with the upstream gethull pubkey. */
    if (sig.platform.signature_hex && sig.platform.public_key_hex) {
        uint8_t platform_pk[32];
        if (strlen(sig.platform.public_key_hex) == 64 &&
            hl_hex_decode(sig.platform.public_key_hex, 64, platform_pk, 32) == 32) {
            if (hl_sig_verify_platform(&sig, platform_pk) != 0) {
                log_error("[sig] platform signature verification failed");
                hl_sig_free(&sig);
                return -1;
            }
        }
    }

    /* 5b. v0.1.3 gethull platform-sig layer.
     *
     * The gethull manifest+signature embedded by `hull build` proves
     * the platform .a's hash was signed by the gethull.dev platform
     * key at release time. Verify the signature against the embedded
     * HL_PLATFORM_PUBKEY_HEX.
     *
     * Three skip paths, in priority order:
     *   1. --no-verify-platform → skip with no message (caller explicit)
     *   2. HL_PLATFORM_PUBKEY_HEX is all-zeros (placeholder build) →
     *      skip with a one-time warning. Same bootstrap behavior as
     *      hull update's HL_RELEASE_PUBKEY_HEX placeholder. v0.1.3 C5
     *      restores the real pubkey; until then this path always fires.
     *   3. gethull block absent in package.sig.platform AND real pubkey
     *      embedded → hard reject. Apps built with --no-verify-platform
     *      or against pre-v0.1.3 hulls hit this; they need
     *      --no-verify-platform at runtime too.
     *
     * The "older" platform.{platforms,public_key,signature} layer
     * (checked above) is independent - it's the developer-signed
     * fork-deployable layer and remains enforceable on its own. */
    if (!no_verify_platform) {
        if (hl_platform_pubkey_is_placeholder()) {
            /* Skip-with-warning (matches v0.1.0 release-pubkey bootstrap).
             * `static int` guard so a long-running process logs once,
             * not once per `--verify-sig` startup. The macro is
             * compile-time-constant so the first answer is the only
             * one. */
            static int warned_placeholder = 0;
            if (!warned_placeholder) {
                log_warn("[sig] HL_PLATFORM_PUBKEY_HEX is the all-zeros "
                         "placeholder - skipping gethull platform-sig check");
                warned_placeholder = 1;
            }
        } else if (!sig.platform.gethull_manifest ||
                   !sig.platform.gethull_signature_hex ||
                   sig.platform.gethull_manifest_len == 0) {
            log_error("[sig] app was built without a gethull platform-sig "
                      "(rebuild against a release-built hull >=0.1.3, or "
                      "pass --no-verify-platform to skip this check)");
            hl_sig_free(&sig);
            return -1;
        } else {
            /* Decode HL_PLATFORM_PUBKEY_HEX once here and pass it
             * explicitly - avoids hl_platform_sig_verify decoding the
             * same macro a second time when its pubkey arg is NULL. */
            uint8_t embedded_pk[32];
            if (hl_hex_decode(HL_PLATFORM_PUBKEY_HEX, 64, embedded_pk, 32) != 32) {
                log_error("[sig] HL_PLATFORM_PUBKEY_HEX is malformed "
                          "(compile-time misconfiguration)");
                hl_sig_free(&sig);
                return -1;
            }
            if (hl_platform_sig_verify(
                    sig.platform.gethull_manifest,
                    sig.platform.gethull_manifest_len,
                    sig.platform.gethull_signature_hex,
                    sig.platform.gethull_signature_hex_len,
                    embedded_pk) != 0) {
                log_error("[sig] gethull platform-sig verification failed");
                hl_sig_free(&sig);
                return -1;
            }

            /* The signed manifest proves only that the MANIFEST is
             * genuine; the app is bound to it by the per-arch hashes the
             * build cross-checked against its platform archive. Without
             * them (a --no-verify-platform or --target build inherits the
             * building hull's manifest + signature verbatim) this check
             * passed for an app linked against any libhull_platform.a. */
            {
                ShJsonValue *ah = sig.platform.gethull_value
                    ? sh_json_get(sig.platform.gethull_value, "arch_hashes") : NULL;
                size_t nah = (ah && sh_json_type(ah) == SH_JSON_OBJECT)
                    ? ah->u.object_val.count : 0;
                if (nah == 0) {
                    log_error("[sig] app does not bind its platform library to "
                              "the signed manifest (built with "
                              "--no-verify-platform?); pass --no-verify-platform "
                              "to skip this check");
                    hl_sig_free(&sig);
                    return -1;
                }
                for (size_t i = 0; i < nah; i++) {
                    const char *arch = ah->u.object_val.members[i].key;
                    const char *hash = sh_json_as_string(
                        ah->u.object_val.members[i].value, NULL);
                    char want[65];
                    if (!arch || !hash || strlen(hash) != 64 ||
                        hl_platform_sig_extract_for_arch(
                            sig.platform.gethull_manifest,
                            sig.platform.gethull_manifest_len, arch, want) != 0 ||
                        memcmp(want, hash, 64) != 0) {   /* public hashes */
                        log_error("[sig] platform hash for '%s' is not the one "
                                  "in the signed manifest", arch ? arch : "?");
                        hl_sig_free(&sig);
                        return -1;
                    }
                }
            }

            /* 5c. Composed-feature attestation (issue #114).
             *
             * Beyond the base platform lib, a native app whole-archives the
             * runtime + (optional) HTTP core + web bindings + tui bridge. Each
             * such archive was recorded under
             * package.sig.gethull.composed.platform_domain as {name, sha256},
             * alongside the SAME platform-key manifest verified in 5b. Prove
             * every recorded hash is present in that signed manifest - so a
             * swapped composed archive can't ride a genuine base attestation.
             *
             * Presence-gated: the block is absent on pre-#114 apps and on cosmo
             * (fat binary, no composed archives), where 5b alone anchors trust.
             * When present, any failure is fatal. The block itself is inside the
             * developer-signed payload (step 6), so it can't be stripped without
             * breaking the app signature. */
            if (sig.platform.gethull_value) {
                ShJsonValue *composed =
                    sh_json_get(sig.platform.gethull_value, "composed");
                ShJsonValue *pd = composed
                    ? sh_json_get(composed, "platform_domain") : NULL;
                if (pd && sh_json_type(pd) == SH_JSON_OBJECT) {
                    const char *pman = sh_json_as_string(
                        sh_json_get(pd, "manifest"), NULL);
                    const char *psig = sh_json_as_string(
                        sh_json_get(pd, "signature"), NULL);
                    ShJsonValue *arr = sh_json_get(pd, "assets");
                    size_t n = arr ? sh_json_array_len(arr) : 0;
                    if (!pman || !psig) {
                        log_error("[sig] composed platform_domain missing "
                                  "manifest/signature");
                        hl_sig_free(&sig);
                        return -1;
                    }
                    HlPlatformArchHash *assets = NULL;
                    if (n > 0) {
                        if (n > SIZE_MAX / sizeof(*assets)) {
                            hl_sig_free(&sig);
                            return -1;
                        }
                        assets = calloc(n, sizeof(*assets));
                        if (!assets) {
                            hl_sig_free(&sig);
                            return -1;
                        }
                        for (size_t i = 0; i < n; i++) {
                            ShJsonValue *e = sh_json_array_get(arr, i);
                            assets[i].arch = sh_json_as_string(
                                sh_json_get(e, "name"), NULL);
                            assets[i].hash_hex = sh_json_as_string(
                                sh_json_get(e, "sha256"), NULL);
                            if (!assets[i].arch || !assets[i].hash_hex) {
                                log_error("[sig] composed asset %zu missing "
                                          "name/sha256", i);
                                free(assets);
                                hl_sig_free(&sig);
                                return -1;
                            }
                        }
                    }
                    int crc = hl_platform_sig_verify_composed(
                        pman, strlen(pman), psig, strlen(psig),
                        embedded_pk, assets, n);
                    free(assets);
                    if (crc != 0) {
                        log_error("[sig] composed-feature attestation "
                                  "verification failed");
                        hl_sig_free(&sig);
                        return -1;
                    }
                }
            }
        }
    }

    /* 6. (The app signature was verified at 4b.) */

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

    /* Both modes: whatever the loaders still read from disk (a dev-mode
     * app's every file; a built binary's fallbacks) must be signed. */
    if (rc == 0 && sig_arm_disk_gate(&sig) != 0) {
        log_error("[sig] cannot arm the signed-file gate");
        rc = -1;
    }

    if (rc != 0) {
        log_error("[sig] file hash verification failed");
        hl_sig_free(&sig);
        return -1;
    }

    sig_capture_policy(&sig);   /* for hl_sig_check_runtime_policy */
    hl_sig_free(&sig);
    return 0;
}
