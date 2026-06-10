/*
 * cap/crypto_hmac_mbedtls.c - mbedTLS backend for HMAC compute.
 *
 * Implements the HlCryptoHmacBackend vtable using mbedTLS's MD layer
 * (mbedtls_md_hmac). Covers HMAC-SHA1 / HMAC-SHA256 / HMAC-SHA512.
 *
 * Build dependency:
 *   This file links mbedTLS MD + SHA1/SHA256/SHA512. Hull pulls in
 *   mbedTLS unconditionally when HL_ENABLE_HTTP_CLIENT=1 (the TLS
 *   client needs it), which is the default build. On builds without
 *   mbedTLS (HL_ENABLE_HTTP_CLIENT=0 AND HL_ENABLE_HTTP_SERVER=0),
 *   the file is still compiled but `compute` short-circuits with
 *   "internal failure" so the symbol is always present at link time.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/crypto.h"

#include <stddef.h>
#include <stdint.h>

#ifdef HL_ENABLE_HTTP
#include <mbedtls/md.h>

static mbedtls_md_type_t md_type_for(HlCryptoHmacAlg alg)
{
    switch (alg) {
        case HL_CRYPTO_HMAC_SHA1:   return MBEDTLS_MD_SHA1;
        case HL_CRYPTO_HMAC_SHA256: return MBEDTLS_MD_SHA256;
        case HL_CRYPTO_HMAC_SHA512: return MBEDTLS_MD_SHA512;
        default:                    return MBEDTLS_MD_NONE;
    }
}

static size_t digest_size_for(HlCryptoHmacAlg alg)
{
    switch (alg) {
        case HL_CRYPTO_HMAC_SHA1:   return 20;
        case HL_CRYPTO_HMAC_SHA256: return 32;
        case HL_CRYPTO_HMAC_SHA512: return 64;
        default:                    return 0;
    }
}

static int mbedtls_supports(HlCryptoHmacAlg alg)
{
    return md_type_for(alg) != MBEDTLS_MD_NONE;
}

static int mbedtls_compute(const HlCryptoHmacBackend *self,
                            HlCryptoHmacAlg alg,
                            const uint8_t *key, size_t key_len,
                            const uint8_t *msg, size_t msg_len,
                            uint8_t *out, size_t out_len)
{
    (void)self;
    if (!key || !msg || !out) return -1;
    if (out_len != digest_size_for(alg)) return -1;

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type_for(alg));
    if (!info) return -1;

    int rc = mbedtls_md_hmac(info, key, key_len, msg, msg_len, out);
    return rc == 0 ? 0 : -1;
}

#else /* !HL_ENABLE_HTTP */

static int mbedtls_supports(HlCryptoHmacAlg alg)
{
    (void)alg;
    return 0;
}

static int mbedtls_compute(const HlCryptoHmacBackend *self,
                            HlCryptoHmacAlg alg,
                            const uint8_t *key, size_t key_len,
                            const uint8_t *msg, size_t msg_len,
                            uint8_t *out, size_t out_len)
{
    (void)self; (void)alg; (void)key; (void)key_len;
    (void)msg; (void)msg_len; (void)out; (void)out_len;
    return -1;
}

#endif

const HlCryptoHmacBackend hl_crypto_hmac_backend_mbedtls = {
    .supports = mbedtls_supports,
    .compute  = mbedtls_compute,
};
