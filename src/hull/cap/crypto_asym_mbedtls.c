/*
 * cap/asym_mbedtls.c - mbedTLS backend for asymmetric verify.
 *
 * Implements the HlCryptoAsymBackend vtable using mbedTLS's PK API. Covers
 * RSA-PKCS1v15 / RSA-PSS (RS256-512, PS256) and ECDSA (ES256, ES384)
 * with the JOSE-flavored raw r||s signature encoding.
 *
 * Build dependency:
 *   This file links mbedTLS PK + RSA + ECDSA + SHA. Hull pulls in
 *   mbedTLS unconditionally when HL_ENABLE_HTTP_CLIENT=1 (the TLS
 *   client needs it), which is the default build. On builds without
 *   mbedTLS (HL_ENABLE_HTTP_CLIENT=0 AND HL_ENABLE_HTTP_SERVER=0),
 *   the file is still compiled but `verify` short-circuits with
 *   "input error" so the symbol is always present at link time.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/crypto.h"
#include "hull/tls_feature.h"   /* hl_crypto_asym_active_backend (strong override) */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* mbedTLS is linked whenever HL_ENABLE_HTTP_CLIENT or HL_ENABLE_HTTP_SERVER
 * is on. The macro HL_ENABLE_HTTP is defined in that case (Makefile
 * sets it). Outside of that, we stub the backend so the symbol still
 * exists but every verify call reports "input error". */
#ifdef HL_ENABLE_HTTP

#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/bignum.h>
#include <mbedtls/asn1.h>
#include <mbedtls/asn1write.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/base64.h>

/* Per-alg static descriptor: hash type + hash size + (for ECDSA)
 * curve identifier. We don't allocate; the table is read-only and
 * indexed by the enum value, with HL_CRYPTO_ASYM_NONE = 0 as a sentinel
 * row that returns NULL on lookup. */
typedef struct {
    HlCryptoAsymAlg            alg;
    mbedtls_md_type_t    md;
    size_t               hash_len;
    int                  is_ecdsa;
    mbedtls_ecp_group_id curve;     /* meaningful iff is_ecdsa */
    size_t               coord_len; /* meaningful iff is_ecdsa */
    int                  is_pss;    /* meaningful iff !is_ecdsa */
} AlgDesc;

static const AlgDesc ALG_DESCS[] = {
    /* alg,         md,                 hash, ec, curve,                    coord, pss */
    { HL_CRYPTO_ASYM_RS256, MBEDTLS_MD_SHA256, 32,   0, MBEDTLS_ECP_DP_NONE,        0,    0 },
    { HL_CRYPTO_ASYM_RS384, MBEDTLS_MD_SHA384, 48,   0, MBEDTLS_ECP_DP_NONE,        0,    0 },
    { HL_CRYPTO_ASYM_RS512, MBEDTLS_MD_SHA512, 64,   0, MBEDTLS_ECP_DP_NONE,        0,    0 },
    { HL_CRYPTO_ASYM_PS256, MBEDTLS_MD_SHA256, 32,   0, MBEDTLS_ECP_DP_NONE,        0,    1 },
    { HL_CRYPTO_ASYM_ES256, MBEDTLS_MD_SHA256, 32,   1, MBEDTLS_ECP_DP_SECP256R1, 32,    0 },
    { HL_CRYPTO_ASYM_ES384, MBEDTLS_MD_SHA384, 48,   1, MBEDTLS_ECP_DP_SECP384R1, 48,    0 },
};

static const AlgDesc *desc_for(HlCryptoAsymAlg alg)
{
    for (size_t i = 0; i < sizeof(ALG_DESCS) / sizeof(ALG_DESCS[0]); i++)
        if (ALG_DESCS[i].alg == alg) return &ALG_DESCS[i];
    return NULL;
}

/* Compute the message digest for the given alg into out_buf, which
 * must hold at least desc->hash_len bytes. Returns 0 on success.
 * Uses streaming SHA APIs so the message buffer is read once. */
static int hash_message(const AlgDesc *desc,
                        const void *data, size_t data_len,
                        unsigned char *out_buf)
{
    int rc = -1;
    if (desc->md == MBEDTLS_MD_SHA256) {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        if (mbedtls_sha256_starts(&ctx, 0) == 0
            && mbedtls_sha256_update(&ctx, data, data_len) == 0
            && mbedtls_sha256_finish(&ctx, out_buf) == 0)
            rc = 0;
        mbedtls_sha256_free(&ctx);
    } else if (desc->md == MBEDTLS_MD_SHA384) {
        mbedtls_sha512_context ctx;
        mbedtls_sha512_init(&ctx);
        if (mbedtls_sha512_starts(&ctx, 1) == 0  /* is384 = 1 */
            && mbedtls_sha512_update(&ctx, data, data_len) == 0
            && mbedtls_sha512_finish(&ctx, out_buf) == 0)
            rc = 0;
        mbedtls_sha512_free(&ctx);
    } else if (desc->md == MBEDTLS_MD_SHA512) {
        mbedtls_sha512_context ctx;
        mbedtls_sha512_init(&ctx);
        if (mbedtls_sha512_starts(&ctx, 0) == 0
            && mbedtls_sha512_update(&ctx, data, data_len) == 0
            && mbedtls_sha512_finish(&ctx, out_buf) == 0)
            rc = 0;
        mbedtls_sha512_free(&ctx);
    }
    return rc;
}

/* PEM input to mbedtls_pk_parse_public_key requires:
 *   (a) a NUL-terminated buffer (the second arg passed to it is
 *       supposed to include the NUL in the length count), and
 *   (b) a trailing newline after `-----END PUBLIC KEY-----`.
 *
 * Callers (Lua / JS strings, file bytes) may not satisfy either:
 *   - JS template literals don't auto-add a trailing newline.
 *   - Some PEM exporters trim the trailing LF.
 *
 * pem_dup_nul copies into a scratch buffer, appends `\n` if the
 * input doesn't already end with one, and NUL-terminates. The
 * caller frees with free(). Returns NULL on out-of-memory or
 * comically-large input. */
static unsigned char *pem_dup_nul(const void *pem, size_t len,
                                  size_t *out_len_with_nl)
{
    /* Defensive cap: a PEM SPKI for an RSA-4096 key is around 800
     * bytes; an EC key is ~200. 64 KiB is generous and prevents
     * comically large inputs from succeeding silently. */
    if (len == 0 || len > 64 * 1024) return NULL;

    const unsigned char *p = (const unsigned char *)pem;
    int needs_nl = (p[len - 1] != '\n');
    size_t total = len + (needs_nl ? 1u : 0u);

    unsigned char *buf = (unsigned char *)malloc(total + 1);
    if (!buf) return NULL;
    memcpy(buf, pem, len);
    if (needs_nl) buf[len] = '\n';
    buf[total] = '\0';

    *out_len_with_nl = total;
    return buf;
}

/* JOSE-encoded ECDSA signature (raw r||s) to DER (mbedTLS's required
 * format for pk_verify). r and s are each fixed-width (coord_len
 * bytes), left-padded with zeros. DER output goes into out_buf;
 * actual encoded length is written to *out_len. out_buf must hold
 * at least 2*coord_len + 16 bytes (DER overhead is bounded). */
static int jose_to_der(const unsigned char *sig, size_t sig_len,
                       size_t coord_len,
                       unsigned char *out_buf, size_t out_buf_size,
                       size_t *out_len)
{
    if (sig_len != 2 * coord_len) return -1;

    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int rc = -1;
    if (mbedtls_mpi_read_binary(&r, sig, coord_len) != 0) goto out;
    if (mbedtls_mpi_read_binary(&s, sig + coord_len, coord_len) != 0) goto out;

    /* Build DER: SEQUENCE { INTEGER r, INTEGER s }. Write each
     * integer into a temp, then emit the wrapping SEQUENCE.
     *
     * mbedtls_asn1_write_mpi writes BACKWARDS from the tail of a
     * buffer (mbedTLS convention). We use the trailing region of
     * out_buf as the working area and end up with `p` pointing at
     * the start of the encoded SEQUENCE; the byte count from p to
     * the tail is the DER length. */
    unsigned char *end = out_buf + out_buf_size;
    unsigned char *p   = end;
    size_t total_len = 0;
    int n;

    n = mbedtls_asn1_write_mpi(&p, out_buf, &s);
    if (n < 0) goto out; total_len += (size_t)n;

    n = mbedtls_asn1_write_mpi(&p, out_buf, &r);
    if (n < 0) goto out; total_len += (size_t)n;

    n = mbedtls_asn1_write_len(&p, out_buf, total_len);
    if (n < 0) goto out; total_len += (size_t)n;

    n = mbedtls_asn1_write_tag(&p, out_buf,
                               MBEDTLS_ASN1_CONSTRUCTED |
                               MBEDTLS_ASN1_SEQUENCE);
    if (n < 0) goto out; total_len += (size_t)n;

    /* Shift the result to the front of the buffer so callers don't
     * need to know about mbedTLS's reverse-write convention. */
    memmove(out_buf, p, total_len);
    *out_len = total_len;
    rc = 0;

out:
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return rc;
}

static int mbed_supports(HlCryptoAsymAlg alg)
{
    return desc_for(alg) != NULL ? 1 : 0;
}

static int mbed_verify(const void *pubkey_pem, size_t pubkey_len,
                       HlCryptoAsymAlg alg,
                       const void *data, size_t data_len,
                       const void *sig,  size_t sig_len)
{
    if (!pubkey_pem || pubkey_len == 0) return -2;
    if (!data && data_len > 0) return -2;
    if (!sig || sig_len == 0) return -2;
    const AlgDesc *desc = desc_for(alg);
    if (!desc) return -2;

    /* 1. Get a NUL-terminated PEM with guaranteed trailing newline.
     *    mbedtls_pk_parse_public_key wants the length to INCLUDE the
     *    NUL terminator, hence the `pem_len_with_nl + 1`. */
    size_t pem_len_with_nl = 0;
    unsigned char *pem_copy = pem_dup_nul(pubkey_pem, pubkey_len,
                                          &pem_len_with_nl);
    if (!pem_copy) return -2;

    int rc = -1;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    if (mbedtls_pk_parse_public_key(&pk, pem_copy, pem_len_with_nl + 1) != 0)
        goto done;

    /* 2. Type check: PEM must match the requested alg family. This
     *    blocks alg-confusion attacks like an RSA key with an ES256
     *    alg claim sneaking past verify by being interpreted as a
     *    raw signed-payload check. */
    if (desc->is_ecdsa) {
        if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY
            && mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY_DH)
            goto done;
        /* Curve match. mbedtls_pk_ec is a macro returning the
         * inner ecp_keypair. */
        const mbedtls_ecp_keypair *ec = mbedtls_pk_ec(pk);
        if (!ec || ec->MBEDTLS_PRIVATE(grp).id != desc->curve)
            goto done;
    } else {
        if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_RSA)
            goto done;
        /* PSS uses the same RSA key as PKCS1v15 - mbedTLS's PK layer
         * doesn't distinguish at parse time; the scheme switch is on
         * the verify call. */
    }

    /* 3. Hash the message under the alg's digest. */
    unsigned char hash[64]; /* room for SHA-512 */
    if (hash_message(desc, data, data_len, hash) != 0)
        goto done;

    /* 4. Dispatch to the right verify routine. */
    if (desc->is_ecdsa) {
        /* JOSE r||s -> DER, then pk_verify. */
        unsigned char der[160]; /* 2*48 (P-384) + DER overhead < 110 */
        size_t der_len = 0;
        if (jose_to_der((const unsigned char *)sig, sig_len,
                        desc->coord_len,
                        der, sizeof(der), &der_len) != 0)
            goto done;
        if (mbedtls_pk_verify(&pk, desc->md,
                              hash, desc->hash_len,
                              der, der_len) == 0)
            rc = 0;
    } else if (desc->is_pss) {
        /* PSS: mbedTLS exposes via pk_verify_ext with options. */
        mbedtls_pk_rsassa_pss_options pss_opts;
        pss_opts.mgf1_hash_id  = desc->md;
        pss_opts.expected_salt_len = (int)desc->hash_len;
        if (mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &pss_opts,
                                  &pk, desc->md,
                                  hash, desc->hash_len,
                                  (const unsigned char *)sig, sig_len) == 0)
            rc = 0;
    } else {
        /* Plain PKCS1v15. */
        if (mbedtls_pk_verify(&pk, desc->md,
                              hash, desc->hash_len,
                              (const unsigned char *)sig, sig_len) == 0)
            rc = 0;
    }

done:
    mbedtls_pk_free(&pk);
    free(pem_copy);
    return rc;
}

const HlCryptoAsymBackend hl_crypto_asym_backend_mbedtls = {
    .supports = mbed_supports,
    .verify   = mbed_verify,
};

/* STRONG override of the base's weak hl_crypto_asym_active_backend() (cap/crypto.c):
 * when this mbedTLS TU is linked, asym verify uses the real backend. Absent on a
 * TLS-less base (this whole #ifdef is compiled out), where the weak fail-closed
 * stub wins. A composed libhull_feature-tls.a provides the same override. */
const HlCryptoAsymBackend *hl_crypto_asym_active_backend(void)
{
    return &hl_crypto_asym_backend_mbedtls;
}

/* ── X.509 -> SPKI PEM (for OIDC JWKS x5c entries) ───────────────── */

int hl_cap_crypto_x509_pubkey_pem(const void *der, size_t der_len,
                                  char *out_pem, size_t out_size,
                                  size_t *out_len)
{
    if (!der || der_len == 0 || !out_pem || out_size == 0 || !out_len)
        return -1;
    /* Defensive cap on input: an RSA-4096 cert is ~2 KiB; 64 KiB is
     * orders of magnitude past what any sane JWKS x5c entry can hold,
     * and protects against pathological input from an unverified
     * source (the JWKS response isn't authenticated until AFTER this
     * parse, so we treat it as untrusted). */
    if (der_len > 64 * 1024) return -1;

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int rc = -1;

    if (mbedtls_x509_crt_parse_der(&crt, (const unsigned char *)der,
                                    der_len) != 0)
        goto done;

    /* crt.pk_raw points at the SubjectPublicKeyInfo bytes already
     * encoded as DER inside the cert. PEM-format = b64 of those bytes
     * wrapped in BEGIN/END PUBLIC KEY headers. This avoids needing
     * MBEDTLS_PK_WRITE_C / MBEDTLS_PEM_WRITE_C (which would bloat the
     * binary by ~10 KiB for a feature only this one cap uses). */
    const unsigned char *spki_der = crt.pk_raw.p;
    size_t spki_der_len = crt.pk_raw.len;
    if (!spki_der || spki_der_len == 0) goto done;

    /* Probe the base64 output size first. */
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, spki_der, spki_der_len);
    if (b64_len == 0) goto done;

    /* PEM = 27 (header + NL) + base64 with one NL per 64 chars +
     * 26 (footer + NL) + 1 (final NL) + 1 (NUL). Base64 line-wrap
     * adds ceil(b64_len/64) newlines. Add slack for the NUL byte
     * mbedtls_base64_encode writes implicitly. */
    static const char HDR[] = "-----BEGIN PUBLIC KEY-----\n";
    static const char FTR[] = "-----END PUBLIC KEY-----\n";
    size_t hdr_len = sizeof(HDR) - 1;
    size_t ftr_len = sizeof(FTR) - 1;
    size_t wrapped_b64_len = b64_len + (b64_len / 64) + 1;
    size_t total = hdr_len + wrapped_b64_len + ftr_len;
    if (total + 1 > out_size) goto done;

    /* Emit header. */
    memcpy(out_pem, HDR, hdr_len);
    size_t pos = hdr_len;

    /* Emit base64 into a scratch buffer, then re-emit into out_pem
     * with a newline every 64 chars (PEM convention). */
    unsigned char *scratch = (unsigned char *)malloc(b64_len + 1);
    if (!scratch) goto done;
    size_t written = 0;
    if (mbedtls_base64_encode(scratch, b64_len + 1, &written,
                              spki_der, spki_der_len) != 0) {
        free(scratch);
        goto done;
    }

    for (size_t i = 0; i < written; i += 64) {
        size_t chunk = (written - i) > 64 ? 64 : (written - i);
        if (pos + chunk + 1 > out_size) {
            free(scratch);
            goto done;
        }
        memcpy(out_pem + pos, scratch + i, chunk);
        pos += chunk;
        out_pem[pos++] = '\n';
    }
    free(scratch);

    if (pos + ftr_len + 1 > out_size) goto done;
    memcpy(out_pem + pos, FTR, ftr_len);
    pos += ftr_len;
    out_pem[pos] = '\0';

    *out_len = pos;
    rc = 0;

done:
    mbedtls_x509_crt_free(&crt);
    return rc;
}

#else /* !HL_ENABLE_HTTP - mbedTLS is not linked */

/* Nothing is defined in this branch anymore: on a build without mbedTLS the base
 * provides the weak fail-closed defaults for the whole asym surface -- the
 * accessor + stub backend + x509 in cap/crypto.c. This TU only carries the REAL
 * mbedTLS implementations (the #ifdef branch), which compose into
 * libhull_feature-tls.a as strong overrides. See docs/tls_feature.md, a2. */

#endif /* HL_ENABLE_HTTP */

/* hl_crypto_asym_alg_{from,to}_string moved to cap/crypto.c (base-resident, pure
 * string<->enum helpers with no mbedTLS dependency) so mod_crypto's references
 * resolve on a TLS-less base where this whole TU is composed away. See
 * docs/tls_feature.md, a2. */

/* hl_cap_crypto_asym_verify (generic dispatcher) + hl_cap_crypto_asym_verify_default
 * moved to cap/crypto.c (base-resident) so they no longer tie those symbols to this
 * mbedTLS TU. verify_default now dispatches through hl_crypto_asym_active_backend(),
 * whose STRONG override lives below (HL_ENABLE_HTTP only). See docs/tls_feature.md. */
