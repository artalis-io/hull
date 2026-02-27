/*
 * signature.c — App signature verification (runtime)
 *
 * Reads hull.sig (JSON), extracts fields with targeted string scanning,
 * verifies Ed25519 signature, and checks file SHA-256 hashes.
 *
 * No full JSON parser needed — hull.sig has a fixed schema produced by
 * build.lua's json.encode(). We extract raw JSON fragments and string
 * fields by key scanning.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/signature.h"
#include "hull/cap/crypto.h"

#include "log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Embedded app entries (provided by app_entries_default.o or app_registry.o) */
typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int len;
} HlStdlibEntry;
extern const HlStdlibEntry hl_app_lua_entries[];

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

/* ── JSON extraction helpers ──────────────────────────────────────── */

/*
 * Find a JSON key in the form "key": and return a pointer to the
 * character after the colon. Only matches top-level keys (depth 0).
 */
static const char *find_json_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    int depth = 0;
    int in_string = 0;

    while (*p) {
        if (in_string) {
            if (*p == '\\') {
                p++;
                if (*p) p++;
                continue;
            }
            if (*p == '"') in_string = 0;
            p++;
            continue;
        }

        if (*p == '"') {
            /* Check if this is our key at depth 1 (inside the top-level object) */
            if (depth == 1 &&
                strncmp(p + 1, key, klen) == 0 &&
                p[1 + klen] == '"') {
                /* Found "key" — skip to colon */
                const char *after = p + 1 + klen + 1;
                while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')
                    after++;
                if (*after == ':') {
                    after++;
                    while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')
                        after++;
                    return after;
                }
            }
            in_string = 1;
            p++;
            continue;
        }

        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }

    return NULL;
}

/*
 * Extract a JSON object/array starting at `start` (which points to '{' or '[').
 * Returns a strdup'd copy of the balanced substring, or NULL on error.
 */
static char *extract_json_value(const char *start)
{
    if (!start) return NULL;

    char open = *start;
    char close_ch;
    if (open == '{') close_ch = '}';
    else if (open == '[') close_ch = ']';
    else return NULL;

    int depth = 0;
    int in_string = 0;
    const char *p = start;

    while (*p) {
        if (in_string) {
            if (*p == '\\') { p++; if (*p) p++; continue; }
            if (*p == '"') in_string = 0;
            p++;
            continue;
        }

        if (*p == '"') { in_string = 1; p++; continue; }
        if (*p == open) depth++;
        else if (*p == close_ch) {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                char *result = malloc(len + 1);
                if (!result) return NULL;
                memcpy(result, start, len);
                result[len] = '\0';
                return result;
            }
        }
        p++;
    }

    return NULL; /* unbalanced */
}

/*
 * Extract a JSON string value: "value" → strdup'd "value" (without quotes).
 * `start` must point to the opening '"'.
 */
static char *extract_json_string(const char *start)
{
    if (!start || *start != '"') return NULL;

    const char *p = start + 1;
    while (*p && *p != '"') {
        if (*p == '\\') { p++; if (!*p) return NULL; }
        p++;
    }
    if (*p != '"') return NULL;

    size_t len = (size_t)(p - start - 1);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, start + 1, len);
    result[len] = '\0';
    return result;
}

/*
 * Extract a JSON integer value.
 */
static int extract_json_int(const char *start, int *out)
{
    if (!start || !out) return -1;
    char *end;
    long val = strtol(start, &end, 10);
    if (end == start) return -1;
    *out = (int)val;
    return 0;
}

/*
 * Extract the raw JSON value at `start` — could be a string, object,
 * array, number, true, false, or null. Returns a strdup'd copy.
 */
static char *extract_raw_json_value(const char *start)
{
    if (!start) return NULL;

    if (*start == '{' || *start == '[')
        return extract_json_value(start);

    if (*start == '"') {
        /* Find closing quote (accounting for escapes) */
        const char *p = start + 1;
        while (*p && *p != '"') {
            if (*p == '\\') { p++; if (!*p) return NULL; }
            p++;
        }
        if (*p != '"') return NULL;
        size_t len = (size_t)(p - start + 1);
        char *result = malloc(len + 1);
        if (!result) return NULL;
        memcpy(result, start, len);
        result[len] = '\0';
        return result;
    }

    /* Number, true, false, null — scan to delimiter */
    const char *p = start;
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    size_t len = (size_t)(p - start);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

/* ── Parse file entries from files_json ───────────────────────────── */

/*
 * Parse {"filename":"hash",...} into HlSigFileEntry array.
 * files_json must point to a complete JSON object string.
 */
static int parse_file_entries(const char *files_json,
                              HlSigFileEntry **out_entries,
                              size_t *out_count)
{
    if (!files_json || files_json[0] != '{') return -1;

    /* Count entries (number of ':' at depth 1) */
    size_t cap = 16;
    size_t count = 0;
    HlSigFileEntry *entries = calloc(cap, sizeof(HlSigFileEntry));
    if (!entries) return -1;

    const char *p = files_json + 1; /* skip '{' */
    int in_string = 0;

    while (*p && *p != '}') {
        /* Skip whitespace/commas */
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '}') break;

        /* Expect "key" */
        if (*p != '"') { free(entries); return -1; }
        char *key = extract_json_string(p);
        if (!key) { free(entries); return -1; }

        /* Advance past "key" */
        p++; /* opening quote */
        while (*p && !(*p == '"' && *(p - 1) != '\\')) p++;
        if (*p == '"') p++; /* closing quote */

        /* Skip : and whitespace */
        while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r')
            p++;

        /* Expect "value" */
        if (*p != '"') { free(key); free(entries); return -1; }
        char *val = extract_json_string(p);
        if (!val) { free(key); free(entries); return -1; }

        /* Advance past "value" */
        p++;
        while (*p && !(*p == '"' && *(p - 1) != '\\')) p++;
        if (*p == '"') p++;

        /* Store entry */
        if (count >= cap) {
            if (cap > SIZE_MAX / (2 * sizeof(HlSigFileEntry))) {
                free(key); free(val);
                goto fail;
            }
            cap *= 2;
            HlSigFileEntry *ne = realloc(entries, cap * sizeof(HlSigFileEntry));
            if (!ne) { free(key); free(val); goto fail; }
            entries = ne;
        }
        entries[count].name = key;
        entries[count].hash_hex = val;
        count++;

        (void)in_string;
    }

    *out_entries = entries;
    *out_count = count;
    return 0;

fail:
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].hash_hex);
    }
    free(entries);
    return -1;
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

    /* Extract top-level fields */
    const char *val;

    /* version */
    val = find_json_key(data, "version");
    if (!val || extract_json_int(val, &sig->version) != 0) {
        free(data);
        return -1;
    }

    /* files (raw JSON object) */
    val = find_json_key(data, "files");
    if (!val) { free(data); return -1; }
    sig->files_json = extract_raw_json_value(val);
    if (!sig->files_json) { free(data); hl_sig_free(sig); return -1; }

    /* manifest (raw JSON — could be object, null, or absent) */
    val = find_json_key(data, "manifest");
    if (val) {
        sig->manifest_json = extract_raw_json_value(val);
        if (!sig->manifest_json) { free(data); hl_sig_free(sig); return -1; }
    }
    /* If absent, manifest_json stays NULL — meaning Lua's nil (key not encoded) */

    /* signature */
    val = find_json_key(data, "signature");
    if (!val) { free(data); hl_sig_free(sig); return -1; }
    sig->signature_hex = extract_json_string(val);
    if (!sig->signature_hex) { free(data); hl_sig_free(sig); return -1; }

    /* public_key */
    val = find_json_key(data, "public_key");
    if (!val) { free(data); hl_sig_free(sig); return -1; }
    sig->public_key_hex = extract_json_string(val);
    if (!sig->public_key_hex) { free(data); hl_sig_free(sig); return -1; }

    free(data);

    /* Parse file entries from files_json */
    if (parse_file_entries(sig->files_json, &sig->entries, &sig->entry_count) != 0) {
        hl_sig_free(sig);
        return -1;
    }

    return 0;
}

int hl_sig_verify(const HlSignature *sig, const uint8_t pubkey[32])
{
    if (!sig || !pubkey) return -1;
    if (!sig->signature_hex || !sig->files_json)
        return -1;

    /* Decode signature hex → 64 bytes */
    size_t sig_hex_len = strlen(sig->signature_hex);
    if (sig_hex_len != 128) return -1;

    uint8_t sig_bytes[64];
    if (hex_decode(sig->signature_hex, sig_hex_len, sig_bytes, 64) != 0)
        return -1;

    /* Build canonical payload matching Lua's json.encode({files=..., manifest=...})
     * - "files" sorts before "manifest" alphabetically (Lua's table.sort(keys))
     * - If manifest is nil in Lua, the key is absent from the table,
     *   so json.encode produces {"files":...} without manifest
     * - If manifest is present, it's {"files":...,"manifest":...}
     */
    size_t files_len = strlen(sig->files_json);
    char *payload;

    if (sig->manifest_json) {
        /* Manifest present: {"files":...,"manifest":...} */
        size_t manifest_len = strlen(sig->manifest_json);
        size_t payload_len = 9 + files_len + 12 + manifest_len + 1;
        payload = malloc(payload_len + 1);
        if (!payload) return -1;
        snprintf(payload, payload_len + 1,
                 "{\"files\":%s,\"manifest\":%s}",
                 sig->files_json, sig->manifest_json);
    } else {
        /* Manifest absent (nil): {"files":...} */
        size_t payload_len = 9 + files_len + 1;
        payload = malloc(payload_len + 1);
        if (!payload) return -1;
        snprintf(payload, payload_len + 1,
                 "{\"files\":%s}",
                 sig->files_json);
    }

    /* Verify Ed25519 signature */
    int rc = hl_cap_crypto_ed25519_verify(
        (const uint8_t *)payload, strlen(payload), sig_bytes, pubkey);

    free(payload);
    return rc;
}

int hl_sig_verify_files_embedded(const HlSignature *sig)
{
    if (!sig || !sig->entries) return -1;

    /* Build a lookup: for each sig entry, find matching embedded entry */
    for (size_t i = 0; i < sig->entry_count; i++) {
        const char *sig_name = sig->entries[i].name;
        const char *expected_hash = sig->entries[i].hash_hex;

        /* Embedded entries use "./" prefix for relative paths.
         * sig entries use bare relative paths (e.g. "app.lua").
         * Try both: "./" + sig_name and bare sig_name. */
        const unsigned char *data = NULL;
        unsigned int data_len = 0;
        int found = 0;

        for (const HlStdlibEntry *e = hl_app_lua_entries; e->name; e++) {
            /* Embedded uses "./app" for "app.lua", sig uses "app.lua" */
            const char *ename = e->name;
            /* Skip "./" prefix */
            if (ename[0] == '.' && ename[1] == '/')
                ename += 2;

            /* Build expected module name from sig_name: strip .lua suffix */
            size_t slen = strlen(sig_name);
            size_t mlen = slen;
            if (slen > 4 && strcmp(sig_name + slen - 4, ".lua") == 0)
                mlen = slen - 4;

            if (strlen(ename) == mlen && strncmp(ename, sig_name, mlen) == 0) {
                data = e->data;
                data_len = e->len;
                found = 1;
                break;
            }
        }

        if (!found) {
            log_error("[sig] file not found in binary: %s", sig_name);
            return -1;
        }

        /* Compute SHA-256 */
        uint8_t hash[32];
        if (hl_cap_crypto_sha256(data, data_len, hash) != 0) return -1;

        char hash_hex[65];
        hex_encode(hash, 32, hash_hex);

        if (strcmp(hash_hex, expected_hash) != 0) {
            log_error("[sig] hash mismatch for %s", sig_name);
            return -1;
        }
    }

    /* Check for extra files in the binary not in the signature */
    for (const HlStdlibEntry *e = hl_app_lua_entries; e->name; e++) {
        const char *ename = e->name;
        if (ename[0] == '.' && ename[1] == '/')
            ename += 2;

        int found = 0;
        for (size_t i = 0; i < sig->entry_count; i++) {
            const char *sig_name = sig->entries[i].name;
            size_t slen = strlen(sig_name);
            size_t mlen = slen;
            if (slen > 4 && strcmp(sig_name + slen - 4, ".lua") == 0)
                mlen = slen - 4;

            if (strlen(ename) == mlen && strncmp(ename, sig_name, mlen) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            log_error("[sig] extra file in binary not in signature: %s", e->name);
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

        /* Build full path */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", app_dir, name);

        /* Read file */
        FILE *f = fopen(path, "rb");
        if (!f) {
            log_error("[sig] cannot open file: %s", path);
            return -1;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fsize < 0 || fsize > 100 * 1024 * 1024) { /* max 100MB per file */
            fclose(f);
            log_error("[sig] file too large: %s", path);
            return -1;
        }

        char *data = malloc((size_t)fsize);
        if (!data) { fclose(f); return -1; }

        size_t nread = fread(data, 1, (size_t)fsize, f);
        fclose(f);

        /* Compute SHA-256 */
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
    free(sig->files_json);
    free(sig->manifest_json);
    free(sig->signature_hex);
    free(sig->public_key_hex);
    if (sig->entries) {
        for (size_t i = 0; i < sig->entry_count; i++) {
            free(sig->entries[i].name);
            free(sig->entries[i].hash_hex);
        }
        free(sig->entries);
    }
    memset(sig, 0, sizeof(*sig));
}

/* ── Full startup verification ────────────────────────────────────── */

int hl_verify_startup(const char *pubkey_path, const char *entry_point)
{
    if (!pubkey_path || !entry_point) return -1;

    /* 1. Read pubkey file (64 hex chars → 32 bytes) */
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

    /* 2. Derive hull.sig path from entry_point directory */
    char sig_path[PATH_MAX];
    const char *slash = strrchr(entry_point, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - entry_point);
        snprintf(sig_path, sizeof(sig_path), "%.*s/hull.sig",
                 (int)dir_len, entry_point);
    } else {
        snprintf(sig_path, sizeof(sig_path), "hull.sig");
    }

    /* 3. Read and parse hull.sig */
    HlSignature sig;
    if (hl_sig_read(sig_path, &sig) != 0) {
        log_error("[sig] cannot read hull.sig: %s", sig_path);
        return -1;
    }

    /* 4. Verify pubkey matches hull.sig public_key */
    if (sig.public_key_hex) {
        size_t hex_len = strlen(sig.public_key_hex);
        if (hex_len != 64 || strncmp(sig.public_key_hex, pk_hex, 64) != 0) {
            log_error("[sig] pubkey mismatch: --verify-sig key differs from hull.sig");
            hl_sig_free(&sig);
            return -1;
        }
    }

    /* 5. Verify Ed25519 signature */
    if (hl_sig_verify(&sig, pubkey) != 0) {
        log_error("[sig] Ed25519 signature verification failed");
        hl_sig_free(&sig);
        return -1;
    }

    /* 6. Verify file hashes: embedded or filesystem */
    int rc;
    if (hl_app_lua_entries[0].name != NULL) {
        /* Embedded mode */
        rc = hl_sig_verify_files_embedded(&sig);
    } else {
        /* Filesystem mode — derive app_dir from entry_point */
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
