/* mod_crypto.c - hull.crypto module: hashing, encryption, signatures
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
#include "protected.h"   /* pushes that cannot leak or skip a scrub */

#include <sh_arena.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.crypto module
 *
 * Bytes in, bytes out: keys, nonces, signatures, tags, digests and
 * ciphertexts are raw byte strings, never hex. Text encodings are
 * hull.encoding's job (encoding.hex.encode(crypto.sha256(s))). The one
 * exception is hash_password's stored "pbkdf2:..." string, which is a
 * storage format rather than an encoding of a value.
 * ════════════════════════════════════════════════════════════════════ */

/* Charge a digest over @p n bytes to the instruction budget (Lua HULL PATCH
 * 0004, docs/lua_patches.md): one call hashes up to the whole heap, and it
 * counted as one instruction, so a loop of them was not bounded in time.
 * One unit per 8 bytes, a hash costing about an instruction per few bytes. */
static void crypto_charge(lua_State *L, size_t n)
{
    lua_hlcharge(L, n / 8, 0);
}

/* Charge a key derivation of @p rounds rounds at @p per_round units each,
 * BEFORE it runs (audit 9 H3). lua_hlwork runs the budget hook at once when
 * the charge made it due, so a run over its budget raises here instead of
 * first deriving: verify_password takes its iteration count from the stored
 * string (up to HL_PBKDF2_MAX_ITERATIONS, 10M), and one call held the event
 * loop for seconds while counting as a single instruction. */
static void crypto_charge_kdf(lua_State *L, uint64_t rounds, uint64_t per_round)
{
    uint64_t u = (per_round && rounds > UINT64_MAX / per_round)
                     ? UINT64_MAX : rounds * per_round;
    lua_hlwork(L, u > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)u, 0);
}

/* One PBKDF2-HMAC-SHA256 iteration per 32-byte output block is two SHA-256
 * compressions (inner + outer HMAC) of 64 bytes: crypto_charge's rate. */
#define HL_LUA_PBKDF2_UNITS_PER_ITER  ((2u * 64u) / 8u)

/* bcrypt_pbkdf runs one bcrypt_hash per round per 32-byte output block, and
 * a bcrypt_hash is ~130 Blowfish key expansions (~5M cycles): charged as
 * 2^17 instructions, well under its real cost. */
#define HL_LUA_BCRYPT_UNITS_PER_HASH  (1u << 17)

static void crypto_charge_bcrypt(lua_State *L, lua_Integer rounds,
                                 lua_Integer outlen)
{
    uint64_t blocks = ((uint64_t)outlen + 31u) / 32u;
    crypto_charge_kdf(L, (uint64_t)rounds * blocks, HL_LUA_BCRYPT_UNITS_PER_HASH);
}

/* Public-key operations (audit 10): an Ed25519 / X25519 / ECDSA scalar
 * multiplication is ~100k-300k cycles and an RSA private operation grows
 * with the cube of the modulus (an RSA-8192 sign takes most of a second),
 * yet each call counted as one instruction, so a loop of them was not
 * bounded in time. Each is charged a fixed cost BEFORE it runs (lua_hlwork,
 * as crypto_charge_kdf: a run over its budget raises with nothing done).
 *
 * One scalar multiplication (keypair, sign, verify, x25519, box's shared
 * key, an ECDSA sign / verify) is HL_LUA_ASYM_UNITS (2^14). RSA is
 * (bits / 1024)^3 * 2^14, never less than one scalar multiplication, with
 * bits capped at 8192 (mbedTLS's MPI ceiling). The key's size is not known
 * before mbedTLS parses it, so it comes from what the caller handed in:
 * for a verify, the signature (an RSA signature is exactly the modulus
 * long, and a shorter one is refused before any exponentiation); for a
 * sign, the PEM: a PEM private key carries at least n, d, p and q, three
 * modulus-lengths of DER and so four of base64, so its modulus has at most
 * 2 bits per PEM byte. The same numbers as the JS runtime. */
#define HL_LUA_ASYM_UNITS      ((size_t)1 << 14)
#define HL_LUA_RSA_MAX_BITS    8192u

static void crypto_charge_asym(lua_State *L)
{
    lua_hlwork(L, HL_LUA_ASYM_UNITS, 0);
}

/* (bits / 1024)^3 * 2^14 = bits^3 / 2^16, at least one scalar mult. */
static void crypto_charge_rsa_bits(lua_State *L, uint64_t bits)
{
    if (bits > HL_LUA_RSA_MAX_BITS) bits = HL_LUA_RSA_MAX_BITS;
    uint64_t u = (bits * bits * bits) >> 16;
    if (u < HL_LUA_ASYM_UNITS) u = HL_LUA_ASYM_UNITS;
    lua_hlwork(L, (size_t)u, 0);
}

static int crypto_alg_is_rsa(HlCryptoAsymAlg alg)
{
    return alg == HL_CRYPTO_ASYM_RS256 || alg == HL_CRYPTO_ASYM_RS384 ||
           alg == HL_CRYPTO_ASYM_RS512 || alg == HL_CRYPTO_ASYM_PS256;
}

static int lua_crypto_sha256(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    crypto_charge(L, len);
    uint8_t hash[32];
    if (hl_cap_crypto_sha256(data, len, hash) != 0)
        return luaL_error(L, "sha256 failed");
    lua_pushlstring(L, (const char *)hash, sizeof hash);
    return 1;
}

/* crypto.sha1(data) → 20-byte binary digest.
 *
 * LEGACY INTEROP ONLY. SHA-1 is collision-broken; this exists for
 * third-party protocols that hardcode it (HIBP range API, etc.).
 * DO NOT use for new password hashing / MAC / digest needs - use
 * crypto.sha256 / crypto.hmac_sha256 / crypto.hash_password instead.
 * Returns raw bytes (not hex) so callers can render uppercase or
 * lowercase as needed: `encoding.hex.encode(crypto.sha1(s)):upper()`. */
static int lua_crypto_sha1(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    crypto_charge(L, len);

    uint8_t hash[20];
    if (hl_cap_crypto_sha1(data, len, hash) != 0)
        return luaL_error(L, "sha1 failed");

    lua_pushlstring(L, (const char *)hash, 20);
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

/* crypto.random_token(n [, "hex"]) -> an unguessable token: n random bytes
 * as unpadded base64url (the default; safe in URLs, cookies and file names)
 * or lowercase hex. The one way the stdlib makes ids, nonces, CSRF secrets
 * and session tokens, so none of them hand-roll the encoding. n is 1-1024. */
static int lua_crypto_random_token(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    const char *fmt = luaL_optstring(L, 2, "base64url");
    int hex = strcmp(fmt, "hex") == 0;
    if (!hex && strcmp(fmt, "base64url") != 0)
        return luaL_error(L, "crypto.random_token: format must be \"base64url\" or \"hex\"");
    if (n <= 0 || n > HL_RANDOM_TOKEN_MAX)
        return luaL_error(L, "crypto.random_token: bytes must be 1-%d", HL_RANDOM_TOKEN_MAX);

    uint8_t raw[HL_RANDOM_TOKEN_MAX];
    char out[HL_RANDOM_TOKEN_MAX * 2 + 1];
    if (hl_cap_crypto_random(raw, (size_t)n) != 0)
        return luaL_error(L, "random failed");
    int len = hex
        ? (hl_hex_encode(raw, (size_t)n, out, sizeof out) == 0 ? (int)n * 2 : -1)
        : hl_base64_encode(raw, (size_t)n, out, sizeof out, HL_BASE64_URL | HL_BASE64_NOPAD);
    if (len < 0)
        return luaL_error(L, "crypto.random_token: encode failed");
    lua_pushlstring(L, out, (size_t)len);
    return 1;
}

/* crypto.hash_password(password) → "pbkdf2:iterations:salt_hex:hash_hex" */
static int lua_crypto_hash_password(lua_State *L)
{
    size_t pw_len;
    const char *pw = luaL_checklstring(L, 1, &pw_len);

    /* Charged before the derivation (audit 9 H3). */
    crypto_charge(L, pw_len);
    crypto_charge_kdf(L, HL_PBKDF2_ITERATIONS, HL_LUA_PBKDF2_UNITS_PER_ITER);

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
    hl_hex_encode(salt, sizeof salt, salt_hex, sizeof salt_hex);
    hl_hex_encode(hash, sizeof hash, hash_hex, sizeof hash_hex);

    char result[128];
    snprintf(result, sizeof(result), "pbkdf2:%d:%s:%s",
             iterations, salt_hex, hash_hex);

    secure_zero(hash, sizeof(hash));
    secure_zero(salt, sizeof(salt));

    lua_pushstring(L, result);
    return 1;
}

/* Local 0/-1 wrapper over utils/hex's hl_hex_decode, kept for the
 * many existing callsites in this file. The actual decode lives
 * in utils/hex.c so all of Hull shares one implementation. */
static int hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2) return -1;
    return hl_hex_decode(hex, hex_len, out, out_len) >= 0 ? 0 : -1;
}

/* crypto.verify_password(password, hash_string) → boolean */
static int lua_crypto_verify_password(lua_State *L)
{
    size_t pw_len;
    const char *pw = luaL_checklstring(L, 1, &pw_len);
    const char *stored = luaL_checkstring(L, 2);

    /* Parse "pbkdf2:iterations:salt_hex:hash_hex" manually (no scansets
     * - Cosmopolitan libc doesn't support sscanf %[...] scansets). */
    if (strncmp(stored, "pbkdf2:", 7) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const char *p = stored + 7;

    /* Parse iterations */
    char *end = NULL;
    long iterations = strtol(p, &end, 10);
    if (!end || *end != ':' || iterations < 100000 ||
        iterations > HL_PBKDF2_MAX_ITERATIONS) {
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

    uint8_t salt[16];
    if (hex_decode(salt_hex, 32, salt, sizeof(salt)) != 0) {
        lua_pushboolean(L, 0); return 1;
    }

    /* Charged before the derivation, for the count the STORED string names
     * (audit 9 H3): a tripped budget raises here, with nothing derived. */
    crypto_charge(L, pw_len);
    crypto_charge_kdf(L, (uint64_t)iterations, HL_LUA_PBKDF2_UNITS_PER_ITER);

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                                (int)iterations, computed, sizeof(computed)) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    uint8_t stored_hash[32];
    if (hex_decode(hash_hex, 64, stored_hash, sizeof(stored_hash)) != 0) {
        lua_pushboolean(L, 0); return 1;
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

/* ── Ed25519 bindings ──────────────────────────────────────────────── */

/* ── Pushing a result that sits next to a secret ─────────────────────
 *
 * A push ALLOCATES, and an allocation failure longjmps - which skips whatever
 * scrub was written on the line after it. That is not a theoretical OOM:
 * hl_lua_alloc returns NULL once an app reaches its mem_limit (64 MB by
 * default), so app code can reach this on purpose by filling the heap first.
 *
 * So every binding that has key material live at push time pushes through
 * here instead, and scrubs on BOTH outcomes. The two pushes that allocate
 * nothing - a light C function and a light userdata - are staged first, so
 * the only thing left inside the protected call is the part that can fail. */
typedef struct {
    const char *a; size_t an;
    const char *b; size_t bn;    /* b may be NULL: push one value */
} CryptoPushArgs;

static int crypto_push_values(lua_State *L)
{
    CryptoPushArgs *x = lua_touserdata(L, 1);
    lua_pushlstring(L, x->a, x->an);
    if (!x->b) return 1;
    lua_pushlstring(L, x->b, x->bn);
    return 2;
}

/* Push `pub` then `sec`, then scrub `sec`'s buffer whichever way it went.
 * Returns the number of results, or longjmps with the original error. */
static int crypto_push_keypair(lua_State *L, const char *pub, size_t publen,
                               char *sec, size_t seclen)
{
    CryptoPushArgs args = { pub, publen, sec, seclen };
    if (!lua_checkstack(L, 4)) {
        secure_zero(sec, seclen);
        return luaL_error(L, "crypto: stack");
    }
    lua_pushcfunction(L, crypto_push_values);
    lua_pushlightuserdata(L, &args);
    int st = lua_pcall(L, 1, 2, 0);
    secure_zero(sec, seclen);
    if (st != LUA_OK) return lua_error(L);
    return 2;
}

/* crypto.ed25519_keypair() -> public_key (32 bytes), secret_key (64 bytes) */
static int lua_crypto_ed25519_keypair(lua_State *L)
{
    uint8_t pk[32], sk[64];
    crypto_charge_asym(L);   /* audit 10 */
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0)
        return luaL_error(L, "ed25519 keypair generation failed");
    return crypto_push_keypair(L, (const char *)pk, sizeof pk,
                               (char *)sk, sizeof sk);
}

/* crypto.ed25519_sign(data, secret_key) -> 64-byte signature */
static int lua_crypto_ed25519_sign(lua_State *L)
{
    size_t data_len, sk_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    const char *sk   = luaL_checklstring(L, 2, &sk_len);
    crypto_charge(L, data_len);
    if (sk_len != 64)
        return luaL_error(L, "ed25519_sign: secret key must be 64 bytes");
    crypto_charge_asym(L);   /* audit 10 */
    uint8_t sig[64];
    if (hl_cap_crypto_ed25519_sign((const uint8_t *)data, data_len,
                                   (const uint8_t *)sk, sig) != 0)
        return luaL_error(L, "ed25519 sign failed");
    lua_pushlstring(L, (const char *)sig, sizeof sig);
    return 1;
}

/* crypto.ed25519_verify(data, signature, public_key) -> boolean */
static int lua_crypto_ed25519_verify(lua_State *L)
{
    size_t data_len, sig_len, pk_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    const char *sig  = luaL_checklstring(L, 2, &sig_len);
    const char *pk   = luaL_checklstring(L, 3, &pk_len);
    crypto_charge(L, data_len);
    if (sig_len != 64)
        return luaL_error(L, "ed25519_verify: signature must be 64 bytes");
    if (pk_len != 32)
        return luaL_error(L, "ed25519_verify: public key must be 32 bytes");
    crypto_charge_asym(L);   /* audit 10 */
    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)data, data_len,
                                          (const uint8_t *)sig, (const uint8_t *)pk);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* crypto.verify(alg, pubkey_pem, data, sig) -> boolean
 *
 *  alg        - "RS256" / "RS384" / "RS512" / "PS256" / "ES256" / "ES384"
 *               (case-insensitive). "HS256" / "none" / anything else
 *               raises (this surface is asymmetric-only by design;
 *               HMAC verification is crypto.hmac_sha256_verify).
 *  pubkey_pem - PEM-encoded SubjectPublicKeyInfo (a Lua string).
 *  data       - message bytes (Lua string).
 *  sig        - raw signature bytes (Lua string). For ECDSA, this is
 *               JOSE r||s (NOT DER) - matches the JWT wire format.
 *
 *  Returns true if the signature verifies, false otherwise. Raises
 *  on programming errors (unknown alg, NULL, etc.) so callers don't
 *  silently get a false-on-misuse oracle. Errors and false-returns
 *  are not distinguishable to the caller for the sig itself - the
 *  cap layer doesn't leak whether the failure was bad-sig vs bad-pem.
 */

static int lua_crypto_verify(lua_State *L)
{
    size_t alg_len, pk_len, data_len, sig_len;
    const char *alg_str = luaL_checklstring(L, 1, &alg_len);
    const char *pk      = luaL_checklstring(L, 2, &pk_len);
    const char *data    = luaL_checklstring(L, 3, &data_len);
    const char *sig     = luaL_checklstring(L, 4, &sig_len);
    crypto_charge(L, data_len);

    HlCryptoAsymAlg alg = hl_crypto_asym_alg_from_string(alg_str, alg_len);
    if (alg == HL_CRYPTO_ASYM_NONE)
        return luaL_error(L,
            "crypto.verify: unsupported alg '%.*s' (use one of "
            "RS256/RS384/RS512/PS256/ES256/ES384; HS256 is "
            "crypto.hmac_sha256_verify; 'none' is rejected)",
            (int)alg_len, alg_str);

    /* audit 10: an RSA signature is the modulus long (see the top). */
    if (crypto_alg_is_rsa(alg))
        crypto_charge_rsa_bits(L, (uint64_t)sig_len * 8u);
    else
        crypto_charge_asym(L);
    int rc = hl_cap_crypto_asym_verify_default(pk, pk_len, alg,
                                         data, data_len, sig, sig_len);
    /* rc == 0 -> verified. rc < 0 -> any failure (bad sig, bad PEM,
     * wrong key type, etc.). We collapse all failure modes to `false`
     * so the script can't observe which class of failure happened.
     * The cap layer's distinction is logged separately when audit
     * mode is on. */
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* crypto.sign(alg, private_pem, data) -> signature bytes
 *
 *  The inverse of crypto.verify, same algorithms, same encodings: RSA
 *  signatures are modulus-length, ECDSA ones raw r||s. private_pem is an
 *  unencrypted PEM private key (PKCS#1, SEC1 or PKCS#8) of the family and
 *  curve `alg` names. Raises on anything else - signing with the wrong key is
 *  a mistake to surface, not a false to test for. */
static int lua_crypto_sign(lua_State *L)
{
    size_t alg_len, pk_len, data_len;
    const char *alg_str = luaL_checklstring(L, 1, &alg_len);
    const char *pk      = luaL_checklstring(L, 2, &pk_len);
    const char *data    = luaL_checklstring(L, 3, &data_len);
    crypto_charge(L, data_len);

    HlCryptoAsymAlg alg = hl_crypto_asym_alg_from_string(alg_str, alg_len);
    if (alg == HL_CRYPTO_ASYM_NONE)
        return luaL_error(L,
            "crypto.sign: unsupported alg '%.*s' (use one of "
            "RS256/RS384/RS512/PS256/ES256/ES384)", (int)alg_len, alg_str);

    /* audit 10: at most 2 modulus bits per PEM byte (see the top). */
    if (crypto_alg_is_rsa(alg))
        crypto_charge_rsa_bits(L, (uint64_t)pk_len * 2u);
    else
        crypto_charge_asym(L);
    uint8_t sig[HL_CRYPTO_SIGN_MAX];
    size_t sig_len = 0;
    int rc = hl_cap_crypto_asym_sign_default(pk, pk_len, alg, data, data_len,
                                             sig, sizeof sig, &sig_len);
    if (rc == -2)
        return luaL_error(L, "crypto.sign: signing is not available in this build");
    if (rc != 0)
        return luaL_error(L, "crypto.sign: the key cannot sign under %s (malformed, "
                          "encrypted, a public key, or the wrong key type)",
                          hl_crypto_asym_alg_to_string(alg));
    lua_pushlstring(L, (const char *)sig, sig_len);
    return 1;
}

/* crypto.rsa_private_pem(n, e, d, p, q) -> PKCS#1 PEM
 *
 *  An RSA private key from its components (unsigned big-endian bytes), for
 *  formats that store a key that way - an OpenSSH key file does. The CRT
 *  values are derived and the key checked; inconsistent components raise.
 *  The result is secret: keep it no longer than the signing needs. */
static int lua_crypto_rsa_private_pem(lua_State *L)
{
    HlCryptoRsaParts parts;
    size_t n;
    parts.n = (const uint8_t *)luaL_checklstring(L, 1, &n); parts.n_len = n;
    parts.e = (const uint8_t *)luaL_checklstring(L, 2, &n); parts.e_len = n;
    parts.d = (const uint8_t *)luaL_checklstring(L, 3, &n); parts.d_len = n;
    parts.p = (const uint8_t *)luaL_checklstring(L, 4, &n); parts.p_len = n;
    parts.q = (const uint8_t *)luaL_checklstring(L, 5, &n); parts.q_len = n;

    /* audit 10: deriving the CRT values and checking the key is modular
     * arithmetic on n-sized numbers, charged as an RSA operation on n. */
    crypto_charge_rsa_bits(L, (uint64_t)parts.n_len * 8u);
    char pem[HL_CRYPTO_RSA_PEM_MAX];
    size_t pem_len = 0;
    int rc = hl_cap_crypto_rsa_private_pem(&parts, pem, sizeof pem, &pem_len);
    if (rc == -2)
        return luaL_error(L, "crypto.rsa_private_pem: not available in this build "
                          "(or an empty component)");
    if (rc != 0)
        return luaL_error(L, "crypto.rsa_private_pem: the components do not form "
                          "a valid RSA key");
    /* Protected, and scrubbed either way: a push that raised left the
     * private key in this stack buffer. */
    int pushed = hl_lua_pushlstring_safe(L, pem, pem_len);
    secure_zero(pem, pem_len);
    if (pushed != 0) return luaL_error(L, "not enough memory for the key");
    return 1;
}

/* crypto.x509_pubkey_pem(der) -> pem_string or nil, err
 *
 *  Bridge from a base64-decoded X.509 certificate (DER) to the PEM-
 *  encoded SubjectPublicKeyInfo that crypto.verify consumes. Lets
 *  OIDC apps consume JWKS `x5c` entries directly.
 */
static int lua_crypto_x509_pubkey_pem(lua_State *L)
{
    size_t der_len;
    const char *der = luaL_checklstring(L, 1, &der_len);

    /* mbedtls_pk_write_pubkey_pem needs ~800 bytes for RSA-2048 SPKI
     * and ~2 KiB for RSA-4096; 4 KiB is generous and stack-safe. */
    char pem[4096];
    size_t pem_len = 0;
    if (hl_cap_crypto_x509_pubkey_pem(der, der_len,
                                       pem, sizeof(pem), &pem_len) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "x509_pubkey_pem: parse or write failed");
        return 2;
    }
    lua_pushlstring(L, pem, pem_len);
    return 1;
}

/* ── SHA-512 ───────────────────────────────────────────────────────── */

/* crypto.sha512(data) -> 64-byte digest */
static int lua_crypto_sha512(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    crypto_charge(L, len);
    uint8_t hash[64];
    if (hl_cap_crypto_sha512(data, len, hash) != 0)
        return luaL_error(L, "sha512 failed");
    lua_pushlstring(L, (const char *)hash, sizeof hash);
    return 1;
}

/* ── HMAC-SHA512/256 authentication ────────────────────────────────── */

/* crypto.auth(msg, key) -> 32-byte tag (HMAC-SHA512/256; key 32 bytes) */
static int lua_crypto_auth(lua_State *L)
{
    size_t msg_len, key_len;
    const char *msg = luaL_checklstring(L, 1, &msg_len);
    const char *key = luaL_checklstring(L, 2, &key_len);
    crypto_charge(L, msg_len);
    if (key_len != 32)
        return luaL_error(L, "auth: key must be 32 bytes");
    uint8_t tag[32];
    if (hl_cap_crypto_auth(msg, msg_len, (const uint8_t *)key, tag) != 0)
        return luaL_error(L, "auth failed");
    lua_pushlstring(L, (const char *)tag, sizeof tag);
    return 1;
}

/* crypto.auth_verify(tag, msg, key) -> boolean */
static int lua_crypto_auth_verify(lua_State *L)
{
    size_t tag_len, msg_len, key_len;
    const char *tag = luaL_checklstring(L, 1, &tag_len);
    const char *msg = luaL_checklstring(L, 2, &msg_len);
    const char *key = luaL_checklstring(L, 3, &key_len);
    crypto_charge(L, msg_len);
    if (tag_len != 32)
        return luaL_error(L, "auth_verify: tag must be 32 bytes");
    if (key_len != 32)
        return luaL_error(L, "auth_verify: key must be 32 bytes");
    int rc = hl_cap_crypto_auth_verify((const uint8_t *)tag, msg, msg_len,
                                       (const uint8_t *)key);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── Secret-key authenticated encryption (XSalsa20+Poly1305) ──────── */

/* crypto.secretbox(msg, nonce, key) -> ciphertext (msg + 16-byte tag);
 * nonce 24 bytes, key 32 bytes */
static int lua_crypto_secretbox(lua_State *L)
{
    size_t msg_len, nonce_len, key_len;
    const char *msg   = luaL_checklstring(L, 1, &msg_len);
    const char *nonce = luaL_checklstring(L, 2, &nonce_len);
    const char *key   = luaL_checklstring(L, 3, &key_len);
    if (nonce_len != 24)
        return luaL_error(L, "secretbox: nonce must be 24 bytes");
    if (key_len != 32)
        return luaL_error(L, "secretbox: key must be 32 bytes");
    if (msg_len > SIZE_MAX - HL_SECRETBOX_MACBYTES)
        return luaL_error(L, "secretbox: message too large");
    crypto_charge(L, msg_len);   /* audit 9 H3 */
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;
    luaL_Buffer b;
    uint8_t *ct = (uint8_t *)luaL_buffinitsize(L, &b, ct_len);
    if (hl_cap_crypto_secretbox(ct, msg, msg_len, (const uint8_t *)nonce,
                                (const uint8_t *)key) != 0)
        return luaL_error(L, "secretbox failed");
    luaL_pushresultsize(&b, ct_len);
    return 1;
}

/* crypto.secretbox_open(ciphertext, nonce, key) -> msg | nil */
static int lua_crypto_secretbox_open(lua_State *L)
{
    size_t ct_len, nonce_len, key_len;
    const char *ct    = luaL_checklstring(L, 1, &ct_len);
    const char *nonce = luaL_checklstring(L, 2, &nonce_len);
    const char *key   = luaL_checklstring(L, 3, &key_len);
    if (nonce_len != 24)
        return luaL_error(L, "secretbox_open: nonce must be 24 bytes");
    if (key_len != 32)
        return luaL_error(L, "secretbox_open: key must be 32 bytes");
    crypto_charge(L, ct_len);   /* audit 9 H3 */
    if (ct_len < HL_SECRETBOX_MACBYTES) {
        lua_pushnil(L);
        return 1;
    }
    size_t msg_len = ct_len - HL_SECRETBOX_MACBYTES;
    luaL_Buffer b;
    uint8_t *msg = (uint8_t *)luaL_buffinitsize(L, &b, msg_len + 1);
    if (hl_cap_crypto_secretbox_open(msg, (const uint8_t *)ct, ct_len,
                                     (const uint8_t *)nonce,
                                     (const uint8_t *)key) != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, msg_len);
    return 1;
}

/* ── Public-key authenticated encryption (Curve25519+XSalsa20+Poly1305) */

/* crypto.box(msg, nonce, public_key, secret_key) -> ciphertext;
 * nonce 24 bytes, keys 32 bytes */
static int lua_crypto_box(lua_State *L)
{
    size_t msg_len, nonce_len, pk_len, sk_len;
    const char *msg   = luaL_checklstring(L, 1, &msg_len);
    const char *nonce = luaL_checklstring(L, 2, &nonce_len);
    const char *pk    = luaL_checklstring(L, 3, &pk_len);
    const char *sk    = luaL_checklstring(L, 4, &sk_len);
    if (nonce_len != 24)
        return luaL_error(L, "box: nonce must be 24 bytes");
    if (pk_len != 32)
        return luaL_error(L, "box: public key must be 32 bytes");
    if (sk_len != 32)
        return luaL_error(L, "box: secret key must be 32 bytes");
    if (msg_len > SIZE_MAX - HL_BOX_MACBYTES)
        return luaL_error(L, "box: message too large");
    crypto_charge(L, msg_len);   /* audit 9 H3 */
    crypto_charge_asym(L);       /* the shared key (audit 10) */
    size_t ct_len = msg_len + HL_BOX_MACBYTES;
    luaL_Buffer b;
    uint8_t *ct = (uint8_t *)luaL_buffinitsize(L, &b, ct_len);
    if (hl_cap_crypto_box(ct, msg, msg_len, (const uint8_t *)nonce,
                          (const uint8_t *)pk, (const uint8_t *)sk) != 0)
        return luaL_error(L, "box failed");
    luaL_pushresultsize(&b, ct_len);
    return 1;
}

/* crypto.box_open(ciphertext, nonce, public_key, secret_key) -> msg | nil */
static int lua_crypto_box_open(lua_State *L)
{
    size_t ct_len, nonce_len, pk_len, sk_len;
    const char *ct    = luaL_checklstring(L, 1, &ct_len);
    const char *nonce = luaL_checklstring(L, 2, &nonce_len);
    const char *pk    = luaL_checklstring(L, 3, &pk_len);
    const char *sk    = luaL_checklstring(L, 4, &sk_len);
    if (nonce_len != 24)
        return luaL_error(L, "box_open: nonce must be 24 bytes");
    if (pk_len != 32)
        return luaL_error(L, "box_open: public key must be 32 bytes");
    if (sk_len != 32)
        return luaL_error(L, "box_open: secret key must be 32 bytes");
    crypto_charge(L, ct_len);   /* audit 9 H3 */
    crypto_charge_asym(L);      /* the shared key (audit 10) */
    if (ct_len < HL_BOX_MACBYTES) {
        lua_pushnil(L);
        return 1;
    }
    size_t msg_len = ct_len - HL_BOX_MACBYTES;
    luaL_Buffer b;
    uint8_t *msg = (uint8_t *)luaL_buffinitsize(L, &b, msg_len + 1);
    if (hl_cap_crypto_box_open(msg, (const uint8_t *)ct, ct_len,
                               (const uint8_t *)nonce, (const uint8_t *)pk,
                               (const uint8_t *)sk) != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, msg_len);
    return 1;
}

/* crypto.x25519_keypair() -> public_key, secret_key (32 bytes each) */
static int lua_crypto_x25519_keypair(lua_State *L)
{
    uint8_t pk[32], sk[32];
    crypto_charge_asym(L);   /* audit 10 */
    if (hl_cap_crypto_x25519_keypair(pk, sk) != 0)
        return luaL_error(L, "x25519 keypair generation failed");
    return crypto_push_keypair(L, (const char *)pk, sizeof pk,
                               (char *)sk, sizeof sk);
}

/* crypto.x25519(secret_key, public_key) -> 32-byte shared secret | nil, err
 *
 * A low-order peer point is a protocol-level event a caller has to handle, not
 * a programming error, so it comes back as (nil, reason) rather than raising.
 */
static int lua_crypto_x25519(lua_State *L)
{
    size_t sk_len, pk_len;
    const char *sk = luaL_checklstring(L, 1, &sk_len);
    const char *pk = luaL_checklstring(L, 2, &pk_len);
    if (sk_len != 32)
        return luaL_error(L, "x25519: secret key must be 32 bytes");
    if (pk_len != 32)
        return luaL_error(L, "x25519: public key must be 32 bytes");
    crypto_charge_asym(L);   /* audit 10 */
    uint8_t shared[32];
    int rc = hl_cap_crypto_x25519(shared, (const uint8_t *)sk, (const uint8_t *)pk);
    if (rc == -2) {
        lua_pushnil(L);
        lua_pushstring(L, "peer sent a low-order point");
        return 2;
    }
    if (rc != 0)
        return luaL_error(L, "x25519 failed");
    /* Protected, and scrubbed either way (see rsa_private_pem). */
    int pushed = hl_lua_pushlstring_safe(L, (const char *)shared, sizeof shared);
    secure_zero(shared, sizeof shared);
    if (pushed != 0) return luaL_error(L, "not enough memory for the secret");
    return 1;
}

/* crypto.box_keypair() -> public_key, secret_key (32 bytes each) */
static int lua_crypto_box_keypair(lua_State *L)
{
    uint8_t pk[32], sk[32];
    crypto_charge_asym(L);   /* audit 10 */
    if (hl_cap_crypto_box_keypair(pk, sk) != 0)
        return luaL_error(L, "box keypair generation failed");
    return crypto_push_keypair(L, (const char *)pk, sizeof pk,
                               (char *)sk, sizeof sk);
}

/* crypto.hmac_sha256(data, key) -> 32-byte MAC; the key is any non-empty
 * byte string */
static int lua_crypto_hmac_sha256(lua_State *L)
{
    size_t data_len, key_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    const char *key  = luaL_checklstring(L, 2, &key_len);
    crypto_charge(L, data_len);
    if (key_len == 0)
        return luaL_error(L, "hmac_sha256: key must not be empty");
    uint8_t out[32];
    if (hl_cap_crypto_hmac_sha256((const uint8_t *)key, key_len,
                                  (const uint8_t *)data, data_len, out) != 0)
        return luaL_error(L, "hmac_sha256 failed");
    lua_pushlstring(L, (const char *)out, sizeof out);
    return 1;
}

/* crypto.hmac_sha1(data, key) -> 20-byte MAC.
 *
 * HOTP/TOTP compatibility only - see hl_cap_crypto_hmac_sha1 docstring. */
static int lua_crypto_hmac_sha1(lua_State *L)
{
    size_t data_len, key_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    const char *key  = luaL_checklstring(L, 2, &key_len);
    crypto_charge(L, data_len);
    if (key_len == 0)
        return luaL_error(L, "hmac_sha1: key must not be empty");
    uint8_t out[20];
    if (hl_cap_crypto_hmac_sha1((const uint8_t *)key, key_len,
                                (const uint8_t *)data, data_len, out) != 0)
        return luaL_error(L, "hmac_sha1 failed");
    lua_pushlstring(L, (const char *)out, sizeof out);
    return 1;
}

/* crypto.hmac_sha256_verify(data, key, expected) -> boolean; `expected` is
 * the 32-byte MAC, compared in constant time */
static int lua_crypto_hmac_sha256_verify(lua_State *L)
{
    size_t data_len, key_len, expected_len;
    const char *data     = luaL_checklstring(L, 1, &data_len);
    const char *key      = luaL_checklstring(L, 2, &key_len);
    const char *expected = luaL_checklstring(L, 3, &expected_len);
    crypto_charge(L, data_len);
    if (key_len == 0)
        return luaL_error(L, "hmac_sha256_verify: key must not be empty");
    if (expected_len != 32) {
        lua_pushboolean(L, 0);           /* not a MAC of ours: no match */
        return 1;
    }
    int rc = hl_cap_crypto_hmac_sha256_verify((const uint8_t *)key, key_len,
                                               (const uint8_t *)data, data_len,
                                               (const uint8_t *)expected);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* crypto.constant_time_eq(a, b) → boolean
 *
 * Constant-time equality of two byte strings, compared in C so the timing is
 * not subject to interpreter/bytecode-dispatch variance. Length is NOT secret
 * (callers compare fixed-size digests/MACs), so a length mismatch returns
 * false immediately; equal-length inputs are compared with no early exit. Used
 * by jwt/csrf to compare HMAC signatures. */
static int lua_crypto_constant_time_eq(lua_State *L)
{
    size_t alen, blen;
    const char *a = luaL_checklstring(L, 1, &alen);
    const char *b = luaL_checklstring(L, 2, &blen);
    if (alen != blen) { lua_pushboolean(L, 0); return 1; }
    crypto_charge(L, alen);   /* audit 9 H3 */
    unsigned diff = 0;
    for (size_t i = 0; i < alen; i++)
        diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    lua_pushboolean(L, diff == 0);
    return 1;
}

/* ── Incremental SHA-256 hasher ─────────────────────────────────────
 *
 *   local h = crypto.create_sha256()
 *   h:update(chunk)         -- repeat as needed
 *   local digest = h:digest()  -- 32 bytes; further update/digest = error
 *
 * Backed by HlSha256Ctx; the userdata holds the context + a 'done' flag
 * so we can reject double-digest + post-digest updates cleanly. */

#define HL_SHA256_HASHER_MT "HlSha256Hasher"

typedef struct {
    HlSha256Ctx ctx;
    int         done;
} HlLuaSha256Hasher;

static HlLuaSha256Hasher *check_hasher(lua_State *L, int idx)
{
    return (HlLuaSha256Hasher *)luaL_checkudata(L, idx, HL_SHA256_HASHER_MT);
}

static int lua_crypto_create_sha256(lua_State *L)
{
    HlLuaSha256Hasher *h = (HlLuaSha256Hasher *)
        lua_newuserdatauv(L, sizeof(*h), 0);
    hl_cap_crypto_sha256_init(&h->ctx);
    h->done = 0;
    luaL_setmetatable(L, HL_SHA256_HASHER_MT);
    return 1;
}

static int lua_sha256_hasher_update(lua_State *L)
{
    HlLuaSha256Hasher *h = check_hasher(L, 1);
    if (h->done)
        return luaL_error(L, "sha256:update() after digest()");
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    crypto_charge(L, len);
    if (hl_cap_crypto_sha256_update(&h->ctx, data, len) != 0)
        return luaL_error(L, "sha256:update() failed");
    /* Return the hasher so calls can chain. */
    lua_settop(L, 1);
    return 1;
}

static int lua_sha256_hasher_digest(lua_State *L)
{
    HlLuaSha256Hasher *h = check_hasher(L, 1);
    if (h->done)
        return luaL_error(L, "sha256:digest() already called");
    uint8_t out[32];
    if (hl_cap_crypto_sha256_final(&h->ctx, out) != 0)
        return luaL_error(L, "sha256:digest() failed");
    h->done = 1;
    lua_pushlstring(L, (const char *)out, sizeof out);
    return 1;
}

static int lua_sha256_hasher_gc(lua_State *L)
{
    HlLuaSha256Hasher *h = check_hasher(L, 1);
    /* If digest() wasn't called, scrub the in-flight state - it may
     * contain partial input bytes. _final zeros the ctx on success;
     * do the same here for the abandoned-without-final path. */
    if (!h->done) secure_zero(&h->ctx, sizeof(h->ctx));
    h->done = 1;   /* nothing runs on it after this */
    return 0;
}

static const luaL_Reg sha256_hasher_methods[] = {
    {"update", lua_sha256_hasher_update},
    {"digest", lua_sha256_hasher_digest},
    {NULL, NULL}
};

static void register_sha256_hasher_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_SHA256_HASHER_MT);
    luaL_newlib(L, sha256_hasher_methods);   /* methods apart from the mt */
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_sha256_hasher_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "locked");            /* app code gets no handle on it */
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

/* ── Keys held in C ─────────────────────────────────────────────────── */

/* crypto.key_from_env(var) -> key
 *
 * A 32-byte secretbox key read from the environment variable `var` (64 hex
 * digits or base64, under manifest.env like any env read) into memory the C
 * layer owns. The script gets a handle, never the bytes: key:secretbox /
 * key:secretbox_open seal with it, key:destroy() or the collector zeroes it.
 * hull/crypto/sealbox keyrings take a key in place of a byte string, which is
 * how hull/kv's `encrypt` and TOTP's secret store keep their keys out of the
 * script heap. */
#define HL_CRYPTO_KEY_MT "hull.crypto.key"

typedef struct {
    HlCryptoKey *k;
    char name[64];              /* the variable's name, for tostring; not secret */
} HlLuaCryptoKey;

static HlLuaCryptoKey *check_key(lua_State *L, int idx)
{
    return (HlLuaCryptoKey *)luaL_checkudata(L, idx, HL_CRYPTO_KEY_MT);
}

static const HlCryptoKey *live_key(lua_State *L, int idx)
{
    HlLuaCryptoKey *h = check_key(L, idx);
    if (!h->k) luaL_error(L, "crypto.key: the key has been destroyed");
    return h->k;
}

static int lua_crypto_key_from_env(lua_State *L)
{
    const char *var = luaL_checkstring(L, 1);
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.env_cfg)
        return luaL_error(L, "crypto.key_from_env: no env capability");

    /* The handle is allocated first, so nothing that can raise runs while a
     * key exists outside it. */
    HlLuaCryptoKey *h = (HlLuaCryptoKey *)lua_newuserdatauv(L, sizeof *h, 0);
    h->k = NULL;
    snprintf(h->name, sizeof h->name, "%s", var);
    luaL_setmetatable(L, HL_CRYPTO_KEY_MT);

    const char *val = hl_cap_env_get(lua->base.env_cfg, var);
    if (!val || !*val) {
        /* One message for "not in manifest.env" and "declared but unset", as
         * bcrypt_pbkdf_env: telling them apart would let a caller probe the
         * allowlist, and the remedy is the same. */
        return luaL_error(L, "crypto.key_from_env: '%s' is not available "
                             "(declare it in manifest.env and set it)", var);
    }
    /* Decoded straight from the environment's own copy into the key buffer;
     * no intermediate script-visible or unscrubbed copy is made. */
    if (hl_cap_crypto_key_from_text(val, strlen(val), &h->k) != 0)
        return luaL_error(L, "crypto.key_from_env: '%s' is not a 32-byte key "
                             "(64 hex digits or base64)", var);
    return 1;
}

static int lua_crypto_key_secretbox(lua_State *L)
{
    (void)live_key(L, 1);   /* type check now; the pointer is taken below */
    size_t msg_len, nonce_len;
    const char *msg   = luaL_checklstring(L, 2, &msg_len);
    const char *nonce = luaL_checklstring(L, 3, &nonce_len);
    if (nonce_len != 24) return luaL_error(L, "key:secretbox: nonce must be 24 bytes");
    if (msg_len > SIZE_MAX - HL_SECRETBOX_MACBYTES)
        return luaL_error(L, "key:secretbox: message too large");
    crypto_charge(L, msg_len);   /* audit 9 H3 */
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;
    luaL_Buffer b;
    uint8_t *ct = (uint8_t *)luaL_buffinitsize(L, &b, ct_len);
    /* The key is resolved AFTER the allocations: a large buffer is a box
     * userdata, and making it can run a GC step - an app finalizer that
     * calls key:destroy() then freed the key a pointer taken earlier read. */
    const HlCryptoKey *k = live_key(L, 1);
    if (hl_cap_crypto_key_secretbox(k, ct, msg, msg_len, (const uint8_t *)nonce) != 0)
        return luaL_error(L, "key:secretbox failed");
    luaL_pushresultsize(&b, ct_len);
    return 1;
}

/* key:secretbox_open(ciphertext, nonce) -> msg | nil */
static int lua_crypto_key_secretbox_open(lua_State *L)
{
    (void)live_key(L, 1);   /* type check now; the pointer is taken below */
    size_t ct_len, nonce_len;
    const char *ct    = luaL_checklstring(L, 2, &ct_len);
    const char *nonce = luaL_checklstring(L, 3, &nonce_len);
    if (nonce_len != 24) return luaL_error(L, "key:secretbox_open: nonce must be 24 bytes");
    crypto_charge(L, ct_len);   /* audit 9 H3 */
    if (ct_len < HL_SECRETBOX_MACBYTES) { lua_pushnil(L); return 1; }
    size_t msg_len = ct_len - HL_SECRETBOX_MACBYTES;
    luaL_Buffer b;
    uint8_t *msg = (uint8_t *)luaL_buffinitsize(L, &b, msg_len + 1);
    const HlCryptoKey *k = live_key(L, 1);   /* after the allocation: see above */
    if (hl_cap_crypto_key_secretbox_open(k, msg, ct, ct_len, (const uint8_t *)nonce) != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, msg_len);
    return 1;
}

/* Zero it now rather than whenever the collector runs. Idempotent. */
static int lua_crypto_key_destroy(lua_State *L)
{
    HlLuaCryptoKey *h = check_key(L, 1);
    hl_cap_crypto_key_free(h->k);
    h->k = NULL;
    return 0;
}

static int lua_crypto_key_tostring(lua_State *L)
{
    HlLuaCryptoKey *h = check_key(L, 1);
    lua_pushfstring(L, "crypto.key(%s%s)", h->name, h->k ? "" : ", destroyed");
    return 1;
}

static const luaL_Reg crypto_key_methods[] = {
    {"secretbox",      lua_crypto_key_secretbox},
    {"secretbox_open", lua_crypto_key_secretbox_open},
    {"destroy",        lua_crypto_key_destroy},
    {NULL, NULL}
};

static void register_crypto_key_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_CRYPTO_KEY_MT);
    lua_newtable(L);
    luaL_setfuncs(L, crypto_key_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_crypto_key_destroy);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, lua_crypto_key_tostring);
    lua_setfield(L, -2, "__tostring");
    /* No __metatable access to the methods table beyond these three, and no
     * field holds the bytes, so a script cannot read a key back. */
    lua_pushliteral(L, "crypto.key");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

/* ── OpenSSH key passphrases ────────────────────────────────────────── */

/* Validate the two numeric arguments both entry points share. Returns 0 when
 * they are usable, or pushes nothing and returns -1 with `why` set.
 *
 * Separate from the derivation so the env path can REJECT before it copies a
 * passphrase - there is no reason to have the secret in hand while arguing
 * about a round count. */
static int crypto_bcrypt_args_ok(lua_Integer rounds, lua_Integer outlen,
                                 const char **why)
{
    if (rounds <= 0 || rounds > (lua_Integer)HL_BCRYPT_MAX_ROUNDS) {
        /* Bounded, not just positive: the count is read from the key file,
         * and this runs uninterruptibly on the event-loop thread. */
        *why = "rounds is out of range";
        return -1;
    }
    if (outlen <= 0 || outlen > HL_BCRYPT_MAX_OUT) {
        *why = "length is out of range";
        return -1;
    }
    return 0;
}

/* crypto.bcrypt_pbkdf(pass, salt, rounds, len) -> derived
 *
 * The generic primitive: pinned by a known-answer test, and usable by any
 * caller that already holds the bytes.
 *
 * NOTE the passphrase is a Lua string here, and Lua strings are immutable and
 * interned - Hull cannot scrub it, and it lives in the script heap until GC.
 * Where that matters, use bcrypt_pbkdf_env below, which never lets the
 * passphrase become a Lua value at all. */
static int lua_crypto_bcrypt_pbkdf(lua_State *L)
{
    size_t pass_len, salt_len;
    const char *pass = luaL_checklstring(L, 1, &pass_len);
    const char *salt = luaL_checklstring(L, 2, &salt_len);
    lua_Integer rounds = luaL_checkinteger(L, 3);
    lua_Integer outlen = luaL_checkinteger(L, 4);

    const char *why = NULL;
    if (crypto_bcrypt_args_ok(rounds, outlen, &why) != 0)
        return luaL_error(L, "crypto.bcrypt_pbkdf: %s", why);
    if (!pass_len) return luaL_error(L, "crypto.bcrypt_pbkdf: empty passphrase");
    if (!salt_len) return luaL_error(L, "crypto.bcrypt_pbkdf: empty salt");

    /* Charged before the derivation (audit 9 H3). */
    crypto_charge(L, pass_len + salt_len);
    crypto_charge_bcrypt(L, rounds, outlen);

    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, (size_t)outlen);
    if (hl_cap_crypto_bcrypt_pbkdf(pass, pass_len, salt, salt_len,
                                   (unsigned int)rounds,
                                   (uint8_t *)out, (size_t)outlen) != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        /* Never echo the passphrase, its length, or any derived byte. */
        return luaL_error(L, "crypto.bcrypt_pbkdf: derivation failed");
    }
    luaL_pushresultsize(&b, (size_t)outlen);
    return 1;
}

/* crypto.bcrypt_pbkdf_env(var, salt, rounds, len) -> derived
 *
 * The same derivation, but Lua passes only the NAME of an environment
 * variable. The C layer reads the value, derives from it, and scrubs its copy;
 * the passphrase never becomes a Lua value, so it never lands anywhere Hull
 * cannot reach. Mirrors how databases.named already takes "$VAR" references.
 *
 * Gated by manifest.env like any other environment read - naming a variable
 * here grants nothing env.get would not already grant.
 *
 * Honest about its limit: the value still sits in the process environment for
 * the process's lifetime, the standard trade-off for an env-carried secret.
 * What this buys is that it is not ALSO in the script heap, where it would be
 * unscrubbable, GC-visible and reachable by any app code.
 *
 * Structured so NO error path unwinds over a live secret: everything that can
 * be rejected is rejected before the copy is taken, and the copy is wiped and
 * freed before any luaL_error can longjmp out of here.
 *
 * That covered the PASSPHRASE and missed the DERIVED KEY, which is
 * passphrase-equivalent for the file it opens. The last statement used to be
 * a bare lua_pushlstring, and a push allocates: on LUA_ERRMEM it longjmps,
 * and the wipe-and-free two lines below never ran. Not a theoretical OOM
 * either - hl_lua_alloc returns NULL once an app reaches its 64 MB mem_limit,
 * so app code could reach it on purpose. So the push now happens under
 * lua_pcall and the scrub runs on both outcomes. */

/* Pushes a byte range. Runs under lua_pcall so the caller's scrub cannot be
 * skipped by an allocation failure inside the push. */
typedef struct { const char *p; size_t n; } CryptoPushArg;

static int crypto_push_bytes(lua_State *L)
{
    CryptoPushArg *a = lua_touserdata(L, 1);
    lua_pushlstring(L, a->p, a->n);
    return 1;
}

static int lua_crypto_bcrypt_pbkdf_env(lua_State *L)
{
    const char *var = luaL_checkstring(L, 1);
    size_t salt_len;
    const char *salt = luaL_checklstring(L, 2, &salt_len);
    lua_Integer rounds = luaL_checkinteger(L, 3);
    lua_Integer outlen = luaL_checkinteger(L, 4);

    const char *why = NULL;
    if (crypto_bcrypt_args_ok(rounds, outlen, &why) != 0)
        return luaL_error(L, "crypto.bcrypt_pbkdf_env: %s", why);
    if (!salt_len) return luaL_error(L, "crypto.bcrypt_pbkdf_env: empty salt");

    /* Charged before the passphrase is read (audit 9 H3). */
    crypto_charge(L, salt_len);
    crypto_charge_bcrypt(L, rounds, outlen);

    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.env_cfg)
        return luaL_error(L, "crypto.bcrypt_pbkdf_env: no env capability");

    const char *val = hl_cap_env_get(lua->base.env_cfg, var);
    if (!val || !*val) {
        /* One message for "not in manifest.env" and "declared but unset".
         * Distinguishing them would let a caller probe the allowlist, and the
         * remedy is the same either way. */
        return luaL_error(L, "crypto.bcrypt_pbkdf_env: '%s' is not available "
                             "(declare it in manifest.env and set it)", var);
    }

    /* Copy so the derivation reads memory WE can wipe. The environment's own
     * copy belongs to the OS and is left alone: zeroing it would break any
     * later read and is not this function's decision. */
    size_t n = strlen(val);
    char *copy = malloc(n);
    uint8_t *derived = malloc((size_t)outlen);
    if (!copy || !derived) {
        free(copy); free(derived);
        return luaL_error(L, "crypto.bcrypt_pbkdf_env: out of memory");
    }
    memcpy(copy, val, n);

    /* Everything the push needs is staged BEFORE the derivation, while there
     * is still nothing to scrub: lua_checkstack can throw, and a light C
     * function and a light userdata are the two pushes that allocate nothing
     * and so cannot. After this point no Lua API call happens until the
     * protected one. */
    CryptoPushArg arg = { (const char *)derived, (size_t)outlen };
    if (!lua_checkstack(L, 3)) {
        secure_zero(copy, n); free(copy);
        secure_zero(derived, (size_t)outlen); free(derived);
        return luaL_error(L, "crypto.bcrypt_pbkdf_env: stack");
    }
    lua_pushcfunction(L, crypto_push_bytes);
    lua_pushlightuserdata(L, &arg);

    int rc = hl_cap_crypto_bcrypt_pbkdf(copy, n, salt, salt_len,
                                        (unsigned int)rounds, derived,
                                        (size_t)outlen);
    secure_zero(copy, n);
    free(copy);

    if (rc != 0) {
        lua_pop(L, 2);                    /* the staged function + argument */
        secure_zero(derived, (size_t)outlen);
        free(derived);
        return luaL_error(L, "crypto.bcrypt_pbkdf_env: derivation failed");
    }

    int st = lua_pcall(L, 1, 1, 0);
    /* Unconditional, and before the error is re-raised: this is the whole
     * point of the protected call. */
    secure_zero(derived, (size_t)outlen);
    free(derived);
    if (st != LUA_OK) return lua_error(L);   /* re-raise, message intact */
    return 1;
}

/* crypto.aes256ctr(key, iv, data) -> out
 *
 * One function for both directions, because CTR has only one: it XORs a
 * keystream. Unauthenticated by nature, so the caller supplies its own
 * integrity check - an OpenSSH private section carries check1/check2 inside
 * the plaintext for exactly this reason. */
static int lua_crypto_aes256ctr(lua_State *L)
{
    size_t klen, ivlen, len;
    const char *key = luaL_checklstring(L, 1, &klen);
    const char *iv  = luaL_checklstring(L, 2, &ivlen);
    const char *in  = luaL_optlstring(L, 3, "", &len);

    if (klen != HL_AEAD_KEY_LEN)
        return luaL_error(L, "crypto.aes256ctr: key must be %d bytes, got %d",
                          (int)HL_AEAD_KEY_LEN, (int)klen);
    if (ivlen != HL_AES_CTR_IV_LEN)
        return luaL_error(L, "crypto.aes256ctr: iv must be %d bytes, got %d",
                          (int)HL_AES_CTR_IV_LEN, (int)ivlen);
    if (!len) { lua_pushliteral(L, ""); return 1; }
    crypto_charge(L, len);   /* audit 9 H3 */

    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, len);
    int rc = hl_cap_crypto_aes256ctr((uint8_t *)out, (const uint8_t *)key,
                                     (const uint8_t *)iv, in, len);
    if (rc != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        return luaL_error(L, rc == -3
            ? "crypto.aes256ctr: no AES backend in this build (TLS is not composed)"
            : "crypto.aes256ctr: bad argument");
    }
    luaL_pushresultsize(&b, len);
    return 1;
}

/* ── ChaCha20 / Poly1305 ────────────────────────────────────────────── */

/* crypto.chacha20(key, nonce, counter, data) -> out
 * crypto.poly1305(key, msg) -> tag
 *
 * The two halves of chacha20-poly1305@openssh.com, which composes them itself
 * (separate keys for the length and the payload, the MAC over the encrypted
 * packet) - so the IETF AEAD does not fit and the raw primitives are bound.
 * RFC 8439 layout: 32-byte key, 12-byte nonce, 32-bit block counter. Like
 * aes256ctr, unauthenticated by itself; poly1305's key is ONE-TIME. */
static int lua_crypto_chacha20(lua_State *L)
{
    size_t klen, nlen, len;
    const char *key   = luaL_checklstring(L, 1, &klen);
    const char *nonce = luaL_checklstring(L, 2, &nlen);
    lua_Integer ctr   = luaL_checkinteger(L, 3);
    const char *in    = luaL_optlstring(L, 4, "", &len);

    if (klen != HL_CHACHA20_KEY_LEN)
        return luaL_error(L, "crypto.chacha20: key must be %d bytes, got %d",
                          (int)HL_CHACHA20_KEY_LEN, (int)klen);
    if (nlen != HL_CHACHA20_NONCE_LEN)
        return luaL_error(L, "crypto.chacha20: nonce must be %d bytes, got %d",
                          (int)HL_CHACHA20_NONCE_LEN, (int)nlen);
    if (ctr < 0 || ctr > 0xFFFFFFFFLL)
        return luaL_error(L, "crypto.chacha20: counter must be 0..2^32-1");
    if (!len) { lua_pushliteral(L, ""); return 1; }
    crypto_charge(L, len);   /* audit 9 H3 */

    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, len);
    int rc = hl_cap_crypto_chacha20((uint8_t *)out, (const uint8_t *)key,
                                    (const uint8_t *)nonce, (uint32_t)ctr, in, len);
    if (rc != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        return luaL_error(L, rc == -3
            ? "crypto.chacha20: no ChaCha20 backend in this build (TLS is not composed)"
            : "crypto.chacha20: bad argument (or the block counter would wrap)");
    }
    luaL_pushresultsize(&b, len);
    return 1;
}

static int lua_crypto_poly1305(lua_State *L)
{
    size_t klen, len;
    const char *key = luaL_checklstring(L, 1, &klen);
    const char *msg = luaL_optlstring(L, 2, "", &len);
    crypto_charge(L, len);
    if (klen != HL_POLY1305_KEY_LEN)
        return luaL_error(L, "crypto.poly1305: key must be %d bytes, got %d",
                          (int)HL_POLY1305_KEY_LEN, (int)klen);
    uint8_t tag[HL_POLY1305_TAG_LEN];
    int rc = hl_cap_crypto_poly1305(tag, (const uint8_t *)key, msg, len);
    if (rc != 0)
        return luaL_error(L, rc == -3
            ? "crypto.poly1305: no Poly1305 backend in this build (TLS is not composed)"
            : "crypto.poly1305: bad argument");
    lua_pushlstring(L, (const char *)tag, sizeof tag);
    return 1;
}

/* ── AES-256-GCM ────────────────────────────────────────────────────── */

/* crypto.gcm_seal(key, iv, aad, plaintext) -> ciphertext, tag
 * crypto.gcm_open(key, iv, aad, ciphertext, tag) -> plaintext | nil
 *
 * The cap layer has had AES-256-GCM since the SSH work needed it, but nothing
 * bound it to a runtime - so `crypto.gcm_seal` was nil and the first encrypted
 * SSH packet died on "attempt to call a nil value". Every SSH unit suite
 * passes its own AEAD table, which is why a shipped cipher layer could have no
 * cipher under it and still be green.
 *
 * Lengths are fixed (32/12/16) and checked here rather than clamped: GCM with
 * a wrong-length IV is a different construction, and silently accepting one
 * would produce packets no peer can open.
 *
 * Binary in, binary out. Lua strings are byte-clean, so no hex round-trip. */
static int lua_crypto_gcm_seal(lua_State *L)
{
    size_t klen, ivlen, aad_len, pt_len;
    const char *key = luaL_checklstring(L, 1, &klen);
    const char *iv  = luaL_checklstring(L, 2, &ivlen);
    const char *aad = luaL_optlstring(L, 3, "", &aad_len);
    const char *pt  = luaL_optlstring(L, 4, "", &pt_len);

    if (klen != HL_AEAD_KEY_LEN)
        return luaL_error(L, "crypto.gcm_seal: key must be %d bytes, got %d",
                          (int)HL_AEAD_KEY_LEN, (int)klen);
    if (ivlen != HL_AEAD_IV_LEN)
        return luaL_error(L, "crypto.gcm_seal: iv must be %d bytes, got %d",
                          (int)HL_AEAD_IV_LEN, (int)ivlen);

    crypto_charge(L, aad_len);   /* audit 9 H3 */
    crypto_charge(L, pt_len);
    uint8_t tag[HL_AEAD_TAG_LEN];
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, pt_len ? pt_len : 1);

    int rc = hl_cap_crypto_aes256gcm_seal((uint8_t *)out, tag,
                                          (const uint8_t *)key,
                                          (const uint8_t *)iv,
                                          aad_len ? aad : NULL, aad_len,
                                          pt_len ? pt : NULL, pt_len);
    if (rc != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        return luaL_error(L, rc == -3
            ? "crypto.gcm_seal: no AEAD backend in this build (TLS is not composed)"
            : "crypto.gcm_seal: bad argument");
    }
    luaL_pushresultsize(&b, pt_len);
    lua_pushlstring(L, (const char *)tag, sizeof tag);
    return 2;
}

static int lua_crypto_gcm_open(lua_State *L)
{
    size_t klen, ivlen, aad_len, ct_len, tag_len;
    const char *key = luaL_checklstring(L, 1, &klen);
    const char *iv  = luaL_checklstring(L, 2, &ivlen);
    const char *aad = luaL_optlstring(L, 3, "", &aad_len);
    const char *ct  = luaL_optlstring(L, 4, "", &ct_len);
    const char *tag = luaL_checklstring(L, 5, &tag_len);

    if (klen != HL_AEAD_KEY_LEN)
        return luaL_error(L, "crypto.gcm_open: key must be %d bytes, got %d",
                          (int)HL_AEAD_KEY_LEN, (int)klen);
    if (ivlen != HL_AEAD_IV_LEN)
        return luaL_error(L, "crypto.gcm_open: iv must be %d bytes, got %d",
                          (int)HL_AEAD_IV_LEN, (int)ivlen);
    /* A wrong-length tag is a failed open, not an error: it is attacker-
     * controlled input on the receive path, and raising there would turn a
     * forged packet into a crash instead of a rejection. */
    if (tag_len != HL_AEAD_TAG_LEN) { lua_pushnil(L); return 1; }
    crypto_charge(L, aad_len);   /* audit 9 H3 */
    crypto_charge(L, ct_len);

    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, ct_len ? ct_len : 1);

    int rc = hl_cap_crypto_aes256gcm_open((uint8_t *)out,
                                          (const uint8_t *)key,
                                          (const uint8_t *)iv,
                                          aad_len ? aad : NULL, aad_len,
                                          ct_len ? ct : NULL, ct_len,
                                          (const uint8_t *)tag);
    if (rc != 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        /* -3 is a BUILD fact: no AEAD backend was composed, so no packet can
         * ever be opened and the caller needs to know why. -2 is a tag that
         * did not verify, which is the normal way a forged or corrupted
         * packet arrives - data, not an error, so nil.
         *
         * These shared a code once, and this branch read it as "no backend":
         * every forged packet reported that the build lacked TLS. */
        if (rc == -3)
            return luaL_error(L, "crypto.gcm_open: no AEAD backend in this "
                                 "build (TLS is not composed)");
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, ct_len);
    return 1;
}

static const luaL_Reg crypto_funcs[] = {
    {"sha256",            lua_crypto_sha256},
    {"create_sha256",        lua_crypto_create_sha256},
    {"sha512",            lua_crypto_sha512},
    {"sha1",              lua_crypto_sha1},
    {"random",            lua_crypto_random},
    {"random_token",      lua_crypto_random_token},
    {"hash_password",     lua_crypto_hash_password},
    {"verify_password",   lua_crypto_verify_password},
    {"ed25519_keypair",   lua_crypto_ed25519_keypair},
    {"ed25519_sign",      lua_crypto_ed25519_sign},
    {"ed25519_verify",    lua_crypto_ed25519_verify},
    {"verify",            lua_crypto_verify},
    {"sign",              lua_crypto_sign},
    {"rsa_private_pem",   lua_crypto_rsa_private_pem},
    {"x509_pubkey_pem",   lua_crypto_x509_pubkey_pem},
    {"auth",              lua_crypto_auth},
    {"auth_verify",       lua_crypto_auth_verify},
    {"secretbox",         lua_crypto_secretbox},
    {"secretbox_open",    lua_crypto_secretbox_open},
    {"box",               lua_crypto_box},
    {"box_open",          lua_crypto_box_open},
    {"box_keypair",       lua_crypto_box_keypair},
    {"x25519",            lua_crypto_x25519},
    {"x25519_keypair",    lua_crypto_x25519_keypair},
    {"hmac_sha256",       lua_crypto_hmac_sha256},
    {"hmac_sha256_verify", lua_crypto_hmac_sha256_verify},
    {"constant_time_eq",  lua_crypto_constant_time_eq},
    {"hmac_sha1",         lua_crypto_hmac_sha1},
    {"gcm_seal",          lua_crypto_gcm_seal},
    {"gcm_open",          lua_crypto_gcm_open},
    {"aes256ctr",         lua_crypto_aes256ctr},
    {"chacha20",          lua_crypto_chacha20},
    {"poly1305",          lua_crypto_poly1305},
    {"bcrypt_pbkdf",      lua_crypto_bcrypt_pbkdf},
    {"bcrypt_pbkdf_env",  lua_crypto_bcrypt_pbkdf_env},
    {"key_from_env",      lua_crypto_key_from_env},
    {NULL, NULL}
};

int luaopen_hull_crypto(lua_State *L)
{
    register_sha256_hasher_mt(L);
    register_crypto_key_mt(L);
    luaL_newlib(L, crypto_funcs);

    /* Exported so a caller that reads a work factor out of a FILE can refuse
     * it with a message naming that file, instead of letting the derivation
     * fail here with no idea which key was wrong. Exported rather than
     * duplicated in Lua so the two cannot drift. */
    lua_pushinteger(L, (lua_Integer)HL_BCRYPT_MAX_ROUNDS);
    lua_setfield(L, -2, "BCRYPT_MAX_ROUNDS");
    return 1;
}
