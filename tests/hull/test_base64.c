/*
 * test_base64.c - tests for the utils/base64 leaf: RFC 4648 vectors, both
 * alphabets, padding control, strict decoding and the capacity contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "../../src/hull/utils/base64.h"

#include <string.h>

static const char *IN[]     = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
static const char *PADDED[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
static const char *BARE[]   = { "", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy" };

static int decode_ok(const char *s, unsigned flags, const char *want)
{
    uint8_t out[64];
    size_t n = 0;
    if (hl_base64_decode(s, strlen(s), out, sizeof out, &n, flags) != 0) return 0;
    return n == strlen(want) && memcmp(out, want, n) == 0;
}

static int decode_fails(const char *s, unsigned flags)
{
    uint8_t out[64];
    size_t n = 0;
    return hl_base64_decode(s, strlen(s), out, sizeof out, &n, flags) == -1;
}

UTEST(base64, rfc4648_padded)
{
    for (int i = 0; i < 7; i++) {
        char out[16];
        int n = hl_base64_encode(IN[i], strlen(IN[i]), out, sizeof out, 0);
        ASSERT_EQ(n, (int)strlen(PADDED[i]));
        ASSERT_STREQ(out, PADDED[i]);
        ASSERT_EQ(hl_base64_encoded_len(strlen(IN[i]), 0), strlen(PADDED[i]));
        ASSERT_TRUE(decode_ok(PADDED[i], 0, IN[i]));
    }
}

UTEST(base64, rfc4648_unpadded)
{
    for (int i = 0; i < 7; i++) {
        char out[16];
        int n = hl_base64_encode(IN[i], strlen(IN[i]), out, sizeof out, HL_BASE64_NOPAD);
        ASSERT_EQ(n, (int)strlen(BARE[i]));
        ASSERT_STREQ(out, BARE[i]);
        ASSERT_EQ(hl_base64_encoded_len(strlen(IN[i]), HL_BASE64_NOPAD), strlen(BARE[i]));
        /* Padding is optional on decode by default, refused with NOPAD. */
        ASSERT_TRUE(decode_ok(BARE[i], 0, IN[i]));
        ASSERT_TRUE(decode_ok(BARE[i], HL_BASE64_NOPAD, IN[i]));
    }
    ASSERT_TRUE(decode_fails("Zg==", HL_BASE64_NOPAD));
}

UTEST(base64, url_alphabet)
{
    const uint8_t in[] = { 0xfb, 0xff, 0xbf };
    char out[8];
    ASSERT_EQ(hl_base64_encode(in, 3, out, sizeof out, HL_BASE64_URL), 4);
    ASSERT_STREQ(out, "-_-_");
    ASSERT_EQ(hl_base64_encode(in, 3, out, sizeof out, 0), 4);
    ASSERT_STREQ(out, "+/+/");
    ASSERT_TRUE(decode_ok("-_-_", HL_BASE64_URL, "\xfb\xff\xbf"));
    ASSERT_TRUE(decode_fails("+/+/", HL_BASE64_URL));     /* std chars, url alphabet */
    ASSERT_TRUE(decode_fails("-_-_", 0));                 /* url chars, std alphabet */
}

UTEST(base64, every_byte_round_trips)
{
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    char enc[400];
    uint8_t dec[256];
    size_t n = 0;
    for (unsigned flags = 0; flags < 4; flags++) {
        ASSERT_TRUE(hl_base64_encode(all, sizeof all, enc, sizeof enc, flags) > 0);
        ASSERT_EQ(hl_base64_decode(enc, strlen(enc), dec, sizeof dec, &n, flags), 0);
        ASSERT_EQ(n, (size_t)256);
        ASSERT_EQ(memcmp(dec, all, 256), 0);
    }
}

UTEST(base64, strict_decoding)
{
    ASSERT_TRUE(decode_fails("Zm9v!YmFy", 0));    /* not in the alphabet */
    ASSERT_TRUE(decode_fails("Zm9v YmFy", 0));    /* no whitespace */
    ASSERT_TRUE(decode_fails("Zg=", 0));          /* incomplete padding */
    ASSERT_TRUE(decode_fails("Zm9v=", 0));        /* padding on a full group */
    ASSERT_TRUE(decode_fails("Zg===", 0));        /* too much padding */
    ASSERT_TRUE(decode_fails("Zg==Zg==", 0));     /* data after padding */
    ASSERT_TRUE(decode_fails("Zm9vY", 0));        /* a length nothing encodes to */
    ASSERT_TRUE(decode_fails("Zh==", 0));         /* non-zero unused bits ("Zg==") */
    ASSERT_TRUE(decode_fails("Zm9=", 0));         /* likewise ("Zm8=") */
    ASSERT_TRUE(decode_fails("Zh", HL_BASE64_URL));
}

UTEST(base64, auth_plain)
{
    /* SMTP AUTH PLAIN: base64(\0user\0pass). */
    const unsigned char plain[] = { 0, 'u', 's', 'e', 'r', 0, 'p', 'a', 's', 's' };
    char out[32];
    ASSERT_TRUE(hl_base64_encode(plain, sizeof plain, out, sizeof out, 0) > 0);
    ASSERT_STREQ(out, "AHVzZXIAcGFzcw==");
}

UTEST(base64, capacity_and_arguments)
{
    char small[4];                 /* "Zg==" needs 5 with the terminator */
    ASSERT_EQ(hl_base64_encode("f", 1, small, sizeof small, 0), -1);
    ASSERT_EQ(small[0], '\0');     /* never a partial, unterminated string */
    char exact[5];
    ASSERT_EQ(hl_base64_encode("f", 1, exact, sizeof exact, 0), 4);

    char buf[8];
    ASSERT_EQ(hl_base64_encode(NULL, 1, buf, sizeof buf, 0), -1);
    ASSERT_EQ(hl_base64_encode("f", 1, NULL, 8, 0), -1);
    ASSERT_EQ(hl_base64_encode(NULL, 0, buf, sizeof buf, 0), 0);
    ASSERT_STREQ(buf, "");

    uint8_t out[1];
    size_t n = 0;
    ASSERT_EQ(hl_base64_decode("Zm8=", 4, out, sizeof out, &n, 0), -1);  /* needs 2 */
    ASSERT_EQ(hl_base64_decode("Zg==", 4, out, sizeof out, &n, 0), 0);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(hl_base64_decode("", 0, NULL, 0, &n, 0), 0);
    ASSERT_EQ(n, (size_t)0);
}

UTEST_MAIN();
