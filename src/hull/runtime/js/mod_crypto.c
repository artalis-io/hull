/*
 * mod_crypto.c — hull:crypto module (SHA, HMAC, PBKDF2, Ed25519, box, etc.)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/crypto.h"
#include "hull/limits/core.h"

#include <stdio.h>

static JSValue js_crypto_sha256(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.sha256 requires (data)");

    /* Accept either ArrayBuffer (binary-safe — preferred for file
     * contents, multipart bodies, etc.) or string (UTF-8 encoded
     * via JS_ToCStringLen). Trying ArrayBuffer first means binary
     * data never round-trips through UTF-8 encoding. */
    size_t len = 0;
    const uint8_t *bytes = JS_GetArrayBuffer(ctx, &len, argv[0]);
    const char *cstr = NULL;
    if (!bytes) {
        cstr = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!cstr) return JS_EXCEPTION;
        bytes = (const uint8_t *)cstr;
    }

    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256((const char *)bytes, len, hash);
    if (cstr) JS_FreeCString(ctx, cstr);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "sha256 failed");

    /* Convert to hex string */
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = '\0';

    return JS_NewString(ctx, hex);
}

static JSValue js_crypto_random(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.random requires (n)");

    int32_t n;
    if (JS_ToInt32(ctx, &n, argv[0]))
        return JS_EXCEPTION;

    if (n <= 0 || n > HL_RANDOM_MAX_BYTES)
        return JS_ThrowRangeError(ctx, "random bytes must be 1-%d",
                                  HL_RANDOM_MAX_BYTES);

    uint8_t *buf = js_malloc(ctx, (size_t)n);
    if (!buf)
        return JS_EXCEPTION;

    if (hl_cap_crypto_random(buf, (size_t)n) != 0) {
        js_free(ctx, buf);
        return JS_ThrowInternalError(ctx, "random failed");
    }

    /* Copy into ArrayBuffer and free temp */
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)n);
    js_free(ctx, buf);
    return ab;
}

/* Hex nibble helper (no sscanf — Cosmopolitan compat) */
static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* crypto.hashPassword(password) -> "pbkdf2:iterations:salt_hex:hash_hex" */
static JSValue js_crypto_hash_password(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.hashPassword requires (password)");

    size_t pw_len;
    const char *pw = JS_ToCStringLen(ctx, &pw_len, argv[0]);
    if (!pw)
        return JS_EXCEPTION;

    /* Generate 16-byte salt */
    uint8_t salt[16];
    if (hl_cap_crypto_random(salt, sizeof(salt)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_ThrowInternalError(ctx, "random failed");
    }

    /* PBKDF2-HMAC-SHA256, 32-byte output */
    uint8_t hash[32];
    int iterations = HL_PBKDF2_ITERATIONS;
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                               iterations, hash, sizeof(hash)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_ThrowInternalError(ctx, "pbkdf2 failed");
    }
    JS_FreeCString(ctx, pw);

    /* Format: "pbkdf2:100000:salt_hex:hash_hex" */
    char salt_hex[33], hash_hex[65];
    for (int i = 0; i < 16; i++)
        snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);

    char result[128];
    snprintf(result, sizeof(result), "pbkdf2:%d:%s:%s",
             iterations, salt_hex, hash_hex);

    secure_zero(hash, sizeof(hash));
    secure_zero(salt, sizeof(salt));

    return JS_NewString(ctx, result);
}

/* crypto.verifyPassword(password, hash_string) -> boolean */
static JSValue js_crypto_verify_password(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.verifyPassword requires (password, hash)");

    size_t pw_len;
    const char *pw = JS_ToCStringLen(ctx, &pw_len, argv[0]);
    const char *stored = JS_ToCString(ctx, argv[1]);
    if (!pw || !stored) {
        if (pw) JS_FreeCString(ctx, pw);
        if (stored) JS_FreeCString(ctx, stored);
        return JS_EXCEPTION;
    }

    /* Parse "pbkdf2:iterations:salt_hex:hash_hex" manually (no scansets
     * — Cosmopolitan libc doesn't support sscanf %[...] scansets). */
    if (strncmp(stored, "pbkdf2:", 7) != 0) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    const char *p = stored + 7;

    char *end = NULL;
    long iterations = strtol(p, &end, 10);
    if (!end || *end != ':' || iterations < 100000) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    p = end + 1;

    if (strlen(p) < 32 + 1 + 64 || p[32] != ':') {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    char salt_hex[33];
    memcpy(salt_hex, p, 32);
    salt_hex[32] = '\0';
    p += 33;

    if (strlen(p) < 64) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    char hash_hex[65];
    memcpy(hash_hex, p, 64);
    hash_hex[64] = '\0';

    JS_FreeCString(ctx, stored);

    /* Decode hex salt (manual — sscanf %x broken on Cosmopolitan) */
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble((unsigned char)salt_hex[i * 2]);
        int lo = hex_nibble((unsigned char)salt_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { JS_FreeCString(ctx, pw); return JS_FALSE; }
        salt[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                               (int)iterations, computed, sizeof(computed)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_FALSE;
    }
    JS_FreeCString(ctx, pw);

    /* Decode stored hash and compare (constant-time) */
    uint8_t stored_hash[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble((unsigned char)hash_hex[i * 2]);
        int lo = hex_nibble((unsigned char)hash_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return JS_FALSE;
        stored_hash[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Constant-time comparison */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed[i] ^ stored_hash[i];

    secure_zero(computed, sizeof(computed));
    secure_zero(stored_hash, sizeof(stored_hash));
    secure_zero(salt, sizeof(salt));

    return diff == 0 ? JS_TRUE : JS_FALSE;
}

/* ── Hex decode helper (no sscanf — Cosmopolitan compat) ──────────── */

static int hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2)
        return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── Ed25519 bindings ──────────────────────────────────────────────── */

/* crypto.ed25519Keypair() -> { publicKey: hex, secretKey: hex } */
static JSValue js_crypto_ed25519_keypair(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0)
        return JS_ThrowInternalError(ctx, "ed25519 keypair generation failed");

    char pk_hex[65], sk_hex[129];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 64; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[128] = '\0';

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "publicKey", JS_NewString(ctx, pk_hex));
    JS_SetPropertyStr(ctx, obj, "secretKey", JS_NewString(ctx, sk_hex));
    secure_zero(sk, sizeof(sk));
    secure_zero(sk_hex, sizeof(sk_hex));
    return obj;
}

/* crypto.ed25519Sign(data, secretKeyHex) -> signatureHex */
static JSValue js_crypto_ed25519_sign(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.ed25519Sign requires (data, secretKeyHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t sk_hex_len;
    const char *sk_hex = JS_ToCStringLen(ctx, &sk_hex_len, argv[1]);
    if (!sk_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    if (sk_hex_len != 128) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sk_hex);
        return JS_ThrowTypeError(ctx, "secret key must be 128 hex chars (64 bytes)");
    }

    uint8_t sk[64];
    if (hex_decode(sk_hex, sk_hex_len, sk, 64) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sk_hex);
        secure_zero(sk, sizeof(sk));
        return JS_ThrowTypeError(ctx, "invalid hex in secret key");
    }
    JS_FreeCString(ctx, sk_hex);

    uint8_t sig[64];
    if (hl_cap_crypto_ed25519_sign((const uint8_t *)data, data_len, sk, sig) != 0) {
        JS_FreeCString(ctx, data);
        secure_zero(sk, sizeof(sk));
        return JS_ThrowInternalError(ctx, "ed25519 sign failed");
    }
    JS_FreeCString(ctx, data);
    secure_zero(sk, sizeof(sk));

    char sig_hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(sig_hex + i * 2, 3, "%02x", sig[i]);
    sig_hex[128] = '\0';

    return JS_NewString(ctx, sig_hex);
}

/* crypto.ed25519Verify(data, sigHex, pubkeyHex) -> boolean */
static JSValue js_crypto_ed25519_verify(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.ed25519Verify requires (data, sigHex, pubkeyHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t sig_hex_len;
    const char *sig_hex = JS_ToCStringLen(ctx, &sig_hex_len, argv[1]);
    if (!sig_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    size_t pk_hex_len;
    const char *pk_hex = JS_ToCStringLen(ctx, &pk_hex_len, argv[2]);
    if (!pk_hex) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sig_hex);
        return JS_EXCEPTION;
    }

    if (sig_hex_len != 128 || pk_hex_len != 64) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sig_hex);
        JS_FreeCString(ctx, pk_hex);
        return JS_FALSE;
    }

    uint8_t sig[64], pk[32];
    if (hex_decode(sig_hex, sig_hex_len, sig, 64) != 0 ||
        hex_decode(pk_hex, pk_hex_len, pk, 32) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sig_hex);
        JS_FreeCString(ctx, pk_hex);
        return JS_FALSE;
    }

    JS_FreeCString(ctx, sig_hex);
    JS_FreeCString(ctx, pk_hex);

    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)data, data_len, sig, pk);
    JS_FreeCString(ctx, data);

    return rc == 0 ? JS_TRUE : JS_FALSE;
}

/* crypto.verify(alg, pubkeyPem, data, sig) -> boolean
 *
 *  alg        - "RS256" / "RS384" / "RS512" / "PS256" / "ES256" / "ES384"
 *  pubkeyPem  - PEM-encoded SubjectPublicKeyInfo (string).
 *  data       - message bytes (string).
 *  sig        - raw signature bytes (string). ECDSA must be JOSE
 *               r||s, NOT DER.
 *
 *  Returns true on a valid signature, false otherwise. Throws on
 *  programming errors (unknown alg) so callers can't silently get
 *  false on misuse. Mirrors Lua's crypto.verify exactly.
 */

static JSValue js_crypto_verify(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx,
            "crypto.verify requires (alg, pubkeyPem, data, sig)");

    /* alg + pubkey are always ASCII (alg short, PEM base64 body) so
     * the string path is fine. data + sig are arbitrary bytes and
     * must go through js_get_buffer to stay binary-safe: a JS string
     * with high bytes round-trips through JS_ToCStringLen as UTF-8
     * and ends up longer than its source, which corrupts RSA / ECDSA
     * signatures. ArrayBuffer / WasmBuffer / MappedBuffer all reach
     * verify() with the original bytes intact. */
    size_t alg_len = 0, pk_len = 0;
    const char *alg_str = JS_ToCStringLen(ctx, &alg_len, argv[0]);
    if (!alg_str) return JS_EXCEPTION;
    const char *pk = JS_ToCStringLen(ctx, &pk_len, argv[1]);
    if (!pk) { JS_FreeCString(ctx, alg_str); return JS_EXCEPTION; }

    HlBufferView data_view = {0}, sig_view = {0};
    const char *data_str = NULL, *sig_str = NULL;
    int data_needs_free = 0, sig_needs_free = 0;

    if (!js_get_buffer(ctx, argv[2], &data_view, &data_str, &data_needs_free)) {
        JS_FreeCString(ctx, alg_str);
        JS_FreeCString(ctx, pk);
        return JS_ThrowTypeError(ctx,
            "crypto.verify: data must be ArrayBuffer or string");
    }
    if (!js_get_buffer(ctx, argv[3], &sig_view, &sig_str, &sig_needs_free)) {
        JS_FreeCString(ctx, alg_str);
        JS_FreeCString(ctx, pk);
        if (data_needs_free) JS_FreeCString(ctx, data_str);
        return JS_ThrowTypeError(ctx,
            "crypto.verify: sig must be ArrayBuffer or string");
    }

    HlCryptoAsymAlg alg = hl_crypto_asym_alg_from_string(alg_str, alg_len);
    JSValue out;
    if (alg == HL_CRYPTO_ASYM_NONE) {
        out = JS_ThrowTypeError(ctx,
            "crypto.verify: unsupported alg '%.*s' (use one of "
            "RS256/RS384/RS512/PS256/ES256/ES384; HS256 is "
            "crypto.hmacSha256Verify; 'none' is rejected)",
            (int)alg_len, alg_str);
    } else {
        int rc = hl_cap_crypto_asym_verify_default(pk, pk_len, alg,
                                             data_view.data, data_view.len,
                                             sig_view.data,  sig_view.len);
        /* Collapse all failure modes to `false`. The cap layer's
         * -1 vs -2 distinction is for audit-mode logging, not for
         * callers (avoids leaking an oracle). */
        out = rc == 0 ? JS_TRUE : JS_FALSE;
    }
    JS_FreeCString(ctx, alg_str);
    JS_FreeCString(ctx, pk);
    if (data_needs_free) JS_FreeCString(ctx, data_str);
    if (sig_needs_free)  JS_FreeCString(ctx, sig_str);
    return out;
}

/* ── SHA-512 ──────────────────────────────────────────────────────── */

static JSValue js_crypto_sha512(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.sha512 requires (data)");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data) return JS_EXCEPTION;

    uint8_t hash[64];
    if (hl_cap_crypto_sha512(data, len, hash) != 0) {
        JS_FreeCString(ctx, data);
        return JS_ThrowInternalError(ctx, "sha512 failed");
    }
    JS_FreeCString(ctx, data);

    char hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[128] = '\0';

    return JS_NewString(ctx, hex);
}

/* ── HMAC-SHA512/256 auth ─────────────────────────────────────────── */

/* crypto.auth(msg, keyHex) -> hex */
static JSValue js_crypto_auth(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.auth requires (msg, keyHex)");

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[0]);
    if (!msg) return JS_EXCEPTION;

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[1]);
    if (!key_hex) { JS_FreeCString(ctx, msg); return JS_EXCEPTION; }

    if (key_hex_len != 64) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "key must be 64 hex chars (32 bytes)");
    }

    uint8_t key[32];
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "invalid hex in key");
    }
    JS_FreeCString(ctx, key_hex);

    uint8_t tag[32];
    if (hl_cap_crypto_auth(msg, msg_len, key, tag) != 0) {
        JS_FreeCString(ctx, msg);
        secure_zero(key, sizeof(key));
        return JS_ThrowInternalError(ctx, "crypto.auth failed");
    }
    JS_FreeCString(ctx, msg);
    secure_zero(key, sizeof(key));

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", tag[i]);
    hex[64] = '\0';

    return JS_NewString(ctx, hex);
}

/* crypto.authVerify(tagHex, msg, keyHex) -> boolean */
static JSValue js_crypto_auth_verify(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.authVerify requires (tagHex, msg, keyHex)");

    size_t tag_hex_len;
    const char *tag_hex = JS_ToCStringLen(ctx, &tag_hex_len, argv[0]);
    if (!tag_hex) return JS_EXCEPTION;

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[1]);
    if (!msg) { JS_FreeCString(ctx, tag_hex); return JS_EXCEPTION; }

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[2]);
    if (!key_hex) {
        JS_FreeCString(ctx, tag_hex);
        JS_FreeCString(ctx, msg);
        return JS_EXCEPTION;
    }

    if (tag_hex_len != 64 || key_hex_len != 64) {
        JS_FreeCString(ctx, tag_hex);
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_FALSE;
    }

    uint8_t tag[32], key[32];
    if (hex_decode(tag_hex, tag_hex_len, tag, 32) != 0 ||
        hex_decode(key_hex, key_hex_len, key, 32) != 0) {
        JS_FreeCString(ctx, tag_hex);
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_FALSE;
    }
    JS_FreeCString(ctx, tag_hex);
    JS_FreeCString(ctx, key_hex);

    int rc = hl_cap_crypto_auth_verify(tag, msg, msg_len, key);
    JS_FreeCString(ctx, msg);
    secure_zero(key, sizeof(key));

    return rc == 0 ? JS_TRUE : JS_FALSE;
}

/* ── Secretbox ────────────────────────────────────────────────────── */

/* crypto.secretbox(msg, nonceHex, keyHex) -> hex */
static JSValue js_crypto_secretbox(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.secretbox requires (msg, nonceHex, keyHex)");

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[0]);
    if (!msg) return JS_EXCEPTION;

    size_t nonce_hex_len, key_hex_len;
    const char *nonce_hex = JS_ToCStringLen(ctx, &nonce_hex_len, argv[1]);
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[2]);
    if (!nonce_hex || !key_hex) {
        JS_FreeCString(ctx, msg);
        if (nonce_hex) JS_FreeCString(ctx, nonce_hex);
        if (key_hex) JS_FreeCString(ctx, key_hex);
        return JS_EXCEPTION;
    }

    if (nonce_hex_len != 48 || key_hex_len != 64) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "nonce must be 48 hex (24 bytes), key 64 hex (32 bytes)");
    }

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, 48, nonce, 24) != 0 ||
        hex_decode(key_hex, 64, key, 32) != 0) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "invalid hex");
    }
    JS_FreeCString(ctx, nonce_hex);
    JS_FreeCString(ctx, key_hex);

    if (msg_len > SIZE_MAX - HL_SECRETBOX_MACBYTES) {
        JS_FreeCString(ctx, msg);
        secure_zero(key, sizeof(key));
        return JS_ThrowRangeError(ctx, "message too large");
    }
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;
    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, msg); secure_zero(key, sizeof(key)); return JS_EXCEPTION; }

    if (hl_cap_crypto_secretbox(ct, msg, msg_len, nonce, key) != 0) {
        JS_FreeCString(ctx, msg);
        js_free(ctx, ct);
        secure_zero(key, sizeof(key));
        return JS_ThrowInternalError(ctx, "secretbox failed");
    }
    JS_FreeCString(ctx, msg);
    secure_zero(key, sizeof(key));

    /* Hex encode */
    if (ct_len > SIZE_MAX / 2) { js_free(ctx, ct); return JS_ThrowRangeError(ctx, "ciphertext too large"); }
    char *hex = js_malloc(ctx, ct_len * 2 + 1);
    if (!hex) { js_free(ctx, ct); return JS_EXCEPTION; }
    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);
    hex[ct_len * 2] = '\0';
    js_free(ctx, ct);

    JSValue result = JS_NewString(ctx, hex);
    js_free(ctx, hex);
    return result;
}

/* crypto.secretboxOpen(ctHex, nonceHex, keyHex) -> string/null */
static JSValue js_crypto_secretbox_open(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.secretboxOpen requires (ctHex, nonceHex, keyHex)");

    size_t ct_hex_len;
    const char *ct_hex = JS_ToCStringLen(ctx, &ct_hex_len, argv[0]);
    if (!ct_hex) return JS_EXCEPTION;

    size_t nonce_hex_len, key_hex_len;
    const char *nonce_hex = JS_ToCStringLen(ctx, &nonce_hex_len, argv[1]);
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[2]);
    if (!nonce_hex || !key_hex) {
        JS_FreeCString(ctx, ct_hex);
        if (nonce_hex) JS_FreeCString(ctx, nonce_hex);
        if (key_hex) JS_FreeCString(ctx, key_hex);
        return JS_EXCEPTION;
    }

    if (ct_hex_len % 2 != 0 || nonce_hex_len != 48 || key_hex_len != 64) {
        JS_FreeCString(ctx, ct_hex);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_NULL;
    }

    size_t ct_len = ct_hex_len / 2;
    if (ct_len < HL_SECRETBOX_MACBYTES) {
        JS_FreeCString(ctx, ct_hex);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_NULL;
    }

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, 48, nonce, 24) != 0 ||
        hex_decode(key_hex, 64, key, 32) != 0) {
        JS_FreeCString(ctx, ct_hex);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_NULL;
    }
    JS_FreeCString(ctx, nonce_hex);
    JS_FreeCString(ctx, key_hex);

    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, ct_hex); secure_zero(key, sizeof(key)); return JS_EXCEPTION; }
    if (hex_decode(ct_hex, ct_hex_len, ct, ct_len) != 0) {
        JS_FreeCString(ctx, ct_hex);
        js_free(ctx, ct);
        secure_zero(key, sizeof(key));
        return JS_NULL;
    }
    JS_FreeCString(ctx, ct_hex);

    size_t pt_len = ct_len - HL_SECRETBOX_MACBYTES;
    uint8_t *pt = js_malloc(ctx, pt_len + 1);
    if (!pt) { js_free(ctx, ct); secure_zero(key, sizeof(key)); return JS_EXCEPTION; }

    if (hl_cap_crypto_secretbox_open(pt, ct, ct_len, nonce, key) != 0) {
        js_free(ctx, ct);
        js_free(ctx, pt);
        secure_zero(key, sizeof(key));
        return JS_NULL;
    }
    js_free(ctx, ct);
    secure_zero(key, sizeof(key));

    JSValue result = JS_NewStringLen(ctx, (const char *)pt, pt_len);
    js_free(ctx, pt);
    return result;
}

/* ── Box (public-key encryption) ──────────────────────────────────── */

/* crypto.box(msg, nonceHex, pkHex, skHex) -> hex */
static JSValue js_crypto_box(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "crypto.box requires (msg, nonceHex, pkHex, skHex)");

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[0]);
    if (!msg) return JS_EXCEPTION;

    size_t nh_len, pkh_len, skh_len;
    const char *nh = JS_ToCStringLen(ctx, &nh_len, argv[1]);
    const char *pkh = JS_ToCStringLen(ctx, &pkh_len, argv[2]);
    const char *skh = JS_ToCStringLen(ctx, &skh_len, argv[3]);
    if (!nh || !pkh || !skh) {
        JS_FreeCString(ctx, msg);
        if (nh) JS_FreeCString(ctx, nh);
        if (pkh) JS_FreeCString(ctx, pkh);
        if (skh) JS_FreeCString(ctx, skh);
        return JS_EXCEPTION;
    }

    if (nh_len != 48 || pkh_len != 64 || skh_len != 64) {
        JS_FreeCString(ctx, msg); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_ThrowTypeError(ctx, "nonce 48 hex, pk/sk 64 hex each");
    }

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nh, 48, nonce, 24) != 0 ||
        hex_decode(pkh, 64, pk, 32) != 0 ||
        hex_decode(skh, 64, sk, 32) != 0) {
        JS_FreeCString(ctx, msg); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_ThrowTypeError(ctx, "invalid hex");
    }
    JS_FreeCString(ctx, nh);
    JS_FreeCString(ctx, pkh);
    JS_FreeCString(ctx, skh);

    if (msg_len > SIZE_MAX - HL_BOX_MACBYTES) {
        JS_FreeCString(ctx, msg);
        secure_zero(sk, sizeof(sk));
        return JS_ThrowRangeError(ctx, "message too large");
    }
    size_t ct_len = msg_len + HL_BOX_MACBYTES;
    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, msg); secure_zero(sk, sizeof(sk)); return JS_EXCEPTION; }

    if (hl_cap_crypto_box(ct, msg, msg_len, nonce, pk, sk) != 0) {
        JS_FreeCString(ctx, msg);
        js_free(ctx, ct);
        secure_zero(sk, sizeof(sk));
        return JS_ThrowInternalError(ctx, "box failed");
    }
    JS_FreeCString(ctx, msg);
    secure_zero(sk, sizeof(sk));

    if (ct_len > SIZE_MAX / 2) { js_free(ctx, ct); return JS_ThrowRangeError(ctx, "ciphertext too large"); }
    char *hex = js_malloc(ctx, ct_len * 2 + 1);
    if (!hex) { js_free(ctx, ct); return JS_EXCEPTION; }
    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);
    hex[ct_len * 2] = '\0';
    js_free(ctx, ct);

    JSValue result = JS_NewString(ctx, hex);
    js_free(ctx, hex);
    return result;
}

/* crypto.boxOpen(ctHex, nonceHex, pkHex, skHex) -> string/null */
static JSValue js_crypto_box_open(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "crypto.boxOpen requires (ctHex, nonceHex, pkHex, skHex)");

    size_t cth_len, nh_len, pkh_len, skh_len;
    const char *cth = JS_ToCStringLen(ctx, &cth_len, argv[0]);
    const char *nh = JS_ToCStringLen(ctx, &nh_len, argv[1]);
    const char *pkh = JS_ToCStringLen(ctx, &pkh_len, argv[2]);
    const char *skh = JS_ToCStringLen(ctx, &skh_len, argv[3]);
    if (!cth || !nh || !pkh || !skh) {
        if (cth) JS_FreeCString(ctx, cth);
        if (nh) JS_FreeCString(ctx, nh);
        if (pkh) JS_FreeCString(ctx, pkh);
        if (skh) JS_FreeCString(ctx, skh);
        return JS_EXCEPTION;
    }

    if (cth_len % 2 != 0 || nh_len != 48 || pkh_len != 64 || skh_len != 64) {
        JS_FreeCString(ctx, cth); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_NULL;
    }

    size_t ct_len = cth_len / 2;
    if (ct_len < HL_BOX_MACBYTES) {
        JS_FreeCString(ctx, cth); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_NULL;
    }

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nh, 48, nonce, 24) != 0 ||
        hex_decode(pkh, 64, pk, 32) != 0 ||
        hex_decode(skh, 64, sk, 32) != 0) {
        JS_FreeCString(ctx, cth); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_NULL;
    }
    JS_FreeCString(ctx, nh);
    JS_FreeCString(ctx, pkh);
    JS_FreeCString(ctx, skh);

    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, cth); secure_zero(sk, sizeof(sk)); return JS_EXCEPTION; }
    if (hex_decode(cth, cth_len, ct, ct_len) != 0) {
        JS_FreeCString(ctx, cth);
        js_free(ctx, ct);
        secure_zero(sk, sizeof(sk));
        return JS_NULL;
    }
    JS_FreeCString(ctx, cth);

    size_t pt_len = ct_len - HL_BOX_MACBYTES;
    uint8_t *pt = js_malloc(ctx, pt_len + 1);
    if (!pt) { js_free(ctx, ct); secure_zero(sk, sizeof(sk)); return JS_EXCEPTION; }

    if (hl_cap_crypto_box_open(pt, ct, ct_len, nonce, pk, sk) != 0) {
        js_free(ctx, ct);
        js_free(ctx, pt);
        secure_zero(sk, sizeof(sk));
        return JS_NULL;
    }
    js_free(ctx, ct);
    secure_zero(sk, sizeof(sk));

    JSValue result = JS_NewStringLen(ctx, (const char *)pt, pt_len);
    js_free(ctx, pt);
    return result;
}

/* crypto.boxKeypair() -> { publicKey: hex, secretKey: hex } */
static JSValue js_crypto_box_keypair(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    uint8_t pk[32], sk[32];
    if (hl_cap_crypto_box_keypair(pk, sk) != 0)
        return JS_ThrowInternalError(ctx, "box keypair generation failed");

    char pk_hex[65], sk_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 32; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[64] = '\0';

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "publicKey", JS_NewString(ctx, pk_hex));
    JS_SetPropertyStr(ctx, obj, "secretKey", JS_NewString(ctx, sk_hex));
    secure_zero(sk, sizeof(sk));
    secure_zero(sk_hex, sizeof(sk_hex));
    return obj;
}

/* crypto.hmacSha256(data, keyHex) -> hex string */
static JSValue js_crypto_hmac_sha256(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256 requires (data, keyHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[1]);
    if (!key_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    if (key_hex_len % 2 != 0 || key_hex_len == 0 || key_hex_len > 256) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "key must be 1-128 bytes (2-256 hex chars)");
    }

    size_t key_len = key_hex_len / 2;
    uint8_t key[128];
    if (hex_decode(key_hex, key_hex_len, key, key_len) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "invalid hex in key");
    }
    JS_FreeCString(ctx, key_hex);

    uint8_t out[32];
    if (hl_cap_crypto_hmac_sha256(key, key_len,
                                  (const uint8_t *)data, data_len, out) != 0) {
        JS_FreeCString(ctx, data);
        secure_zero(key, sizeof(key));
        return JS_ThrowInternalError(ctx, "hmacSha256 failed");
    }
    JS_FreeCString(ctx, data);
    secure_zero(key, sizeof(key));

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    hex[64] = '\0';

    return JS_NewString(ctx, hex);
}

/* crypto.hmacSha256Verify(data, keyHex, expectedHex) -> boolean */
static JSValue js_crypto_hmac_sha256_verify(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256Verify requires (data, keyHex, expectedHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[1]);
    if (!key_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    size_t expected_hex_len;
    const char *expected_hex = JS_ToCStringLen(ctx, &expected_hex_len, argv[2]);
    if (!expected_hex) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        return JS_EXCEPTION;
    }

    if (key_hex_len % 2 != 0 || key_hex_len == 0 || key_hex_len > 256) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        JS_FreeCString(ctx, expected_hex);
        return JS_ThrowTypeError(ctx, "key must be 1-128 bytes (2-256 hex chars)");
    }
    if (expected_hex_len != 64) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        JS_FreeCString(ctx, expected_hex);
        return JS_ThrowTypeError(ctx, "expected mac must be 64 hex chars (32 bytes)");
    }

    size_t key_len = key_hex_len / 2;
    uint8_t key[128];
    if (hex_decode(key_hex, key_hex_len, key, key_len) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        JS_FreeCString(ctx, expected_hex);
        return JS_ThrowTypeError(ctx, "invalid hex in key");
    }
    JS_FreeCString(ctx, key_hex);

    uint8_t expected[32];
    if (hex_decode(expected_hex, expected_hex_len, expected, 32) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, expected_hex);
        secure_zero(key, sizeof(key));
        return JS_ThrowTypeError(ctx, "invalid hex in expected mac");
    }
    JS_FreeCString(ctx, expected_hex);

    int rc = hl_cap_crypto_hmac_sha256_verify(key, key_len,
                                               (const uint8_t *)data, data_len,
                                               expected);
    JS_FreeCString(ctx, data);
    secure_zero(key, sizeof(key));

    return JS_NewBool(ctx, rc == 0);
}

/* crypto.base64urlEncode(data) -> string (no padding) */
static JSValue js_crypto_base64url_encode(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.base64urlEncode requires (data)");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data) return JS_EXCEPTION;

    if (len > SIZE_MAX / 4) {
        JS_FreeCString(ctx, data);
        return JS_ThrowRangeError(ctx, "input too large for base64url");
    }
    size_t out_size = ((len * 4) + 2) / 3 + 1;
    char *out = js_malloc(ctx, out_size);
    if (!out) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    size_t out_len;
    if (hl_cap_crypto_base64url_encode(data, len, out, out_size, &out_len) != 0) {
        JS_FreeCString(ctx, data);
        js_free(ctx, out);
        return JS_ThrowInternalError(ctx, "base64urlEncode failed");
    }
    JS_FreeCString(ctx, data);

    JSValue result = JS_NewStringLen(ctx, out, out_len);
    js_free(ctx, out);
    return result;
}

/* crypto.base64urlDecode(str) -> string or null on error */
static JSValue js_crypto_base64url_decode(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.base64urlDecode requires (str)");

    size_t str_len;
    const char *str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str) return JS_EXCEPTION;

    size_t out_size = (str_len * 3) / 4 + 1;
    uint8_t *out = js_malloc(ctx, out_size);
    if (!out) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }

    size_t out_len;
    if (hl_cap_crypto_base64url_decode(str, str_len, out, out_size, &out_len) != 0) {
        JS_FreeCString(ctx, str);
        js_free(ctx, out);
        return JS_NULL;
    }
    JS_FreeCString(ctx, str);

    JSValue result = JS_NewStringLen(ctx, (const char *)out, out_len);
    js_free(ctx, out);
    return result;
}

/* ── Incremental SHA-256 hasher class ───────────────────────────────
 *
 *   const h = crypto.createSha256();
 *   h.update(chunk);     // accepts ArrayBuffer or string; chainable
 *   const hex = h.digest();   // 64-char hex; further calls throw
 */

static JSClassID hl_js_sha256_hasher_class_id;

typedef struct {
    HlSha256Ctx ctx;
    int         done;
} HlJsSha256Hasher;

static void js_sha256_hasher_finalizer(JSRuntime *rt, JSValue val)
{
    HlJsSha256Hasher *h = JS_GetOpaque(val, hl_js_sha256_hasher_class_id);
    if (!h) return;
    /* Scrub any in-flight state from abandoned hashers — _final does the
     * same on the normal path. */
    if (!h->done) memset(&h->ctx, 0, sizeof(h->ctx));
    js_free_rt(rt, h);
}

static JSClassDef js_sha256_hasher_class = {
    "Sha256Hasher",
    .finalizer = js_sha256_hasher_finalizer,
};

static JSValue js_sha256_hasher_update(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    HlJsSha256Hasher *h = JS_GetOpaque2(ctx, this_val, hl_js_sha256_hasher_class_id);
    if (!h) return JS_EXCEPTION;
    if (h->done)
        return JS_ThrowInternalError(ctx, "sha256.update() after digest()");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sha256.update requires (data)");

    /* Accept ArrayBuffer (binary-safe) or string (UTF-8 via JS_ToCStringLen). */
    size_t len = 0;
    const uint8_t *bytes = JS_GetArrayBuffer(ctx, &len, argv[0]);
    const char *cstr = NULL;
    if (!bytes) {
        cstr = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!cstr) return JS_EXCEPTION;
        bytes = (const uint8_t *)cstr;
    }
    int rc = hl_cap_crypto_sha256_update(&h->ctx, bytes, len);
    if (cstr) JS_FreeCString(ctx, cstr);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "sha256.update() failed");
    return JS_DupValue(ctx, this_val);  /* chainable */
}

static JSValue js_sha256_hasher_digest(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsSha256Hasher *h = JS_GetOpaque2(ctx, this_val, hl_js_sha256_hasher_class_id);
    if (!h) return JS_EXCEPTION;
    if (h->done)
        return JS_ThrowInternalError(ctx, "sha256.digest() already called");
    uint8_t out[32];
    if (hl_cap_crypto_sha256_final(&h->ctx, out) != 0)
        return JS_ThrowInternalError(ctx, "sha256.digest() failed");
    h->done = 1;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", out[i]);
    hex[64] = '\0';
    return JS_NewStringLen(ctx, hex, 64);
}

static JSValue js_crypto_create_sha256(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    /* js_malloc routes through QuickJS's allocator, so the runtime's
     * configured memory cap covers Hasher instances — raw malloc would
     * silently bypass JS_SetMemoryLimit. js_free_rt in the finalizer
     * matches the same allocator. */
    HlJsSha256Hasher *h = js_malloc(ctx, sizeof(*h));
    if (!h) return JS_ThrowOutOfMemory(ctx);
    hl_cap_crypto_sha256_init(&h->ctx);
    h->done = 0;

    JSValue obj = JS_NewObjectClass(ctx, (int)hl_js_sha256_hasher_class_id);
    if (JS_IsException(obj)) { js_free(ctx, h); return obj; }
    JS_SetOpaque(obj, h);
    return obj;
}

static void js_register_sha256_hasher_class(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(&hl_js_sha256_hasher_class_id);
    JS_NewClass(rt, hl_js_sha256_hasher_class_id, &js_sha256_hasher_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "update",
        JS_NewCFunction(ctx, js_sha256_hasher_update, "update", 1));
    JS_SetPropertyStr(ctx, proto, "digest",
        JS_NewCFunction(ctx, js_sha256_hasher_digest, "digest", 0));
    JS_SetClassProto(ctx, hl_js_sha256_hasher_class_id, proto);
}

static int js_crypto_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/crypto", "hull:crypto") != 0)
        return -1;

    js_register_sha256_hasher_class(ctx);

    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "sha256",
                      JS_NewCFunction(ctx, js_crypto_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto, "createSha256",
                      JS_NewCFunction(ctx, js_crypto_create_sha256, "createSha256", 0));
    JS_SetPropertyStr(ctx, crypto, "sha512",
                      JS_NewCFunction(ctx, js_crypto_sha512, "sha512", 1));
    JS_SetPropertyStr(ctx, crypto, "random",
                      JS_NewCFunction(ctx, js_crypto_random, "random", 1));
    JS_SetPropertyStr(ctx, crypto, "hashPassword",
                      JS_NewCFunction(ctx, js_crypto_hash_password, "hashPassword", 1));
    JS_SetPropertyStr(ctx, crypto, "verifyPassword",
                      JS_NewCFunction(ctx, js_crypto_verify_password, "verifyPassword", 2));
    JS_SetPropertyStr(ctx, crypto, "ed25519Keypair",
                      JS_NewCFunction(ctx, js_crypto_ed25519_keypair, "ed25519Keypair", 0));
    JS_SetPropertyStr(ctx, crypto, "ed25519Sign",
                      JS_NewCFunction(ctx, js_crypto_ed25519_sign, "ed25519Sign", 2));
    JS_SetPropertyStr(ctx, crypto, "ed25519Verify",
                      JS_NewCFunction(ctx, js_crypto_ed25519_verify, "ed25519Verify", 3));
    JS_SetPropertyStr(ctx, crypto, "verify",
                      JS_NewCFunction(ctx, js_crypto_verify, "verify", 4));
    JS_SetPropertyStr(ctx, crypto, "auth",
                      JS_NewCFunction(ctx, js_crypto_auth, "auth", 2));
    JS_SetPropertyStr(ctx, crypto, "authVerify",
                      JS_NewCFunction(ctx, js_crypto_auth_verify, "authVerify", 3));
    JS_SetPropertyStr(ctx, crypto, "secretbox",
                      JS_NewCFunction(ctx, js_crypto_secretbox, "secretbox", 3));
    JS_SetPropertyStr(ctx, crypto, "secretboxOpen",
                      JS_NewCFunction(ctx, js_crypto_secretbox_open, "secretboxOpen", 3));
    JS_SetPropertyStr(ctx, crypto, "box",
                      JS_NewCFunction(ctx, js_crypto_box, "box", 4));
    JS_SetPropertyStr(ctx, crypto, "boxOpen",
                      JS_NewCFunction(ctx, js_crypto_box_open, "boxOpen", 4));
    JS_SetPropertyStr(ctx, crypto, "boxKeypair",
                      JS_NewCFunction(ctx, js_crypto_box_keypair, "boxKeypair", 0));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256",
                      JS_NewCFunction(ctx, js_crypto_hmac_sha256, "hmacSha256", 2));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256Verify",
                      JS_NewCFunction(ctx, js_crypto_hmac_sha256_verify, "hmacSha256Verify", 3));
    JS_SetPropertyStr(ctx, crypto, "base64urlEncode",
                      JS_NewCFunction(ctx, js_crypto_base64url_encode, "base64urlEncode", 1));
    JS_SetPropertyStr(ctx, crypto, "base64urlDecode",
                      JS_NewCFunction(ctx, js_crypto_base64url_decode, "base64urlDecode", 1));
    JS_SetModuleExport(ctx, m, "crypto", crypto);
    return 0;
}

int hl_js_init_crypto_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:crypto", js_crypto_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "crypto");
    return 0;
}
