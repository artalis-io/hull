/*
 * crypto_api.h - Hull's shim for the one OpenSSH API bcrypt_pbkdf.c uses.
 *
 * Upstream this header is OpenSSH's façade over its bundled NaCl. bcrypt_pbkdf
 * needs exactly one thing from it, SHA-512, so this supplies that and nothing
 * else - and supplies it from Hull's own cap layer rather than carrying a
 * second SHA-512. Hull already has one (TweetNaCl, reached through
 * hl_cap_crypto_sha512), and two implementations of a hash in one binary is a
 * way for them to disagree.
 *
 * The argument ORDER differs between the two, which is the whole reason this
 * is an adapter and not a #define: OpenSSH writes (out, in, inlen) and Hull
 * writes (in, inlen, out). Aliasing them by macro would compile and silently
 * hash the output buffer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_VENDOR_BCRYPT_CRYPTO_API_H
#define HULL_VENDOR_BCRYPT_CRYPTO_API_H

#include <stddef.h>
#include <stdint.h>

#define crypto_hash_sha512_BYTES 64U

/* Hull's SHA-512: (data, len, out[64]), 0 on success. */
int hl_cap_crypto_sha512(const void *data, size_t len, uint8_t out[64]);

/* NOTE ON THE RETURN VALUE. bcrypt_pbkdf.c ignores what this returns, because
 * upstream's crypto_hash_sha512 cannot fail. Hull's hl_cap_crypto_sha512 CAN
 * (it returns -1 on a NULL argument), and all three call sites in the
 * vendored file pass provably non-NULL buffers - so the difference is
 * unreachable today. It is written down because it would not stay unreachable
 * quietly: the destinations there are uninitialised stack arrays, so a
 * SHA-512 that failed without anyone looking would derive a key from stack
 * garbage rather than raise. Any change making Hull's SHA-512 fallible for
 * these inputs has to revisit the vendored call sites. */
static inline int crypto_hash_sha512(unsigned char *out,
                                     const unsigned char *in,
                                     unsigned long long inlen)
{
    return hl_cap_crypto_sha512(in, (size_t)inlen, out);
}

#endif /* HULL_VENDOR_BCRYPT_CRYPTO_API_H */
