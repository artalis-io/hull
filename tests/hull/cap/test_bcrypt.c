/*
 * test_bcrypt.c - the vendored bcrypt_pbkdf, pinned to a published vector.
 *
 * This KDF's whole value is agreeing byte-for-byte with what ssh-keygen wrote.
 * A subtly wrong one does not fail loudly: it derives a different key, the
 * decrypt yields garbage, and the symptom is indistinguishable from a wrong
 * passphrase. So the first test here is a KNOWN-ANSWER test against a vector
 * published by someone else - OpenBSD's own regress suite - and not against
 * anything this repository computed.
 *
 * The property tests below it are deliberately secondary. They would all pass
 * against a confidently wrong implementation; only the KAT would not.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include <stdint.h>
#include <string.h>

/* The vendored entry point. Declared here rather than pulled from a header:
 * vendor/bcrypt has no public header of its own, and the cap layer's wrapper
 * is what the rest of Hull uses. */
int bcrypt_pbkdf(const char *pass, size_t passlen,
                 const uint8_t *salt, size_t saltlen,
                 uint8_t *key, size_t keylen, unsigned int rounds);

/* ── the known-answer test ──────────────────────────────────────────── */

UTEST(bcrypt_pbkdf, matches_the_openbsd_regress_vector)
{
    /* src/regress/lib/libutil/bcrypt_pbkdf: pass "password", salt "salt",
     * 4 rounds. Transcribed from the published vector, NOT produced here. */
    static const uint8_t want[32] = {
        0x5b,0xbf,0x0c,0xc2,0x93,0x58,0x7f,0x1c,
        0x36,0x35,0x55,0x5c,0x27,0x79,0x65,0x98,
        0xd4,0x7e,0x57,0x90,0x71,0xbf,0x42,0x7e,
        0x9d,0x8f,0xbe,0x84,0x2a,0xba,0x34,0xd9
    };
    uint8_t out[32];
    ASSERT_EQ(bcrypt_pbkdf("password", 8, (const uint8_t *)"salt", 4,
                           out, sizeof out, 4), 0);
    ASSERT_EQ(memcmp(out, want, sizeof want), 0);
}

/* ── properties ─────────────────────────────────────────────────────── */

UTEST(bcrypt_pbkdf, is_deterministic)
{
    uint8_t a[48], b[48];
    ASSERT_EQ(bcrypt_pbkdf("pw", 2, (const uint8_t *)"NaCl", 4, a, sizeof a, 4), 0);
    ASSERT_EQ(bcrypt_pbkdf("pw", 2, (const uint8_t *)"NaCl", 4, b, sizeof b, 4), 0);
    ASSERT_EQ(memcmp(a, b, sizeof a), 0);
}

UTEST(bcrypt_pbkdf, a_different_passphrase_gives_different_bytes)
{
    uint8_t a[48], b[48];
    ASSERT_EQ(bcrypt_pbkdf("pw", 2, (const uint8_t *)"NaCl", 4, a, sizeof a, 4), 0);
    ASSERT_EQ(bcrypt_pbkdf("pX", 2, (const uint8_t *)"NaCl", 4, b, sizeof b, 4), 0);
    ASSERT_NE(memcmp(a, b, sizeof a), 0);
}

UTEST(bcrypt_pbkdf, a_different_salt_gives_different_bytes)
{
    uint8_t a[48], b[48];
    ASSERT_EQ(bcrypt_pbkdf("pw", 2, (const uint8_t *)"NaCl", 4, a, sizeof a, 4), 0);
    ASSERT_EQ(bcrypt_pbkdf("pw", 2, (const uint8_t *)"NaCm", 4, b, sizeof b, 4), 0);
    ASSERT_NE(memcmp(a, b, sizeof a), 0);
}

UTEST(bcrypt_pbkdf, rounds_change_the_output)
{
    /* Not merely "costs more": the round count is mixed in, so 4 and 8 are
     * different keys. A implementation that ignored rounds would still be
     * deterministic and still vary by passphrase - this is what catches it. */
    uint8_t a[48], b[48];
    ASSERT_EQ(bcrypt_pbkdf("pw", 2, (const uint8_t *)"NaCl", 4, a, sizeof a, 4), 0);
    ASSERT_EQ(bcrypt_pbkdf("pw", 2, (const uint8_t *)"NaCl", 4, b, sizeof b, 8), 0);
    ASSERT_NE(memcmp(a, b, sizeof a), 0);
}

UTEST(bcrypt_pbkdf, the_48_bytes_an_ssh_key_needs)
{
    /* aes256-ctr wants a 32-byte key and a 16-byte IV, taken as one 48-byte
     * draw. The first 32 must equal a 32-byte draw with the same inputs -
     * output length must not perturb the earlier bytes, or a key sealed by
     * ssh-keygen would not open. */
    uint8_t k48[48], k32[32];
    ASSERT_EQ(bcrypt_pbkdf("password", 8, (const uint8_t *)"salt", 4,
                           k48, sizeof k48, 4), 0);
    ASSERT_EQ(bcrypt_pbkdf("password", 8, (const uint8_t *)"salt", 4,
                           k32, sizeof k32, 4), 0);
    ASSERT_NE(memcmp(k48, k32, 32), 0);
    /* Documented, not a bug: bcrypt_pbkdf STRIPES its output, so the first
     * N bytes of a longer draw are NOT the shorter draw. The check above
     * pins that behaviour so nobody "fixes" it into a prefix relationship
     * and silently breaks every key ssh-keygen ever wrote. */
}

/* ── refusals ───────────────────────────────────────────────────────── */

UTEST(bcrypt_pbkdf, rejects_zero_rounds_and_empty_inputs)
{
    uint8_t out[32];
    ASSERT_NE(bcrypt_pbkdf("pw", 2, (const uint8_t *)"salt", 4, out, sizeof out, 0), 0);
    ASSERT_NE(bcrypt_pbkdf("pw", 2, (const uint8_t *)"salt", 4, out, 0, 4), 0);
    ASSERT_NE(bcrypt_pbkdf("pw", 2, (const uint8_t *)"salt", 0, out, sizeof out, 4), 0);
    ASSERT_NE(bcrypt_pbkdf("", 0, (const uint8_t *)"salt", 4, out, sizeof out, 4), 0);
}

UTEST_MAIN()
