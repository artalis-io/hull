/*
 * test_mime.c - MIME sniffer tests.
 *
 * Covers magic-byte signatures, text-shape heuristics, UTF-8
 * plain-text fallback, and edge cases (truncation, NULs, invalid
 * encodings). Plus smoke tests against real fixture files under
 * tests/fixtures/mime/.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/mime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Binary magic: PNG ─────────────────────────────────────────────── */

UTEST(hl_cap_mime, png_magic_detected)
{
    static const uint8_t png[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13,
    };
    ASSERT_STREQ(hl_cap_mime_sniff(png, sizeof(png)), "image/png");
}

UTEST(hl_cap_mime, png_full_magic_required)
{
    /* First 7 bytes match - but the 8th differs. Should NOT match. */
    static const uint8_t fake[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0xFF,
    };
    ASSERT_TRUE(hl_cap_mime_sniff(fake, sizeof(fake)) == NULL ||
                strcmp(hl_cap_mime_sniff(fake, sizeof(fake)), "image/png") != 0);
}

UTEST(hl_cap_mime, png_truncated_returns_null)
{
    /* Only 4 bytes of the 8-byte magic - too short to match. */
    static const uint8_t partial[] = {0x89, 'P', 'N', 'G'};
    const char *got = hl_cap_mime_sniff(partial, sizeof(partial));
    ASSERT_TRUE(got == NULL);
}

/* ── Binary magic: JPEG ────────────────────────────────────────────── */

UTEST(hl_cap_mime, jpeg_jfif_detected)
{
    /* FF D8 FF E0 → JFIF */
    static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 'J', 'F', 'I', 'F'};
    ASSERT_STREQ(hl_cap_mime_sniff(jpeg, sizeof(jpeg)), "image/jpeg");
}

UTEST(hl_cap_mime, jpeg_exif_detected)
{
    /* FF D8 FF E1 → Exif */
    static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE1, 'E', 'x', 'i', 'f'};
    ASSERT_STREQ(hl_cap_mime_sniff(jpeg, sizeof(jpeg)), "image/jpeg");
}

UTEST(hl_cap_mime, jpeg_truncated_returns_null)
{
    static const uint8_t partial[] = {0xFF, 0xD8};   /* 2 of 3 bytes */
    ASSERT_TRUE(hl_cap_mime_sniff(partial, sizeof(partial)) == NULL);
}

/* ── Binary magic: GIF ─────────────────────────────────────────────── */

UTEST(hl_cap_mime, gif87_detected)
{
    static const uint8_t gif[] = {'G', 'I', 'F', '8', '7', 'a', 0, 0};
    ASSERT_STREQ(hl_cap_mime_sniff(gif, sizeof(gif)), "image/gif");
}

UTEST(hl_cap_mime, gif89_detected)
{
    static const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a', 0, 0};
    ASSERT_STREQ(hl_cap_mime_sniff(gif, sizeof(gif)), "image/gif");
}

UTEST(hl_cap_mime, gif_wrong_version_no_match)
{
    static const uint8_t bogus[] = {'G', 'I', 'F', '8', '0', 'a'};
    const char *got = hl_cap_mime_sniff(bogus, sizeof(bogus));
    /* Not GIF - falls through to plain-text (printable ASCII) */
    ASSERT_TRUE(got == NULL || strcmp(got, "image/gif") != 0);
}

/* ── Binary magic: WebP ────────────────────────────────────────────── */

UTEST(hl_cap_mime, webp_detected)
{
    /* RIFF + 4-byte size + WEBP */
    static const uint8_t webp[] = {
        'R', 'I', 'F', 'F', 0x10, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ',
    };
    ASSERT_STREQ(hl_cap_mime_sniff(webp, sizeof(webp)), "image/webp");
}

UTEST(hl_cap_mime, webp_only_riff_returns_null)
{
    /* RIFF prefix but the type at offset 8 is "WAVE" - not WebP. */
    static const uint8_t wave[] = {
        'R', 'I', 'F', 'F', 0x10, 0, 0, 0, 'W', 'A', 'V', 'E',
    };
    const char *got = hl_cap_mime_sniff(wave, sizeof(wave));
    ASSERT_TRUE(got == NULL || strcmp(got, "image/webp") != 0);
}

UTEST(hl_cap_mime, webp_short_buffer_returns_null)
{
    /* Only 11 bytes - WEBP magic requires 12. */
    static const uint8_t partial[] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B',
    };
    const char *got = hl_cap_mime_sniff(partial, sizeof(partial));
    ASSERT_TRUE(got == NULL || strcmp(got, "image/webp") != 0);
}

/* ── Binary magic: PDF ─────────────────────────────────────────────── */

UTEST(hl_cap_mime, pdf_detected)
{
    static const uint8_t pdf[] = {'%', 'P', 'D', 'F', '-', '1', '.', '7'};
    ASSERT_STREQ(hl_cap_mime_sniff(pdf, sizeof(pdf)), "application/pdf");
}

UTEST(hl_cap_mime, pdf_truncated_does_not_match_pdf)
{
    /* 3 of 5 PDF magic bytes - must NOT match application/pdf.
     * Falls through to the plain-text fallback (the bytes are
     * printable ASCII), which is correct. */
    static const uint8_t partial[] = {'%', 'P', 'D'};
    const char *got = hl_cap_mime_sniff(partial, sizeof(partial));
    ASSERT_TRUE(got == NULL || strcmp(got, "application/pdf") != 0);
}

/* ── Text shape: SVG ───────────────────────────────────────────────── */

UTEST(hl_cap_mime, svg_with_xml_prolog)
{
    const char *svg = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)svg, strlen(svg)),
                 "image/svg+xml");
}

UTEST(hl_cap_mime, svg_without_xml_prolog)
{
    const char *svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)svg, strlen(svg)),
                 "image/svg+xml");
}

UTEST(hl_cap_mime, svg_case_insensitive)
{
    const char *svg = "<SVG width=\"1\" height=\"1\"/>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)svg, strlen(svg)),
                 "image/svg+xml");
}

UTEST(hl_cap_mime, svg_with_leading_whitespace)
{
    const char *svg = "  \n\t<svg xmlns=\"...\"/>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)svg, strlen(svg)),
                 "image/svg+xml");
}

UTEST(hl_cap_mime, svg_takes_priority_over_html)
{
    /* Pathological case: `<svg ... >` inside a buffer that also contains
     * `<html>`. SVG should win because it appears at offset 0. */
    const char *mixed = "<svg><html></html></svg>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)mixed, strlen(mixed)),
                 "image/svg+xml");
}

/* ── Text shape: HTML ──────────────────────────────────────────────── */

UTEST(hl_cap_mime, html_doctype)
{
    const char *html = "<!DOCTYPE html>\n<html>...</html>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)html, strlen(html)),
                 "text/html");
}

UTEST(hl_cap_mime, html_lowercase_tag)
{
    const char *html = "<html><body>hi</body></html>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)html, strlen(html)),
                 "text/html");
}

UTEST(hl_cap_mime, html_uppercase_tag)
{
    const char *html = "<HTML><BODY>HI</BODY></HTML>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)html, strlen(html)),
                 "text/html");
}

UTEST(hl_cap_mime, html_comment_prefix)
{
    const char *html = "<!-- hand-rolled -->\n<html></html>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)html, strlen(html)),
                 "text/html");
}

UTEST(hl_cap_mime, html_with_leading_whitespace)
{
    const char *html = " \n\t <!doctype html><html></html>";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)html, strlen(html)),
                 "text/html");
}

/* ── Plain-text fallback ───────────────────────────────────────────── */

UTEST(hl_cap_mime, plaintext_ascii)
{
    const char *txt = "Hello, world!\nThis is plain text.";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)txt, strlen(txt)),
                 "text/plain");
}

UTEST(hl_cap_mime, plaintext_utf8_multibyte)
{
    /* "café" with é encoded as 2-byte UTF-8 (0xC3 0xA9) */
    const char *txt = "caf\xC3\xA9 - résumé";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)txt, strlen(txt)),
                 "text/plain");
}

UTEST(hl_cap_mime, plaintext_utf8_4byte_emoji)
{
    /* "ok 👍" with thumbs-up as 4-byte UTF-8 (F0 9F 91 8D) */
    const char *txt = "ok \xF0\x9F\x91\x8D";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)txt, strlen(txt)),
                 "text/plain");
}

UTEST(hl_cap_mime, plaintext_with_nul_returns_null)
{
    static const uint8_t txt[] = {'h', 'i', 0x00, 't', 'h', 'e', 'r', 'e'};
    ASSERT_TRUE(hl_cap_mime_sniff(txt, sizeof(txt)) == NULL);
}

UTEST(hl_cap_mime, plaintext_invalid_utf8_returns_null)
{
    /* Lone continuation byte - invalid UTF-8 */
    static const uint8_t bad[] = {'a', 0x80, 'b'};
    const char *got = hl_cap_mime_sniff(bad, sizeof(bad));
    ASSERT_TRUE(got == NULL || strcmp(got, "text/plain") != 0);
}

UTEST(hl_cap_mime, plaintext_overlong_utf8_returns_null)
{
    /* Overlong 2-byte encoding of NUL (C0 80) - invalid */
    static const uint8_t bad[] = {0xC0, 0x80, 'x'};
    const char *got = hl_cap_mime_sniff(bad, sizeof(bad));
    ASSERT_TRUE(got == NULL || strcmp(got, "text/plain") != 0);
}

UTEST(hl_cap_mime, plaintext_surrogate_returns_null)
{
    /* U+D800 in UTF-8 encoding (ED A0 80) - reserved for UTF-16 surrogates */
    static const uint8_t bad[] = {'a', 0xED, 0xA0, 0x80, 'b'};
    const char *got = hl_cap_mime_sniff(bad, sizeof(bad));
    ASSERT_TRUE(got == NULL || strcmp(got, "text/plain") != 0);
}

UTEST(hl_cap_mime, json_classified_as_plaintext)
{
    /* Per design: JSON is valid UTF-8 text → classified as text/plain.
     * Callers that need JSON discrimination should use the declared
     * Content-Type header. */
    const char *json = "{\"name\":\"alice\",\"count\":42}";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)json, strlen(json)),
                 "text/plain");
}

UTEST(hl_cap_mime, csv_classified_as_plaintext)
{
    const char *csv = "name,age,city\nalice,30,nyc\nbob,25,sf\n";
    ASSERT_STREQ(hl_cap_mime_sniff((const uint8_t *)csv, strlen(csv)),
                 "text/plain");
}

/* ── Edge cases ────────────────────────────────────────────────────── */

UTEST(hl_cap_mime, null_buffer_returns_null)
{
    ASSERT_TRUE(hl_cap_mime_sniff(NULL, 100) == NULL);
}

UTEST(hl_cap_mime, zero_length_returns_null)
{
    static const uint8_t buf[1] = {0};
    ASSERT_TRUE(hl_cap_mime_sniff(buf, 0) == NULL);
}

UTEST(hl_cap_mime, one_byte_text_classified)
{
    static const uint8_t a = 'a';
    ASSERT_STREQ(hl_cap_mime_sniff(&a, 1), "text/plain");
}

UTEST(hl_cap_mime, one_byte_binary_returns_null)
{
    static const uint8_t b = 0xFE;
    /* High byte alone is an invalid UTF-8 lead - not text. */
    const char *got = hl_cap_mime_sniff(&b, 1);
    ASSERT_TRUE(got == NULL);
}

UTEST(hl_cap_mime, random_binary_returns_null)
{
    /* Mixed high bytes + NUL - no magic, no valid UTF-8. */
    static const uint8_t garbage[] = {
        0xFF, 0xFE, 0x00, 0x80, 0x90, 0xC0, 0xFE, 0xFF,
    };
    ASSERT_TRUE(hl_cap_mime_sniff(garbage, sizeof(garbage)) == NULL);
}

/* ── Fixture-file smoke tests ──────────────────────────────────────── */

static int read_fixture(const char *name, uint8_t **out_buf, size_t *out_len)
{
    char path[512];
    snprintf(path, sizeof(path), "tests/fixtures/mime/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Tests might be invoked from a different cwd - try parent. */
        snprintf(path, sizeof(path), "../tests/fixtures/mime/%s", name);
        f = fopen(path, "rb");
    }
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return -1; }
    *out_buf = buf;
    *out_len = (size_t)sz;
    return 0;
}

UTEST(hl_cap_mime_fixture, png)
{
    uint8_t *buf; size_t len;
    ASSERT_EQ(read_fixture("tiny.png", &buf, &len), 0);
    ASSERT_STREQ(hl_cap_mime_sniff(buf, len), "image/png");
    free(buf);
}

UTEST(hl_cap_mime_fixture, jpeg)
{
    uint8_t *buf; size_t len;
    ASSERT_EQ(read_fixture("tiny.jpg", &buf, &len), 0);
    ASSERT_STREQ(hl_cap_mime_sniff(buf, len), "image/jpeg");
    free(buf);
}

UTEST(hl_cap_mime_fixture, gif)
{
    uint8_t *buf; size_t len;
    ASSERT_EQ(read_fixture("tiny.gif", &buf, &len), 0);
    ASSERT_STREQ(hl_cap_mime_sniff(buf, len), "image/gif");
    free(buf);
}

UTEST(hl_cap_mime_fixture, webp)
{
    uint8_t *buf; size_t len;
    ASSERT_EQ(read_fixture("tiny.webp", &buf, &len), 0);
    ASSERT_STREQ(hl_cap_mime_sniff(buf, len), "image/webp");
    free(buf);
}

UTEST(hl_cap_mime_fixture, pdf)
{
    uint8_t *buf; size_t len;
    ASSERT_EQ(read_fixture("tiny.pdf", &buf, &len), 0);
    ASSERT_STREQ(hl_cap_mime_sniff(buf, len), "application/pdf");
    free(buf);
}

UTEST(hl_cap_mime_fixture, svg)
{
    uint8_t *buf; size_t len;
    ASSERT_EQ(read_fixture("tiny.svg", &buf, &len), 0);
    ASSERT_STREQ(hl_cap_mime_sniff(buf, len), "image/svg+xml");
    free(buf);
}

UTEST(hl_cap_mime_fixture, html)
{
    uint8_t *buf; size_t len;
    ASSERT_EQ(read_fixture("tiny.html", &buf, &len), 0);
    ASSERT_STREQ(hl_cap_mime_sniff(buf, len), "text/html");
    free(buf);
}

UTEST_MAIN()
