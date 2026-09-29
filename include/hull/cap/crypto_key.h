/*
 * hull/cap/crypto_key.h - a 32-byte secret key held in C.
 *
 * The keys for sealing values at rest (hull/crypto/sealbox, and through it
 * hull/kv's `encrypt` and TOTP's secret store) were Lua / JS strings: copied
 * by every concatenation, kept until the collector gets round to them, and
 * impossible to wipe. An HlCryptoKey keeps the bytes in one buffer this
 * module owns, exposes no way to read them back, does the secretbox sealing
 * itself, and zeroes the buffer when freed. The bindings create one from the
 * VALUE of an environment variable (read under the manifest's env allowlist),
 * so a key that lives in the environment never becomes a script value at all.
 *
 * Honest about the limit, as bcrypt_pbkdf_env is: the value still sits in the
 * process environment for the process's lifetime. What this removes is the
 * second, unscrubbable copy in the script heap.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_CRYPTO_KEY_H
#define HULL_CAP_CRYPTO_KEY_H

#include <stddef.h>
#include <stdint.h>

#define HL_CRYPTO_KEY_LEN 32

typedef struct HlCryptoKey HlCryptoKey;

/**
 * A key from its text form: 64 hex digits (either case), or base64 of 32
 * bytes (standard or url-safe alphabet, padded or not). Surrounding
 * whitespace is ignored - a key read from a secret file usually ends in a
 * newline. Anything else, including a key of another length, is refused.
 *
 * @return 0 with *out set (free with hl_cap_crypto_key_free), or -1. The
 *         caller's text is not modified; wiping it is the caller's business.
 */
int hl_cap_crypto_key_from_text(const char *text, size_t len, HlCryptoKey **out);

/** NaCl secretbox under the held key. `out` receives msg_len + 16 bytes.
 *  @return 0, or -1. */
int hl_cap_crypto_key_secretbox(const HlCryptoKey *k, uint8_t *out,
                                const void *msg, size_t msg_len,
                                const uint8_t nonce[24]);

/** Open a secretbox under the held key. `out` receives ct_len - 16 bytes.
 *  @return 0, or -1 if it does not authenticate (out is then zeroed). */
int hl_cap_crypto_key_secretbox_open(const HlCryptoKey *k, uint8_t *out,
                                     const void *ct, size_t ct_len,
                                     const uint8_t nonce[24]);

/** Zero the key and free it. NULL is fine. */
void hl_cap_crypto_key_free(HlCryptoKey *k);

#endif /* HULL_CAP_CRYPTO_KEY_H */
