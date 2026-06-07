/*
 * hull_config.h — Custom mbedTLS configuration for Hull/Keel
 *
 * Enables TLS 1.2 client and server with modern ciphers.
 * No DTLS, no PSK, no obsolete ciphers, no TLS 1.3 (simplifies config).
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#ifndef HULL_MBEDTLS_CONFIG_H
#define HULL_MBEDTLS_CONFIG_H

/* ── System support ──────────────────────────────────────────────── */

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C

/* ── Feature selection ───────────────────────────────────────────── */

/* TLS protocol — 1.2 only (no TLS 1.3 to avoid PSA crypto dependency) */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* Key exchange */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* X.509 certificates */
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

/* PEM parsing (for cert/key files) */
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* ASN.1 */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* Public key */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_BIGNUM_C

/* ECC curves */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* Ciphers — modern only */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CCM_C

/* Hashes */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* NOTE on hardware SHA-256 acceleration: enabling
 * MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT here is a no-op for
 * hull/blob@1 because Hull's cap layer uses a *hand-rolled* SHA-256
 * implementation in src/hull/cap/crypto.c (see HlSha256Ctx /
 * sha256_transform). The hand-roll is intentional — keeps the
 * incremental API working in HL_ENABLE_HTTP_SERVER=0 +
 * HL_ENABLE_HTTP_CLIENT=0 builds where mbedTLS is dropped entirely.
 *
 * Real acceleration needs one of:
 *   - Direct ARMv8 SHA2 (FEAT_SHA256) intrinsics in cap/crypto.c
 *     (NEON / arm_neon_sha2.h — best, no dependency)
 *   - Compile-time switch to mbedTLS's sha256 when linked
 *     (couples blob perf to HTTP build flavor — awkward)
 *
 * Tracked in docs/blob.md §Performance baseline as a v0.1.10+
 * optimization. Until then, large-blob put on Apple Silicon is
 * SHA-bound at ~200 MB/s. */

/* Entropy + RNG */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* Misc */
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_CIPHER_PADDING_PKCS7
#define MBEDTLS_VERSION_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_NET_C

/* ── Size limits ─────────────────────────────────────────────────── */

#define MBEDTLS_SSL_MAX_CONTENT_LEN  16384
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN  16384
#define MBEDTLS_MPI_MAX_SIZE         1024

/* Note: check_config.h is included automatically by build_info.h
 * after all config_adjust_*.h headers have run. Do NOT include
 * it here — the adjust headers haven't run yet at this point. */

#endif /* HULL_MBEDTLS_CONFIG_H */
