/* mod_crypto.c — hull.crypto module: hashing, encryption, signatures
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/crypto.h"
#include "hull/limits.h"

#include <sh_arena.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.crypto module
 *
 * crypto.sha256(data)                → hex string
 * crypto.random(n)                   → string of n random bytes
 * crypto.hash_password(password)     → hash string
 * crypto.verify_password(pw, hash)   → boolean
 * crypto.ed25519_keypair()           → pubkey_hex, secret_key_hex
 * crypto.ed25519_sign(data, sk_hex)  → signature_hex
 * crypto.ed25519_verify(data, sig_hex, pk_hex) → boolean
 * ════════════════════════════════════════════════════════════════════ */

static int lua_crypto_sha256(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);

    uint8_t hash[32];
    if (hl_cap_crypto_sha256(data, len, hash) != 0)
        return luaL_error(L, "sha256 failed");

    /* Convert to hex string */
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = '\0';

    lua_pushstring(L, hex);
    return 1;
}

static int lua_crypto_random(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n <= 0 || n > HL_RANDOM_MAX_BYTES)
        return luaL_error(L, "random bytes must be 1-%d", HL_RANDOM_MAX_BYTES);

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *buf = sh_arena_alloc(lua->scratch, (size_t)n);
    if (!buf)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_random(buf, (size_t)n) != 0)
        return luaL_error(L, "random failed");

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

/* crypto.hash_password(password) → "pbkdf2:iterations:salt_hex:hash_hex" */
static int lua_crypto_hash_password(lua_State *L)
{
    size_t pw_len;
    const char *pw = luaL_checklstring(L, 1, &pw_len);

    /* Generate 16-byte salt */
    uint8_t salt[16];
    if (hl_cap_crypto_random(salt, sizeof(salt)) != 0)
        return luaL_error(L, "random failed");

    /* PBKDF2-HMAC-SHA256, 32-byte output */
    uint8_t hash[32];
    int iterations = HL_PBKDF2_ITERATIONS;
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                                iterations, hash, sizeof(hash)) != 0)
        return luaL_error(L, "pbkdf2 failed");

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

    lua_pushstring(L, result);
    return 1;
}

/* ── Hex nibble helper (no sscanf — Cosmopolitan compat) ──────────── */

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* crypto.verify_password(password, hash_string) → boolean */
static int lua_crypto_verify_password(lua_State *L)
{
    size_t pw_len;
    const char *pw = luaL_checklstring(L, 1, &pw_len);
    const char *stored = luaL_checkstring(L, 2);

    /* Parse "pbkdf2:iterations:salt_hex:hash_hex" manually (no scansets
     * — Cosmopolitan libc doesn't support sscanf %[...] scansets). */
    if (strncmp(stored, "pbkdf2:", 7) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const char *p = stored + 7;

    /* Parse iterations */
    char *end = NULL;
    long iterations = strtol(p, &end, 10);
    if (!end || *end != ':' || iterations < 100000) {
        lua_pushboolean(L, 0);
        return 1;
    }
    p = end + 1;

    /* Read 32-char salt hex */
    if (strlen(p) < 32 + 1 + 64 || p[32] != ':') {
        lua_pushboolean(L, 0);
        return 1;
    }
    char salt_hex[33];
    memcpy(salt_hex, p, 32);
    salt_hex[32] = '\0';
    p += 33;

    /* Read 64-char hash hex */
    if (strlen(p) < 64) {
        lua_pushboolean(L, 0);
        return 1;
    }
    char hash_hex[65];
    memcpy(hash_hex, p, 64);
    hash_hex[64] = '\0';

    /* Decode hex salt (manual — sscanf %x broken on Cosmopolitan) */
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble((unsigned char)salt_hex[i * 2]);
        int lo = hex_nibble((unsigned char)salt_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { lua_pushboolean(L, 0); return 1; }
        salt[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                                (int)iterations, computed, sizeof(computed)) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Decode stored hash and compare (constant-time) */
    uint8_t stored_hash[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble((unsigned char)hash_hex[i * 2]);
        int lo = hex_nibble((unsigned char)hash_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { lua_pushboolean(L, 0); return 1; }
        stored_hash[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Constant-time comparison */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed[i] ^ stored_hash[i];

    secure_zero(computed, sizeof(computed));
    secure_zero(stored_hash, sizeof(stored_hash));
    secure_zero(salt, sizeof(salt));

    lua_pushboolean(L, diff == 0);
    return 1;
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

/* crypto.ed25519_keypair() → pubkey_hex, secret_key_hex */
static int lua_crypto_ed25519_keypair(lua_State *L)
{
    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0)
        return luaL_error(L, "ed25519 keypair generation failed");

    char pk_hex[65], sk_hex[129];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 64; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[128] = '\0';

    lua_pushstring(L, pk_hex);
    lua_pushstring(L, sk_hex);
    secure_zero(sk, sizeof(sk));
    secure_zero(sk_hex, sizeof(sk_hex));
    return 2;
}

/* crypto.ed25519_sign(data, secret_key_hex) → signature_hex */
static int lua_crypto_ed25519_sign(lua_State *L)
{
    size_t data_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    size_t sk_hex_len;
    const char *sk_hex = luaL_checklstring(L, 2, &sk_hex_len);

    if (sk_hex_len != 128)
        return luaL_error(L, "secret key must be 128 hex chars (64 bytes)");

    uint8_t sk[64];
    if (hex_decode(sk_hex, sk_hex_len, sk, 64) != 0)
        return luaL_error(L, "invalid hex in secret key");

    uint8_t sig[64];
    if (hl_cap_crypto_ed25519_sign((const uint8_t *)data, data_len, sk, sig) != 0) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "ed25519 sign failed");
    }

    secure_zero(sk, sizeof(sk));

    char sig_hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(sig_hex + i * 2, 3, "%02x", sig[i]);
    sig_hex[128] = '\0';

    lua_pushstring(L, sig_hex);
    return 1;
}

/* crypto.ed25519_verify(data, signature_hex, pubkey_hex) → boolean */
static int lua_crypto_ed25519_verify(lua_State *L)
{
    size_t data_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    size_t sig_hex_len;
    const char *sig_hex = luaL_checklstring(L, 2, &sig_hex_len);
    size_t pk_hex_len;
    const char *pk_hex = luaL_checklstring(L, 3, &pk_hex_len);

    if (sig_hex_len != 128)
        return luaL_error(L, "signature must be 128 hex chars (64 bytes)");
    if (pk_hex_len != 64)
        return luaL_error(L, "public key must be 64 hex chars (32 bytes)");

    uint8_t sig[64], pk[32];
    if (hex_decode(sig_hex, sig_hex_len, sig, 64) != 0)
        return luaL_error(L, "invalid hex in signature");
    if (hex_decode(pk_hex, pk_hex_len, pk, 32) != 0)
        return luaL_error(L, "invalid hex in public key");

    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)data, data_len, sig, pk);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── SHA-512 ───────────────────────────────────────────────────────── */

/* crypto.sha512(data) → hex string (128 chars) */
static int lua_crypto_sha512(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);

    uint8_t hash[64];
    if (hl_cap_crypto_sha512(data, len, hash) != 0)
        return luaL_error(L, "sha512 failed");

    char hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[128] = '\0';

    lua_pushstring(L, hex);
    return 1;
}

/* ── HMAC-SHA512/256 authentication ────────────────────────────────── */

/* crypto.auth(msg, key_hex) → tag_hex (64 chars) */
static int lua_crypto_auth(lua_State *L)
{
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 1, &msg_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 2, &key_hex_len);

    if (key_hex_len != 64)
        return luaL_error(L, "auth key must be 64 hex chars (32 bytes)");

    uint8_t key[32];
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in auth key");

    uint8_t tag[32];
    if (hl_cap_crypto_auth(msg, msg_len, key, tag) != 0) {
        secure_zero(key, sizeof(key));
        return luaL_error(L, "auth failed");
    }
    secure_zero(key, sizeof(key));

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", tag[i]);
    hex[64] = '\0';

    lua_pushstring(L, hex);
    return 1;
}

/* crypto.auth_verify(tag_hex, msg, key_hex) → boolean */
static int lua_crypto_auth_verify(lua_State *L)
{
    size_t tag_hex_len;
    const char *tag_hex = luaL_checklstring(L, 1, &tag_hex_len);
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 2, &msg_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 3, &key_hex_len);

    if (tag_hex_len != 64)
        return luaL_error(L, "tag must be 64 hex chars (32 bytes)");
    if (key_hex_len != 64)
        return luaL_error(L, "auth key must be 64 hex chars (32 bytes)");

    uint8_t tag[32], key[32];
    if (hex_decode(tag_hex, tag_hex_len, tag, 32) != 0)
        return luaL_error(L, "invalid hex in tag");
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in key");

    int rc = hl_cap_crypto_auth_verify(tag, msg, msg_len, key);
    secure_zero(key, sizeof(key));
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── Secret-key authenticated encryption (XSalsa20+Poly1305) ──────── */

/* crypto.secretbox(msg, nonce_hex, key_hex) → ciphertext_hex */
static int lua_crypto_secretbox(lua_State *L)
{
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 1, &msg_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 3, &key_hex_len);

    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (key_hex_len != 64)
        return luaL_error(L, "key must be 64 hex chars (32 bytes)");

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in key");

    if (msg_len > SIZE_MAX - HL_SECRETBOX_MACBYTES)
        return luaL_error(L, "message too large");
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_secretbox(ct, msg, msg_len, nonce, key) != 0) {
        secure_zero(key, sizeof(key));
        return luaL_error(L, "secretbox failed");
    }
    secure_zero(key, sizeof(key));

    /* Convert to hex */
    if (ct_len > SIZE_MAX / 2)
        return luaL_error(L, "ciphertext too large");
    size_t hex_len = ct_len * 2 + 1;
    char *hex = sh_arena_alloc(lua->scratch, hex_len);
    if (!hex)
        return luaL_error(L, "out of memory");

    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);

    lua_pushstring(L, hex);
    return 1;
}

/* crypto.secretbox_open(ct_hex, nonce_hex, key_hex) → string or nil */
static int lua_crypto_secretbox_open(lua_State *L)
{
    size_t ct_hex_len;
    const char *ct_hex = luaL_checklstring(L, 1, &ct_hex_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 3, &key_hex_len);

    if (ct_hex_len % 2 != 0)
        return luaL_error(L, "ciphertext hex must have even length");
    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (key_hex_len != 64)
        return luaL_error(L, "key must be 64 hex chars (32 bytes)");

    size_t ct_len = ct_hex_len / 2;
    if (ct_len < HL_SECRETBOX_MACBYTES) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0)
        return luaL_error(L, "invalid hex in key");

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct)
        return luaL_error(L, "out of memory");
    if (hex_decode(ct_hex, ct_hex_len, ct, ct_len) != 0)
        return luaL_error(L, "invalid hex in ciphertext");

    size_t msg_len = ct_len - HL_SECRETBOX_MACBYTES;
    uint8_t *msg = sh_arena_alloc(lua->scratch, msg_len + 1);
    if (!msg)
        return luaL_error(L, "out of memory");

    if (hl_cap_crypto_secretbox_open(msg, ct, ct_len, nonce, key) != 0) {
        secure_zero(key, sizeof(key));
        lua_pushnil(L);
        return 1;
    }
    secure_zero(key, sizeof(key));

    lua_pushlstring(L, (const char *)msg, msg_len);
    return 1;
}

/* ── Public-key authenticated encryption (Curve25519+XSalsa20+Poly1305) */

/* crypto.box(msg, nonce_hex, pk_hex, sk_hex) → ciphertext_hex */
static int lua_crypto_box(lua_State *L)
{
    size_t msg_len;
    const char *msg = luaL_checklstring(L, 1, &msg_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t pk_hex_len;
    const char *pk_hex = luaL_checklstring(L, 3, &pk_hex_len);
    size_t sk_hex_len;
    const char *sk_hex = luaL_checklstring(L, 4, &sk_hex_len);

    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (pk_hex_len != 64)
        return luaL_error(L, "public key must be 64 hex chars (32 bytes)");
    if (sk_hex_len != 64)
        return luaL_error(L, "secret key must be 64 hex chars (32 bytes)");

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(pk_hex, pk_hex_len, pk, 32) != 0)
        return luaL_error(L, "invalid hex in public key");
    if (hex_decode(sk_hex, sk_hex_len, sk, 32) != 0) {
        return luaL_error(L, "invalid hex in secret key");
    }

    if (msg_len > SIZE_MAX - HL_BOX_MACBYTES) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "message too large");
    }
    size_t ct_len = msg_len + HL_BOX_MACBYTES;
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "runtime not available");
    }

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "out of memory");
    }

    if (hl_cap_crypto_box(ct, msg, msg_len, nonce, pk, sk) != 0) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "box failed");
    }
    secure_zero(sk, sizeof(sk));

    if (ct_len > SIZE_MAX / 2)
        return luaL_error(L, "ciphertext too large");
    size_t hex_len = ct_len * 2 + 1;
    char *hex = sh_arena_alloc(lua->scratch, hex_len);
    if (!hex)
        return luaL_error(L, "out of memory");

    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);

    lua_pushstring(L, hex);
    return 1;
}

/* crypto.box_open(ct_hex, nonce_hex, pk_hex, sk_hex) → string or nil */
static int lua_crypto_box_open(lua_State *L)
{
    size_t ct_hex_len;
    const char *ct_hex = luaL_checklstring(L, 1, &ct_hex_len);
    size_t nonce_hex_len;
    const char *nonce_hex = luaL_checklstring(L, 2, &nonce_hex_len);
    size_t pk_hex_len;
    const char *pk_hex = luaL_checklstring(L, 3, &pk_hex_len);
    size_t sk_hex_len;
    const char *sk_hex = luaL_checklstring(L, 4, &sk_hex_len);

    if (ct_hex_len % 2 != 0)
        return luaL_error(L, "ciphertext hex must have even length");
    if (nonce_hex_len != 48)
        return luaL_error(L, "nonce must be 48 hex chars (24 bytes)");
    if (pk_hex_len != 64)
        return luaL_error(L, "public key must be 64 hex chars (32 bytes)");
    if (sk_hex_len != 64)
        return luaL_error(L, "secret key must be 64 hex chars (32 bytes)");

    size_t ct_len = ct_hex_len / 2;
    if (ct_len < HL_BOX_MACBYTES) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nonce_hex, nonce_hex_len, nonce, 24) != 0)
        return luaL_error(L, "invalid hex in nonce");
    if (hex_decode(pk_hex, pk_hex_len, pk, 32) != 0)
        return luaL_error(L, "invalid hex in public key");
    if (hex_decode(sk_hex, sk_hex_len, sk, 32) != 0)
        return luaL_error(L, "invalid hex in secret key");

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "runtime not available");
    }

    uint8_t *ct = sh_arena_alloc(lua->scratch, ct_len);
    if (!ct) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "out of memory");
    }
    if (hex_decode(ct_hex, ct_hex_len, ct, ct_len) != 0) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "invalid hex in ciphertext");
    }

    size_t msg_len = ct_len - HL_BOX_MACBYTES;
    uint8_t *msg = sh_arena_alloc(lua->scratch, msg_len + 1);
    if (!msg) {
        secure_zero(sk, sizeof(sk));
        return luaL_error(L, "out of memory");
    }

    if (hl_cap_crypto_box_open(msg, ct, ct_len, nonce, pk, sk) != 0) {
        secure_zero(sk, sizeof(sk));
        lua_pushnil(L);
        return 1;
    }
    secure_zero(sk, sizeof(sk));

    lua_pushlstring(L, (const char *)msg, msg_len);
    return 1;
}

/* crypto.box_keypair() → pk_hex, sk_hex */
static int lua_crypto_box_keypair(lua_State *L)
{
    uint8_t pk[32], sk[32];
    if (hl_cap_crypto_box_keypair(pk, sk) != 0)
        return luaL_error(L, "box keypair generation failed");

    char pk_hex[65], sk_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 32; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[64] = '\0';

    lua_pushstring(L, pk_hex);
    lua_pushstring(L, sk_hex);
    secure_zero(sk, sizeof(sk));
    secure_zero(sk_hex, sizeof(sk_hex));
    return 2;
}

/* crypto.hmac_sha256(data, key_hex) → hex string */
static int lua_crypto_hmac_sha256(lua_State *L)
{
    size_t data_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 2, &key_hex_len);

    if (key_hex_len % 2 != 0 || key_hex_len == 0 || key_hex_len > 256)
        return luaL_error(L, "key must be 1-128 bytes (2-256 hex chars)");

    size_t key_len = key_hex_len / 2;
    uint8_t key[128];
    if (hex_decode(key_hex, key_hex_len, key, key_len) != 0)
        return luaL_error(L, "invalid hex in key");

    uint8_t out[32];
    if (hl_cap_crypto_hmac_sha256(key, key_len,
                                  (const uint8_t *)data, data_len, out) != 0) {
        secure_zero(key, sizeof(key));
        return luaL_error(L, "hmac_sha256 failed");
    }

    secure_zero(key, sizeof(key));

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    hex[64] = '\0';

    lua_pushstring(L, hex);
    return 1;
}

/* crypto.hmac_sha256_verify(data, key_hex, expected_hex) → boolean */
static int lua_crypto_hmac_sha256_verify(lua_State *L)
{
    size_t data_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    size_t key_hex_len;
    const char *key_hex = luaL_checklstring(L, 2, &key_hex_len);
    size_t expected_hex_len;
    const char *expected_hex = luaL_checklstring(L, 3, &expected_hex_len);

    if (key_hex_len % 2 != 0 || key_hex_len == 0 || key_hex_len > 256)
        return luaL_error(L, "key must be 1-128 bytes (2-256 hex chars)");
    if (expected_hex_len != 64)
        return luaL_error(L, "expected mac must be 64 hex chars (32 bytes)");

    size_t key_len = key_hex_len / 2;
    uint8_t key[128];
    if (hex_decode(key_hex, key_hex_len, key, key_len) != 0)
        return luaL_error(L, "invalid hex in key");

    uint8_t expected[32];
    if (hex_decode(expected_hex, expected_hex_len, expected, 32) != 0) {
        secure_zero(key, sizeof(key));
        return luaL_error(L, "invalid hex in expected mac");
    }

    int rc = hl_cap_crypto_hmac_sha256_verify(key, key_len,
                                               (const uint8_t *)data, data_len,
                                               expected);
    secure_zero(key, sizeof(key));

    lua_pushboolean(L, rc == 0);
    return 1;
}

/* crypto.base64url_encode(data) → string (no padding) */
static int lua_crypto_base64url_encode(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);

    if (len > SIZE_MAX / 4)
        return luaL_error(L, "input too large for base64url");
    size_t out_size = ((len * 4) + 2) / 3 + 1;
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    char *out = sh_arena_alloc(lua->scratch, out_size);
    if (!out)
        return luaL_error(L, "out of memory");

    size_t out_len;
    if (hl_cap_crypto_base64url_encode(data, len, out, out_size, &out_len) != 0)
        return luaL_error(L, "base64url_encode failed");

    lua_pushlstring(L, out, out_len);
    return 1;
}

/* crypto.base64url_decode(str) → string or nil on error */
static int lua_crypto_base64url_decode(lua_State *L)
{
    size_t str_len;
    const char *str = luaL_checklstring(L, 1, &str_len);

    size_t out_size = (str_len * 3) / 4 + 1;
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return luaL_error(L, "runtime not available");

    uint8_t *out = sh_arena_alloc(lua->scratch, out_size);
    if (!out)
        return luaL_error(L, "out of memory");

    size_t out_len;
    if (hl_cap_crypto_base64url_decode(str, str_len, out, out_size, &out_len) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *)out, out_len);
    return 1;
}

static const luaL_Reg crypto_funcs[] = {
    {"sha256",            lua_crypto_sha256},
    {"sha512",            lua_crypto_sha512},
    {"random",            lua_crypto_random},
    {"hash_password",     lua_crypto_hash_password},
    {"verify_password",   lua_crypto_verify_password},
    {"ed25519_keypair",   lua_crypto_ed25519_keypair},
    {"ed25519_sign",      lua_crypto_ed25519_sign},
    {"ed25519_verify",    lua_crypto_ed25519_verify},
    {"auth",              lua_crypto_auth},
    {"auth_verify",       lua_crypto_auth_verify},
    {"secretbox",         lua_crypto_secretbox},
    {"secretbox_open",    lua_crypto_secretbox_open},
    {"box",               lua_crypto_box},
    {"box_open",          lua_crypto_box_open},
    {"box_keypair",       lua_crypto_box_keypair},
    {"hmac_sha256",       lua_crypto_hmac_sha256},
    {"hmac_sha256_verify", lua_crypto_hmac_sha256_verify},
    {"base64url_encode",  lua_crypto_base64url_encode},
    {"base64url_decode",  lua_crypto_base64url_decode},
    {NULL, NULL}
};

int luaopen_hull_crypto(lua_State *L)
{
    luaL_newlib(L, crypto_funcs);
    return 1;
}
