/*
 * mod_crypto.c - hull:crypto module (SHA, HMAC, PBKDF2, Ed25519, box, etc.)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/crypto.h"
#include "hull/cap/crypto_key.h"
#include "hull/cap/env.h"
#include "hull/limits/core.h"
#include "../../utils/base64.h"
#include "../../utils/hex.h"

#include <stdio.h>

/* A message argument: the bytes of any buffer (ArrayBuffer, typed array,
 * MappedBuffer, WasmBuffer), or a string's UTF-8 bytes - a string is text.
 * JS_ToCStringLen alone would turn an ArrayBuffer into the text
 * "[object ArrayBuffer]", and give a byte string's high bytes a different
 * encoding from the same bytes in Lua; bytes go in as a buffer
 * (hull:encoding's bytes.toU8). Released with js_msg_free on every path. */
typedef struct {
    HlBufferView view;
    const char  *str;
    int          needs_free;
} JsMsg;

static int js_msg_get(JSContext *ctx, JSValueConst val, JsMsg *m)
{
    memset(m, 0, sizeof(*m));
    if (!js_get_buffer(ctx, val, &m->view, &m->str, &m->needs_free))
        return 0;
    if (!m->view.data) m->view.data = "";       /* an empty buffer */
    return 1;
}

static void js_msg_free(JSContext *ctx, JsMsg *m)
{
    if (m->needs_free) JS_FreeCString(ctx, m->str);
    m->needs_free = 0;
}

/* An argument that must be exactly `want` bytes (a key, nonce, signature or
 * tag). Returns 1, or 0 with a pending exception and nothing to free. */
static int js_fixed_arg(JSContext *ctx, JSValueConst v, JsMsg *m, size_t want,
                        const char *fn, const char *what)
{
    if (!js_msg_get(ctx, v, m)) {
        JS_ThrowTypeError(ctx, "crypto.%s: %s must be a buffer", fn, what);
        return 0;
    }
    if (m->view.len != want) {
        js_msg_free(ctx, m);
        JS_ThrowRangeError(ctx, "crypto.%s: %s must be %zu bytes", fn, what, want);
        return 0;
    }
    return 1;
}

/* { publicKey, secretKey } as ArrayBuffers; the stack copy of the secret key
 * is scrubbed whichever way it goes. */
static JSValue js_keypair_object(JSContext *ctx, const uint8_t *pk, size_t pklen,
                                 uint8_t *sk, size_t sklen)
{
    JSValue obj = JS_NewObject(ctx);
    if (!JS_IsException(obj)) {
        JS_SetPropertyStr(ctx, obj, "publicKey", JS_NewArrayBufferCopy(ctx, pk, pklen));
        JS_SetPropertyStr(ctx, obj, "secretKey", JS_NewArrayBufferCopy(ctx, sk, sklen));
    }
    secure_zero(sk, sklen);
    return obj;
}

typedef int (*JsHmacFn)(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len, uint8_t *out);

/* An HMAC of argv[0] under key argv[1], as an ArrayBuffer of `outlen` bytes. */
static JSValue js_hmac(JSContext *ctx, int argc, JSValueConst *argv,
                       const char *fn, uint8_t *out, size_t outlen, JsHmacFn mac)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.%s requires (data, key)", fn);
    JsMsg data, key;
    if (!js_msg_get(ctx, argv[0], &data))
        return JS_ThrowTypeError(ctx, "crypto.%s: data must be a buffer or a string", fn);
    if (!js_msg_get(ctx, argv[1], &key) || key.view.len == 0) {
        js_msg_free(ctx, &data);
        js_msg_free(ctx, &key);
        return JS_ThrowTypeError(ctx, "crypto.%s: key must be a non-empty buffer or string", fn);
    }
    int rc = mac(key.view.data, key.view.len, data.view.data, data.view.len, out);
    js_msg_free(ctx, &data);
    js_msg_free(ctx, &key);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "%s failed", fn);
    return JS_NewArrayBufferCopy(ctx, out, outlen);
}

/* crypto.sha256(data) -> ArrayBuffer (32 bytes). `data` is any buffer, or a
 * string taken as its UTF-8 text. */
static JSValue js_crypto_sha256(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    JsMsg m;
    if (argc < 1 || !js_msg_get(ctx, argv[0], &m))
        return JS_ThrowTypeError(ctx, "crypto.sha256 requires (data)");
    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256((const char *)m.view.data, m.view.len, hash);
    js_msg_free(ctx, &m);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "sha256 failed");
    return JS_NewArrayBufferCopy(ctx, hash, sizeof hash);
}

/* crypto.sha1(data) -> ArrayBuffer (20 bytes).
 *
 * LEGACY INTEROP ONLY. SHA-1 is collision-broken; this exists for
 * third-party protocols that hardcode it (HIBP range API, etc.).
 * DO NOT use for new password hashing / MAC / digest needs - use
 * crypto.sha256 / crypto.hmacSha256 / crypto.hashPassword instead. */
static JSValue js_crypto_sha1(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    JsMsg m;
    if (argc < 1 || !js_msg_get(ctx, argv[0], &m))
        return JS_ThrowTypeError(ctx, "crypto.sha1 requires (data)");
    uint8_t hash[20];
    int rc = hl_cap_crypto_sha1((const char *)m.view.data, m.view.len, hash);
    js_msg_free(ctx, &m);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "sha1 failed");
    return JS_NewArrayBufferCopy(ctx, hash, sizeof hash);
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

/* crypto.randomToken(n [, "hex"]) -> an unguessable token: n random bytes
 * as unpadded base64url (the default; safe in URLs, cookies and file names)
 * or lowercase hex. The one way the stdlib makes ids, nonces, CSRF secrets
 * and session tokens, so none of them hand-roll the encoding. n is 1-1024. */
static JSValue js_crypto_random_token(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.randomToken requires (n)");
    int32_t n;
    if (JS_ToInt32(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    int hex = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        const char *fmt = JS_ToCString(ctx, argv[1]);
        if (!fmt) return JS_EXCEPTION;
        hex = strcmp(fmt, "hex") == 0;
        int ok = hex || strcmp(fmt, "base64url") == 0;
        JS_FreeCString(ctx, fmt);
        if (!ok)
            return JS_ThrowTypeError(ctx,
                "crypto.randomToken: format must be \"base64url\" or \"hex\"");
    }
    if (n <= 0 || n > HL_RANDOM_TOKEN_MAX)
        return JS_ThrowRangeError(ctx, "crypto.randomToken: bytes must be 1-%d",
                                  HL_RANDOM_TOKEN_MAX);

    uint8_t raw[HL_RANDOM_TOKEN_MAX];
    char out[HL_RANDOM_TOKEN_MAX * 2 + 1];
    if (hl_cap_crypto_random(raw, (size_t)n) != 0)
        return JS_ThrowInternalError(ctx, "random failed");
    int len = hex
        ? (hl_hex_encode(raw, (size_t)n, out, sizeof out) == 0 ? (int)n * 2 : -1)
        : hl_base64_encode(raw, (size_t)n, out, sizeof out, HL_BASE64_URL | HL_BASE64_NOPAD);
    if (len < 0)
        return JS_ThrowInternalError(ctx, "crypto.randomToken: encode failed");
    return JS_NewStringLen(ctx, out, (size_t)len);
}

/* Local 0/-1 wrapper over utils/hex's hl_hex_decode, kept for the
 * many existing callsites in this file. The actual decode lives
 * in utils/hex.c so all of Hull shares one implementation. */
static int hex_decode_compat(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2) return -1;
    return hl_hex_decode(hex, hex_len, out, out_len) >= 0 ? 0 : -1;
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
    hl_hex_encode(salt, sizeof salt, salt_hex, sizeof salt_hex);
    hl_hex_encode(hash, sizeof hash, hash_hex, sizeof hash_hex);

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
     * - Cosmopolitan libc doesn't support sscanf %[...] scansets). */
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

    uint8_t salt[16];
    if (hex_decode_compat(salt_hex, 32, salt, sizeof(salt)) != 0) {
        JS_FreeCString(ctx, pw); return JS_FALSE;
    }

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                               (int)iterations, computed, sizeof(computed)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_FALSE;
    }
    JS_FreeCString(ctx, pw);

    uint8_t stored_hash[32];
    if (hex_decode_compat(hash_hex, 64, stored_hash, sizeof(stored_hash)) != 0)
        return JS_FALSE;

    /* Constant-time comparison */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed[i] ^ stored_hash[i];

    secure_zero(computed, sizeof(computed));
    secure_zero(stored_hash, sizeof(stored_hash));
    secure_zero(salt, sizeof(salt));

    return diff == 0 ? JS_TRUE : JS_FALSE;
}


/* ── Ed25519 bindings ──────────────────────────────────────────────── */

/* crypto.ed25519Keypair() -> { publicKey: ArrayBuffer(32), secretKey: ArrayBuffer(64) } */
static JSValue js_crypto_ed25519_keypair(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0)
        return JS_ThrowInternalError(ctx, "ed25519 keypair generation failed");
    return js_keypair_object(ctx, pk, sizeof pk, sk, sizeof sk);
}

/* crypto.ed25519Sign(data, secretKey) -> ArrayBuffer (64-byte signature) */
static JSValue js_crypto_ed25519_sign(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.ed25519Sign requires (data, secretKey)");
    JsMsg data, sk;
    if (!js_msg_get(ctx, argv[0], &data))
        return JS_ThrowTypeError(ctx, "crypto.ed25519Sign: data must be a buffer or a string");
    if (!js_fixed_arg(ctx, argv[1], &sk, 64, "ed25519Sign", "secret key")) {
        js_msg_free(ctx, &data);
        return JS_EXCEPTION;
    }
    uint8_t sig[64];
    int rc = hl_cap_crypto_ed25519_sign(data.view.data, data.view.len,
                                        sk.view.data, sig);
    js_msg_free(ctx, &data);
    js_msg_free(ctx, &sk);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "ed25519 sign failed");
    return JS_NewArrayBufferCopy(ctx, sig, sizeof sig);
}

/* crypto.ed25519Verify(data, signature, publicKey) -> boolean */
static JSValue js_crypto_ed25519_verify(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.ed25519Verify requires (data, signature, publicKey)");
    JsMsg data, sig, pk;
    if (!js_msg_get(ctx, argv[0], &data))
        return JS_ThrowTypeError(ctx, "crypto.ed25519Verify: data must be a buffer or a string");
    if (!js_fixed_arg(ctx, argv[1], &sig, 64, "ed25519Verify", "signature")) {
        js_msg_free(ctx, &data);
        return JS_EXCEPTION;
    }
    if (!js_fixed_arg(ctx, argv[2], &pk, 32, "ed25519Verify", "public key")) {
        js_msg_free(ctx, &data);
        js_msg_free(ctx, &sig);
        return JS_EXCEPTION;
    }
    int rc = hl_cap_crypto_ed25519_verify(data.view.data, data.view.len,
                                          sig.view.data, pk.view.data);
    js_msg_free(ctx, &data);
    js_msg_free(ctx, &sig);
    js_msg_free(ctx, &pk);
    return JS_NewBool(ctx, rc == 0);
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

/* crypto.sign(alg, privatePem, data) -> ArrayBuffer
 *
 *  The inverse of crypto.verify: same algorithms, same signature encodings
 *  (RSA modulus-length, ECDSA raw r||s). privatePem is an unencrypted PEM
 *  private key of the family and curve `alg` names; anything else throws. */
static JSValue js_crypto_sign(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.sign requires (alg, privatePem, data)");
    size_t alg_len = 0, pk_len = 0;
    const char *alg_str = JS_ToCStringLen(ctx, &alg_len, argv[0]);
    if (!alg_str) return JS_EXCEPTION;
    HlCryptoAsymAlg alg = hl_crypto_asym_alg_from_string(alg_str, alg_len);
    JS_FreeCString(ctx, alg_str);
    if (alg == HL_CRYPTO_ASYM_NONE)
        return JS_ThrowTypeError(ctx, "crypto.sign: unsupported alg (use one of "
                                 "RS256/RS384/RS512/PS256/ES256/ES384)");
    const char *pk = JS_ToCStringLen(ctx, &pk_len, argv[1]);
    if (!pk) return JS_EXCEPTION;
    JsMsg data;
    if (!js_msg_get(ctx, argv[2], &data)) {
        JS_FreeCString(ctx, pk);
        return JS_ThrowTypeError(ctx, "crypto.sign: data must be a buffer or a string");
    }

    uint8_t sig[HL_CRYPTO_SIGN_MAX];
    size_t sig_len = 0;
    int rc = hl_cap_crypto_asym_sign_default(pk, pk_len, alg,
                                             data.view.data, data.view.len,
                                             sig, sizeof sig, &sig_len);
    JS_FreeCString(ctx, pk);
    js_msg_free(ctx, &data);
    if (rc == -2)
        return JS_ThrowInternalError(ctx, "crypto.sign: signing is not available in this build");
    if (rc != 0)
        return JS_ThrowTypeError(ctx, "crypto.sign: the key cannot sign under %s (malformed, "
                                 "encrypted, a public key, or the wrong key type)",
                                 hl_crypto_asym_alg_to_string(alg));
    return JS_NewArrayBufferCopy(ctx, sig, sig_len);
}

/* crypto.rsaPrivatePem(n, e, d, p, q) -> string
 *
 *  An RSA private key PEM from its components (big-endian bytes, as buffers).
 *  The CRT values are derived and the key checked; inconsistent components
 *  throw. The result is secret. */
static JSValue js_crypto_rsa_private_pem(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "crypto.rsaPrivatePem requires (n, e, d, p, q)");
    JsMsg m[5];
    int got = 0;
    for (; got < 5; got++) {
        if (!js_msg_get(ctx, argv[got], &m[got])) break;
    }
    if (got < 5) {
        for (int i = 0; i < got; i++) js_msg_free(ctx, &m[i]);
        return JS_ThrowTypeError(ctx, "crypto.rsaPrivatePem: components must be buffers");
    }
    HlCryptoRsaParts parts = {
        (const uint8_t *)m[0].view.data, m[0].view.len,
        (const uint8_t *)m[1].view.data, m[1].view.len,
        (const uint8_t *)m[2].view.data, m[2].view.len,
        (const uint8_t *)m[3].view.data, m[3].view.len,
        (const uint8_t *)m[4].view.data, m[4].view.len,
    };
    char pem[HL_CRYPTO_RSA_PEM_MAX];
    size_t pem_len = 0;
    int rc = hl_cap_crypto_rsa_private_pem(&parts, pem, sizeof pem, &pem_len);
    for (int i = 0; i < 5; i++) js_msg_free(ctx, &m[i]);
    if (rc == -2)
        return JS_ThrowInternalError(ctx, "crypto.rsaPrivatePem: not available in this build "
                                     "(or an empty component)");
    if (rc != 0)
        return JS_ThrowTypeError(ctx, "crypto.rsaPrivatePem: the components do not form "
                                 "a valid RSA key");
    JSValue out = JS_NewStringLen(ctx, pem, pem_len);
    secure_zero(pem, pem_len);
    return out;
}

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

/* crypto.x509PubkeyPem(der) -> pemString or null
 *
 *  Bridge from a base64-decoded X.509 certificate (DER) to the PEM-
 *  encoded SubjectPublicKeyInfo that crypto.verify consumes. Lets
 *  OIDC apps consume JWKS `x5c` entries directly. der must be an
 *  ArrayBuffer / Uint8Array (binary-safe); a JS string would
 *  UTF-8-inflate the cert bytes through JS_ToCStringLen.
 */
static JSValue js_crypto_x509_pubkey_pem(JSContext *ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "crypto.x509PubkeyPem requires (der)");

    HlBufferView view = {0};
    const char *str = NULL;
    int needs_free = 0;
    if (!js_get_buffer(ctx, argv[0], &view, &str, &needs_free))
        return JS_ThrowTypeError(ctx,
            "crypto.x509PubkeyPem: der must be ArrayBuffer or string");

    char pem[4096];
    size_t pem_len = 0;
    int rc = hl_cap_crypto_x509_pubkey_pem(view.data, view.len,
                                            pem, sizeof(pem), &pem_len);
    if (needs_free) JS_FreeCString(ctx, str);

    if (rc != 0) return JS_NULL;
    return JS_NewStringLen(ctx, pem, pem_len);
}

/* ── SHA-512 ──────────────────────────────────────────────────────── */

/* crypto.sha512(data) -> ArrayBuffer (64 bytes) */
static JSValue js_crypto_sha512(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    JsMsg m;
    if (argc < 1 || !js_msg_get(ctx, argv[0], &m))
        return JS_ThrowTypeError(ctx, "crypto.sha512 requires (data)");
    uint8_t hash[64];
    int rc = hl_cap_crypto_sha512((const char *)m.view.data, m.view.len, hash);
    js_msg_free(ctx, &m);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "sha512 failed");
    return JS_NewArrayBufferCopy(ctx, hash, sizeof hash);
}

/* ── HMAC-SHA512/256 auth ─────────────────────────────────────────── */

/* crypto.auth(msg, key) -> ArrayBuffer (32-byte tag; HMAC-SHA512/256, 32-byte key) */
static JSValue js_crypto_auth(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.auth requires (msg, key)");
    JsMsg msg, key;
    if (!js_msg_get(ctx, argv[0], &msg))
        return JS_ThrowTypeError(ctx, "crypto.auth: msg must be a buffer or a string");
    if (!js_fixed_arg(ctx, argv[1], &key, 32, "auth", "key")) {
        js_msg_free(ctx, &msg);
        return JS_EXCEPTION;
    }
    uint8_t tag[32];
    int rc = hl_cap_crypto_auth((const char *)msg.view.data, msg.view.len,
                                key.view.data, tag);
    js_msg_free(ctx, &msg);
    js_msg_free(ctx, &key);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "auth failed");
    return JS_NewArrayBufferCopy(ctx, tag, sizeof tag);
}

/* crypto.authVerify(tag, msg, key) -> boolean */
static JSValue js_crypto_auth_verify(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.authVerify requires (tag, msg, key)");
    JsMsg tag, msg, key;
    if (!js_fixed_arg(ctx, argv[0], &tag, 32, "authVerify", "tag"))
        return JS_EXCEPTION;
    if (!js_msg_get(ctx, argv[1], &msg)) {
        js_msg_free(ctx, &tag);
        return JS_ThrowTypeError(ctx, "crypto.authVerify: msg must be a buffer or a string");
    }
    if (!js_fixed_arg(ctx, argv[2], &key, 32, "authVerify", "key")) {
        js_msg_free(ctx, &tag);
        js_msg_free(ctx, &msg);
        return JS_EXCEPTION;
    }
    int rc = hl_cap_crypto_auth_verify(tag.view.data, (const char *)msg.view.data,
                                       msg.view.len, key.view.data);
    js_msg_free(ctx, &tag);
    js_msg_free(ctx, &msg);
    js_msg_free(ctx, &key);
    return JS_NewBool(ctx, rc == 0);
}

/* ── Secretbox ────────────────────────────────────────────────────── */

/* crypto.secretbox(msg, nonce, key) -> ArrayBuffer (msg + 16-byte tag);
 * nonce 24 bytes, key 32 bytes */
static JSValue js_crypto_secretbox(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.secretbox requires (msg, nonce, key)");
    JsMsg msg, nonce, key;
    if (!js_msg_get(ctx, argv[0], &msg))
        return JS_ThrowTypeError(ctx, "crypto.secretbox: msg must be a buffer or a string");
    if (!js_fixed_arg(ctx, argv[1], &nonce, 24, "secretbox", "nonce")) {
        js_msg_free(ctx, &msg);
        return JS_EXCEPTION;
    }
    if (!js_fixed_arg(ctx, argv[2], &key, 32, "secretbox", "key")) {
        js_msg_free(ctx, &msg);
        js_msg_free(ctx, &nonce);
        return JS_EXCEPTION;
    }
    JSValue ret;
    if (msg.view.len > SIZE_MAX - HL_SECRETBOX_MACBYTES) {
        ret = JS_ThrowRangeError(ctx, "crypto.secretbox: message too large");
    } else {
        size_t ct_len = msg.view.len + HL_SECRETBOX_MACBYTES;
        uint8_t *ct = js_malloc(ctx, ct_len);
        if (!ct) {
            ret = JS_EXCEPTION;
        } else if (hl_cap_crypto_secretbox(ct, (const char *)msg.view.data, msg.view.len,
                                           nonce.view.data, key.view.data) != 0) {
            js_free(ctx, ct);
            ret = JS_ThrowInternalError(ctx, "secretbox failed");
        } else {
            ret = JS_NewArrayBufferCopy(ctx, ct, ct_len);
            js_free(ctx, ct);
        }
    }
    js_msg_free(ctx, &msg);
    js_msg_free(ctx, &nonce);
    js_msg_free(ctx, &key);
    return ret;
}

/* crypto.secretboxOpen(ciphertext, nonce, key) -> ArrayBuffer | null */
static JSValue js_crypto_secretbox_open(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.secretboxOpen requires (ciphertext, nonce, key)");
    JsMsg ct, nonce, key;
    if (!js_msg_get(ctx, argv[0], &ct))
        return JS_ThrowTypeError(ctx, "crypto.secretboxOpen: ciphertext must be a buffer");
    if (!js_fixed_arg(ctx, argv[1], &nonce, 24, "secretboxOpen", "nonce")) {
        js_msg_free(ctx, &ct);
        return JS_EXCEPTION;
    }
    if (!js_fixed_arg(ctx, argv[2], &key, 32, "secretboxOpen", "key")) {
        js_msg_free(ctx, &ct);
        js_msg_free(ctx, &nonce);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_NULL;
    if (ct.view.len >= HL_SECRETBOX_MACBYTES) {
        size_t msg_len = ct.view.len - HL_SECRETBOX_MACBYTES;
        uint8_t *msg = js_malloc(ctx, msg_len + 1);
        if (!msg) {
            ret = JS_EXCEPTION;
        } else {
            if (hl_cap_crypto_secretbox_open(msg, ct.view.data, ct.view.len,
                                             nonce.view.data, key.view.data) == 0)
                ret = JS_NewArrayBufferCopy(ctx, msg, msg_len);
            js_free(ctx, msg);
        }
    }
    js_msg_free(ctx, &ct);
    js_msg_free(ctx, &nonce);
    js_msg_free(ctx, &key);
    return ret;
}

/* ── Box (public-key encryption) ──────────────────────────────────── */

/* crypto.box(msg, nonce, publicKey, secretKey) -> ArrayBuffer;
 * nonce 24 bytes, keys 32 bytes */
static JSValue js_crypto_box(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "crypto.box requires (msg, nonce, publicKey, secretKey)");
    JsMsg msg, nonce, pk, sk;
    if (!js_msg_get(ctx, argv[0], &msg))
        return JS_ThrowTypeError(ctx, "crypto.box: msg must be a buffer or a string");
    if (!js_fixed_arg(ctx, argv[1], &nonce, 24, "box", "nonce")) {
        js_msg_free(ctx, &msg);
        return JS_EXCEPTION;
    }
    if (!js_fixed_arg(ctx, argv[2], &pk, 32, "box", "public key")) {
        js_msg_free(ctx, &msg); js_msg_free(ctx, &nonce);
        return JS_EXCEPTION;
    }
    if (!js_fixed_arg(ctx, argv[3], &sk, 32, "box", "secret key")) {
        js_msg_free(ctx, &msg); js_msg_free(ctx, &nonce); js_msg_free(ctx, &pk);
        return JS_EXCEPTION;
    }
    JSValue ret;
    if (msg.view.len > SIZE_MAX - HL_BOX_MACBYTES) {
        ret = JS_ThrowRangeError(ctx, "crypto.box: message too large");
    } else {
        size_t ct_len = msg.view.len + HL_BOX_MACBYTES;
        uint8_t *ct = js_malloc(ctx, ct_len);
        if (!ct) {
            ret = JS_EXCEPTION;
        } else if (hl_cap_crypto_box(ct, (const char *)msg.view.data, msg.view.len,
                                     nonce.view.data, pk.view.data, sk.view.data) != 0) {
            js_free(ctx, ct);
            ret = JS_ThrowInternalError(ctx, "box failed");
        } else {
            ret = JS_NewArrayBufferCopy(ctx, ct, ct_len);
            js_free(ctx, ct);
        }
    }
    js_msg_free(ctx, &msg); js_msg_free(ctx, &nonce);
    js_msg_free(ctx, &pk); js_msg_free(ctx, &sk);
    return ret;
}

/* crypto.boxOpen(ciphertext, nonce, publicKey, secretKey) -> ArrayBuffer | null */
static JSValue js_crypto_box_open(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "crypto.boxOpen requires (ciphertext, nonce, publicKey, secretKey)");
    JsMsg ct, nonce, pk, sk;
    if (!js_msg_get(ctx, argv[0], &ct))
        return JS_ThrowTypeError(ctx, "crypto.boxOpen: ciphertext must be a buffer");
    if (!js_fixed_arg(ctx, argv[1], &nonce, 24, "boxOpen", "nonce")) {
        js_msg_free(ctx, &ct);
        return JS_EXCEPTION;
    }
    if (!js_fixed_arg(ctx, argv[2], &pk, 32, "boxOpen", "public key")) {
        js_msg_free(ctx, &ct); js_msg_free(ctx, &nonce);
        return JS_EXCEPTION;
    }
    if (!js_fixed_arg(ctx, argv[3], &sk, 32, "boxOpen", "secret key")) {
        js_msg_free(ctx, &ct); js_msg_free(ctx, &nonce); js_msg_free(ctx, &pk);
        return JS_EXCEPTION;
    }
    JSValue ret = JS_NULL;
    if (ct.view.len >= HL_BOX_MACBYTES) {
        size_t msg_len = ct.view.len - HL_BOX_MACBYTES;
        uint8_t *msg = js_malloc(ctx, msg_len + 1);
        if (!msg) {
            ret = JS_EXCEPTION;
        } else {
            if (hl_cap_crypto_box_open(msg, ct.view.data, ct.view.len, nonce.view.data,
                                       pk.view.data, sk.view.data) == 0)
                ret = JS_NewArrayBufferCopy(ctx, msg, msg_len);
            js_free(ctx, msg);
        }
    }
    js_msg_free(ctx, &ct); js_msg_free(ctx, &nonce);
    js_msg_free(ctx, &pk); js_msg_free(ctx, &sk);
    return ret;
}

/* crypto.boxKeypair() -> { publicKey: ArrayBuffer(32), secretKey: ArrayBuffer(32) } */
static JSValue js_crypto_box_keypair(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    uint8_t pk[32], sk[32];
    if (hl_cap_crypto_box_keypair(pk, sk) != 0)
        return JS_ThrowInternalError(ctx, "box keypair generation failed");
    return js_keypair_object(ctx, pk, sizeof pk, sk, sizeof sk);
}

/* crypto.x25519Keypair() -> { publicKey: ArrayBuffer(32), secretKey: ArrayBuffer(32) } */
static JSValue js_crypto_x25519_keypair(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    uint8_t pk[32], sk[32];
    if (hl_cap_crypto_x25519_keypair(pk, sk) != 0)
        return JS_ThrowInternalError(ctx, "x25519 keypair generation failed");
    return js_keypair_object(ctx, pk, sizeof pk, sk, sizeof sk);
}

/* crypto.x25519(secretKey, publicKey) -> ArrayBuffer (32-byte shared secret),
 * or null when the peer sent a low-order point (a protocol event to handle,
 * not a programming error). */
static JSValue js_crypto_x25519(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.x25519 requires (secretKey, publicKey)");
    JsMsg sk, pk;
    if (!js_fixed_arg(ctx, argv[0], &sk, 32, "x25519", "secret key"))
        return JS_EXCEPTION;
    if (!js_fixed_arg(ctx, argv[1], &pk, 32, "x25519", "public key")) {
        js_msg_free(ctx, &sk);
        return JS_EXCEPTION;
    }
    uint8_t shared[32];
    int rc = hl_cap_crypto_x25519(shared, sk.view.data, pk.view.data);
    js_msg_free(ctx, &sk);
    js_msg_free(ctx, &pk);
    if (rc == -2)
        return JS_NULL;
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "x25519 failed");
    JSValue ret = JS_NewArrayBufferCopy(ctx, shared, sizeof shared);
    secure_zero(shared, sizeof shared);
    return ret;
}

/* crypto.hmacSha256(data, key) -> ArrayBuffer (32 bytes); the key is any
 * non-empty buffer or string */
static JSValue js_crypto_hmac_sha256(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    uint8_t out[32];
    return js_hmac(ctx, argc, argv, "hmacSha256", out, sizeof out,
                   hl_cap_crypto_hmac_sha256);
}

/* crypto.hmacSha1(data, key) -> ArrayBuffer (20 bytes). HOTP/TOTP only. */
static JSValue js_crypto_hmac_sha1(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    uint8_t out[20];
    return js_hmac(ctx, argc, argv, "hmacSha1", out, sizeof out,
                   hl_cap_crypto_hmac_sha1);
}

/* crypto.hmacSha256Verify(data, key, expected) -> boolean; `expected` is the
 * 32-byte MAC, compared in constant time */
static JSValue js_crypto_hmac_sha256_verify(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256Verify requires (data, key, expected)");
    JsMsg data, key, exp;
    if (!js_msg_get(ctx, argv[0], &data))
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256Verify: data must be a buffer or a string");
    if (!js_msg_get(ctx, argv[1], &key) || key.view.len == 0) {
        js_msg_free(ctx, &data);
        js_msg_free(ctx, &key);
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256Verify: key must be a non-empty buffer or string");
    }
    if (!js_msg_get(ctx, argv[2], &exp)) {
        js_msg_free(ctx, &data);
        js_msg_free(ctx, &key);
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256Verify: expected must be a buffer");
    }
    int ok = exp.view.len == 32
        && hl_cap_crypto_hmac_sha256_verify(key.view.data, key.view.len,
                                            data.view.data, data.view.len,
                                            exp.view.data) == 0;
    js_msg_free(ctx, &data);
    js_msg_free(ctx, &key);
    js_msg_free(ctx, &exp);
    return JS_NewBool(ctx, ok);
}

/* crypto.constantTimeEq(a, b) -> boolean
 *
 * Constant-time equality of two strings, compared in C so the timing is not
 * subject to interpreter variance. Length is NOT secret (callers compare
 * fixed-size digests/MACs), so a length mismatch returns false immediately;
 * equal-length inputs compare with no early exit. Used by jwt/csrf. */
static JSValue js_crypto_constant_time_eq(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.constantTimeEq requires (a, b)");
    JsMsg m_a, m_b;
    if (!js_msg_get(ctx, argv[0], &m_a))
        return JS_ThrowTypeError(ctx, "crypto.constantTimeEq: argument 1 must be a buffer or a string");
    if (!js_msg_get(ctx, argv[1], &m_b)) {
        js_msg_free(ctx, &m_a);
        return JS_ThrowTypeError(ctx, "crypto.constantTimeEq: argument 2 must be a buffer or a string");
    }
    const char *a = (const char *)m_a.view.data;
    size_t alen = m_a.view.len;
    const char *b = (const char *)m_b.view.data;
    size_t blen = m_b.view.len;
    int eq;
    if (alen != blen) {
        eq = 0;
    } else {
        unsigned diff = 0;
        for (size_t i = 0; i < alen; i++)
            diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
        eq = (diff == 0);
    }
    js_msg_free(ctx, &m_a);
    js_msg_free(ctx, &m_b);
    return JS_NewBool(ctx, eq);
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
    /* Scrub any in-flight state from abandoned hashers - _final does the
     * same on the normal path. */
    if (!h->done) memset(&h->ctx, 0, sizeof(h->ctx));
    js_free_rt(rt, h);
}

static const JSClassDef js_sha256_hasher_class = {
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

    /* Any buffer, or a string taken as its UTF-8 text. */
    JsMsg m;
    if (!js_msg_get(ctx, argv[0], &m))
        return JS_ThrowTypeError(ctx, "sha256.update: data must be a buffer or a string");
    int rc = hl_cap_crypto_sha256_update(&h->ctx, m.view.data, m.view.len);
    js_msg_free(ctx, &m);
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
    return JS_NewArrayBufferCopy(ctx, out, sizeof out);
}

static JSValue js_crypto_create_sha256(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    /* js_malloc routes through QuickJS's allocator, so the runtime's
     * configured memory cap covers Hasher instances - raw malloc would
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

/* ── Keys held in C ─────────────────────────────────────────────────── *
 *
 * crypto.keyFromEnv(name) -> CryptoKey. A 32-byte secretbox key read from an
 * environment variable (64 hex digits or base64, under manifest.env) into
 * memory the C layer owns. The script gets a handle and never the bytes:
 * key.secretbox / key.secretboxOpen seal with it; key.destroy() or the
 * collector zeroes it. hull:crypto:sealbox keyrings take one in place of a
 * byte string. Same contract as the Lua crypto.key_from_env. */

static JSClassID hl_js_crypto_key_class_id;

typedef struct {
    HlCryptoKey *k;
    char name[64];              /* the variable's name, for toString; not secret */
} HlJsCryptoKey;

static void js_crypto_key_finalizer(JSRuntime *rt, JSValue val)
{
    HlJsCryptoKey *h = JS_GetOpaque(val, hl_js_crypto_key_class_id);
    if (!h) return;
    hl_cap_crypto_key_free(h->k);
    js_free_rt(rt, h);
}

static const JSClassDef js_crypto_key_class = {
    "CryptoKey",
    .finalizer = js_crypto_key_finalizer,
};

static const HlCryptoKey *js_live_key(JSContext *ctx, JSValueConst this_val)
{
    HlJsCryptoKey *h = JS_GetOpaque2(ctx, this_val, hl_js_crypto_key_class_id);
    if (!h) return NULL;
    if (!h->k) {
        JS_ThrowTypeError(ctx, "crypto key: the key has been destroyed");
        return NULL;
    }
    return h->k;
}

static JSValue js_crypto_key_secretbox(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "key.secretbox requires (message, nonce)");
    JsMsg msg, nonce;
    if (!js_msg_get(ctx, argv[0], &msg))
        return JS_ThrowTypeError(ctx, "key.secretbox: message must be a buffer or a string");
    if (!js_fixed_arg(ctx, argv[1], &nonce, 24, "key.secretbox", "nonce")) {
        js_msg_free(ctx, &msg);
        return JS_EXCEPTION;
    }
    /* The key last: converting the arguments can run app code, which can
     * key.destroy() - the key resolved first was then freed under us. */
    const HlCryptoKey *k = js_live_key(ctx, this_val);
    if (!k) { js_msg_free(ctx, &msg); js_msg_free(ctx, &nonce); return JS_EXCEPTION; }
    JSValue ret = JS_EXCEPTION;
    size_t ct_len = msg.view.len + HL_SECRETBOX_MACBYTES;
    uint8_t *ct = js_malloc(ctx, ct_len);
    if (ct) {
        if (hl_cap_crypto_key_secretbox(k, ct, msg.view.data, msg.view.len,
                                        nonce.view.data) == 0)
            ret = JS_NewArrayBufferCopy(ctx, ct, ct_len);
        else
            ret = JS_ThrowInternalError(ctx, "key.secretbox failed");
        js_free(ctx, ct);
    }
    js_msg_free(ctx, &msg);
    js_msg_free(ctx, &nonce);
    return ret;
}

/* key.secretboxOpen(ciphertext, nonce) -> ArrayBuffer | null */
static JSValue js_crypto_key_secretbox_open(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "key.secretboxOpen requires (ciphertext, nonce)");
    JsMsg ct, nonce;
    if (!js_msg_get(ctx, argv[0], &ct))
        return JS_ThrowTypeError(ctx, "key.secretboxOpen: ciphertext must be a buffer");
    if (!js_fixed_arg(ctx, argv[1], &nonce, 24, "key.secretboxOpen", "nonce")) {
        js_msg_free(ctx, &ct);
        return JS_EXCEPTION;
    }
    /* The key last: see key.secretbox. */
    const HlCryptoKey *k = js_live_key(ctx, this_val);
    if (!k) { js_msg_free(ctx, &ct); js_msg_free(ctx, &nonce); return JS_EXCEPTION; }
    JSValue ret = JS_NULL;
    if (ct.view.len >= HL_SECRETBOX_MACBYTES) {
        size_t msg_len = ct.view.len - HL_SECRETBOX_MACBYTES;
        uint8_t *out = js_malloc(ctx, msg_len + 1);
        if (!out) {
            ret = JS_EXCEPTION;
        } else {
            if (hl_cap_crypto_key_secretbox_open(k, out, ct.view.data, ct.view.len,
                                                 nonce.view.data) == 0)
                ret = JS_NewArrayBufferCopy(ctx, out, msg_len);
            js_free(ctx, out);
        }
    }
    js_msg_free(ctx, &ct);
    js_msg_free(ctx, &nonce);
    return ret;
}

/* Zero it now rather than whenever the collector runs. Idempotent. */
static JSValue js_crypto_key_destroy(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsCryptoKey *h = JS_GetOpaque2(ctx, this_val, hl_js_crypto_key_class_id);
    if (!h) return JS_EXCEPTION;
    hl_cap_crypto_key_free(h->k);
    h->k = NULL;
    return JS_UNDEFINED;
}

static JSValue js_crypto_key_to_string(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsCryptoKey *h = JS_GetOpaque2(ctx, this_val, hl_js_crypto_key_class_id);
    if (!h) return JS_EXCEPTION;
    char buf[96];
    snprintf(buf, sizeof buf, "crypto.key(%s%s)", h->name, h->k ? "" : ", destroyed");
    return JS_NewString(ctx, buf);
}

static JSValue js_crypto_key_from_env(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.keyFromEnv requires (name)");
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.env_cfg)
        return JS_ThrowInternalError(ctx, "crypto.keyFromEnv: no env capability");
    const char *var = JS_ToCString(ctx, argv[0]);
    if (!var) return JS_EXCEPTION;

    HlJsCryptoKey *h = js_malloc(ctx, sizeof *h);
    if (!h) { JS_FreeCString(ctx, var); return JS_ThrowOutOfMemory(ctx); }
    h->k = NULL;
    snprintf(h->name, sizeof h->name, "%s", var);

    const char *val = hl_cap_env_get(js->base.env_cfg, var);
    if (!val || !*val) {
        /* One message for "not declared" and "unset", as the Lua twin. */
        JSValue e = JS_ThrowTypeError(ctx, "crypto.keyFromEnv: '%s' is not available "
                                      "(declare it in manifest.env and set it)", var);
        JS_FreeCString(ctx, var);
        js_free(ctx, h);
        return e;
    }
    if (hl_cap_crypto_key_from_text(val, strlen(val), &h->k) != 0) {
        JSValue e = JS_ThrowTypeError(ctx, "crypto.keyFromEnv: '%s' is not a 32-byte "
                                      "key (64 hex digits or base64)", var);
        JS_FreeCString(ctx, var);
        js_free(ctx, h);
        return e;
    }
    JS_FreeCString(ctx, var);

    JSValue obj = JS_NewObjectClass(ctx, (int)hl_js_crypto_key_class_id);
    if (JS_IsException(obj)) {
        hl_cap_crypto_key_free(h->k);
        js_free(ctx, h);
        return obj;
    }
    JS_SetOpaque(obj, h);
    return obj;
}

static void js_register_crypto_key_class(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(&hl_js_crypto_key_class_id);
    JS_NewClass(rt, hl_js_crypto_key_class_id, &js_crypto_key_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "secretbox",
        JS_NewCFunction(ctx, js_crypto_key_secretbox, "secretbox", 2));
    JS_SetPropertyStr(ctx, proto, "secretboxOpen",
        JS_NewCFunction(ctx, js_crypto_key_secretbox_open, "secretboxOpen", 2));
    JS_SetPropertyStr(ctx, proto, "destroy",
        JS_NewCFunction(ctx, js_crypto_key_destroy, "destroy", 0));
    JS_SetPropertyStr(ctx, proto, "toString",
        JS_NewCFunction(ctx, js_crypto_key_to_string, "toString", 0));
    JS_SetClassProto(ctx, hl_js_crypto_key_class_id, proto);
}

static int js_crypto_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/crypto", "hull:crypto") != 0)
        return -1;

    js_register_sha256_hasher_class(ctx);
    js_register_crypto_key_class(ctx);

    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "sha256",
                      JS_NewCFunction(ctx, js_crypto_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto, "keyFromEnv",
                      JS_NewCFunction(ctx, js_crypto_key_from_env, "keyFromEnv", 1));
    JS_SetPropertyStr(ctx, crypto, "createSha256",
                      JS_NewCFunction(ctx, js_crypto_create_sha256, "createSha256", 0));
    JS_SetPropertyStr(ctx, crypto, "sha512",
                      JS_NewCFunction(ctx, js_crypto_sha512, "sha512", 1));
    JS_SetPropertyStr(ctx, crypto, "sha1",
                      JS_NewCFunction(ctx, js_crypto_sha1, "sha1", 1));
    JS_SetPropertyStr(ctx, crypto, "random",
                      JS_NewCFunction(ctx, js_crypto_random, "random", 1));
    JS_SetPropertyStr(ctx, crypto, "randomToken",
                      JS_NewCFunction(ctx, js_crypto_random_token, "randomToken", 2));
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
    JS_SetPropertyStr(ctx, crypto, "sign",
                      JS_NewCFunction(ctx, js_crypto_sign, "sign", 3));
    JS_SetPropertyStr(ctx, crypto, "rsaPrivatePem",
                      JS_NewCFunction(ctx, js_crypto_rsa_private_pem, "rsaPrivatePem", 5));
    JS_SetPropertyStr(ctx, crypto, "x509PubkeyPem",
                      JS_NewCFunction(ctx, js_crypto_x509_pubkey_pem,
                                       "x509PubkeyPem", 1));
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
    JS_SetPropertyStr(ctx, crypto, "x25519",
                      JS_NewCFunction(ctx, js_crypto_x25519, "x25519", 2));
    JS_SetPropertyStr(ctx, crypto, "x25519Keypair",
                      JS_NewCFunction(ctx, js_crypto_x25519_keypair, "x25519Keypair", 0));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256",
                      JS_NewCFunction(ctx, js_crypto_hmac_sha256, "hmacSha256", 2));
    JS_SetPropertyStr(ctx, crypto, "hmacSha1",
                      JS_NewCFunction(ctx, js_crypto_hmac_sha1, "hmacSha1", 2));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256Verify",
                      JS_NewCFunction(ctx, js_crypto_hmac_sha256_verify, "hmacSha256Verify", 3));
    JS_SetPropertyStr(ctx, crypto, "constantTimeEq",
                      JS_NewCFunction(ctx, js_crypto_constant_time_eq, "constantTimeEq", 2));
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
