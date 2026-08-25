/*
 * test_hex.c - boundary tests for the utils/hex byte->hex leaf.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "../../src/hull/utils/hex.h"

#include <string.h>

UTEST(hex, empty_input)
{
    /* Zero bytes: writes just a terminator; in may be NULL. */
    char out[4] = { 'x', 'x', 'x', 'x' };
    ASSERT_EQ(hl_hex_encode(NULL, 0, out, sizeof(out)), 0);
    ASSERT_EQ(out[0], '\0');
    ASSERT_EQ((int)strlen(out), 0);
}

UTEST(hex, empty_input_min_capacity)
{
    /* out_cap == 1 is exactly enough for the terminator of a 0-byte input. */
    char out[1] = { 'x' };
    ASSERT_EQ(hl_hex_encode(NULL, 0, out, 1), 0);
    ASSERT_EQ(out[0], '\0');
}

UTEST(hex, one_byte)
{
    const uint8_t in[] = { 0xAB };
    char out[3];
    ASSERT_EQ(hl_hex_encode(in, 1, out, sizeof(out)), 0);
    ASSERT_STREQ(out, "ab");            /* lowercase */
    ASSERT_EQ(out[2], '\0');            /* terminated */
}

UTEST(hex, lowercase_full_range)
{
    /* Covers both nibbles across the alphabet incl. 0x00 and 0xff. */
    const uint8_t in[] = { 0x00, 0x0f, 0xf0, 0xff, 0x12, 0x9a };
    char out[13];
    ASSERT_EQ(hl_hex_encode(in, sizeof(in), out, sizeof(out)), 0);
    ASSERT_STREQ(out, "000ff0ff129a");
}

UTEST(hex, typical_digest_32_bytes)
{
    uint8_t in[32];
    for (int i = 0; i < 32; i++) in[i] = (uint8_t)i;
    char out[65];
    ASSERT_EQ(hl_hex_encode(in, 32, out, sizeof(out)), 0);
    ASSERT_EQ((int)strlen(out), 64);
    ASSERT_STREQ(out,
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f");
    ASSERT_EQ(out[64], '\0');
}

UTEST(hex, exact_output_capacity)
{
    /* out_cap == in_len*2 + 1 is the minimum that succeeds. */
    const uint8_t in[] = { 0xDE, 0xAD };
    char out[5];                        /* 2*2 + 1 */
    ASSERT_EQ(hl_hex_encode(in, 2, out, 5), 0);
    ASSERT_STREQ(out, "dead");
    ASSERT_EQ(out[4], '\0');
}

UTEST(hex, insufficient_capacity_fails_closed)
{
    /* out_cap one short of the required in_len*2 + 1 must fail and leave the
     * destination as an empty (terminated) string, never a partial digest. */
    const uint8_t in[] = { 0xDE, 0xAD };
    char out[5];
    memset(out, 'Z', sizeof(out));
    ASSERT_EQ(hl_hex_encode(in, 2, out, 4), -1);   /* need 5, given 4 */
    ASSERT_EQ(out[0], '\0');
}

UTEST(hex, zero_capacity_and_null_out)
{
    const uint8_t in[] = { 0x01 };
    char out[2];
    ASSERT_EQ(hl_hex_encode(in, 1, out, 0), -1);   /* zero capacity */
    ASSERT_EQ(hl_hex_encode(in, 1, NULL, 2), -1);  /* NULL destination */
}

UTEST(hex, null_input_nonzero_len_fails)
{
    char out[8];
    ASSERT_EQ(hl_hex_encode(NULL, 4, out, sizeof(out)), -1);
    ASSERT_EQ(out[0], '\0');
}

UTEST_MAIN();
