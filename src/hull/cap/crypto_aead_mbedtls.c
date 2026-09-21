/*
 * cap/crypto_aead_mbedtls.c - AES-256-GCM, mbedTLS backend.
 *
 * Strong override of the weak hl_crypto_aead_active_backend() hook in
 * cap/crypto.c, mirroring the HMAC and asym backends beside it. On a TLS-less
 * base this TU is absent and the weak fail-closed stub answers instead, so an
 * AEAD call without the composed feature refuses rather than crashing.
 *
 * Why mbedTLS rather than an in-tree implementation: AES-GCM is exactly the
 * kind of primitive that should not be hand-rolled - the GHASH side channel
 * and the tag comparison are both easy to get subtly wrong, and mbedTLS is
 * already vendored, reviewed, and hardware-accelerated where the CPU offers it
 * (mbed_aesni.o / mbed_aesce.o are in the same archive).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/crypto.h"
#include "hull/tls_feature.h"   /* hl_crypto_aead_active_backend (strong override) */

#include <stdint.h>
#include <string.h>

/* mbedTLS is linked whenever HL_ENABLE_HTTP_CLIENT or HL_ENABLE_HTTP_SERVER is
 * on; the Makefile defines HL_ENABLE_HTTP in that case. Same guard the asym and
 * hmac backends use, so a pure-compute build compiles this to nothing. */
#ifdef HL_ENABLE_HTTP

#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>   /* mbedtls_platform_zeroize */

#define AEAD_KEY_BITS 256
#define AEAD_TAG_LEN  16
#define AEAD_IV_LEN   12

static int aead_seal(uint8_t *out, uint8_t tag[16],
                     const uint8_t key[32], const uint8_t iv[12],
                     const void *aad, size_t aad_len,
                     const void *pt, size_t pt_len)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, AEAD_KEY_BITS);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, pt_len,
                                       iv, AEAD_IV_LEN,
                                       (const unsigned char *)aad, aad_len,
                                       (const unsigned char *)pt, out,
                                       AEAD_TAG_LEN, tag);

    mbedtls_gcm_free(&ctx);
    return rc == 0 ? 0 : -1;
}

static int aead_open(uint8_t *out,
                     const uint8_t key[32], const uint8_t iv[12],
                     const void *aad, size_t aad_len,
                     const void *ct, size_t ct_len,
                     const uint8_t tag[16])
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, AEAD_KEY_BITS);
    if (rc == 0)
        rc = mbedtls_gcm_auth_decrypt(&ctx, ct_len, iv, AEAD_IV_LEN,
                                      (const unsigned char *)aad, aad_len,
                                      tag, AEAD_TAG_LEN,
                                      (const unsigned char *)ct, out);

    mbedtls_gcm_free(&ctx);

    /* Authentication failure and a malformed call are both -2 here: the caller
     * gets "this did not authenticate" either way, and distinguishing them
     * would only tell an attacker which of their two guesses was closer.
     * mbedTLS has already left `out` untouched on a tag mismatch. */
    if (rc != 0) {
        if (ct_len && out) mbedtls_platform_zeroize(out, ct_len);
        return -2;
    }
    return 0;
}

const HlCryptoAeadBackend hl_crypto_aead_backend_mbedtls = {
    .seal = aead_seal,
    .open = aead_open,
};

/* STRONG override of the base's weak hl_crypto_aead_active_backend()
 * (cap/crypto.c): present iff this TU is linked, which is exactly when mbedTLS
 * is. */
const HlCryptoAeadBackend *hl_crypto_aead_active_backend(void)
{
    return &hl_crypto_aead_backend_mbedtls;
}

#endif /* HL_ENABLE_HTTP */
