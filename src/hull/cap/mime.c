/**
 * @file cap/mime.c
 * @brief Content-MIME sniffer implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/mime.h"
#include <string.h>

/* Sniff window: inspect up to this many bytes for shape heuristics
 * (SVG / HTML scans and UTF-8 validation). The binary magic-byte
 * signatures only need their own length. */
#define HL_MIME_SNIFF_WINDOW 4096

/* ── Binary magic-byte signatures (offset 0, exact match) ────────── */

typedef struct {
    const uint8_t *magic;
    size_t         magic_len;
    const char    *mime;
} HlMimeMagic;

static const uint8_t MAGIC_PNG[]   = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
static const uint8_t MAGIC_JPEG[]  = {0xFF, 0xD8, 0xFF};
static const uint8_t MAGIC_GIF87[] = {'G', 'I', 'F', '8', '7', 'a'};
static const uint8_t MAGIC_GIF89[] = {'G', 'I', 'F', '8', '9', 'a'};
static const uint8_t MAGIC_PDF[]   = {'%', 'P', 'D', 'F', '-'};

static const HlMimeMagic SIGNATURES[] = {
    { MAGIC_PNG,   sizeof(MAGIC_PNG),   "image/png"       },
    { MAGIC_JPEG,  sizeof(MAGIC_JPEG),  "image/jpeg"      },
    { MAGIC_GIF87, sizeof(MAGIC_GIF87), "image/gif"       },
    { MAGIC_GIF89, sizeof(MAGIC_GIF89), "image/gif"       },
    { MAGIC_PDF,   sizeof(MAGIC_PDF),   "application/pdf" },
};

/* WebP: split magic - "RIFF" at offset 0, "WEBP" at offset 8, with a
 * 4-byte length field in between. Needs 12 bytes minimum to identify. */
static int is_webp(const uint8_t *buf, size_t len)
{
    return len >= 12 &&
           memcmp(buf,     "RIFF", 4) == 0 &&
           memcmp(buf + 8, "WEBP", 4) == 0;
}

/* ── Text-shape heuristics ────────────────────────────────────────── */

/* ASCII-only case-insensitive comparison - we don't want locale or
 * UTF-8 case folding inside MIME sniffing. */
static int ci_starts_with(const uint8_t *buf, size_t buf_len,
                            const char *prefix)
{
    size_t plen = strlen(prefix);
    if (buf_len < plen) return 0;
    for (size_t i = 0; i < plen; i++) {
        uint8_t b = buf[i];
        char p = prefix[i];
        if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + 32);
        if (p >= 'A' && p <= 'Z') p = (char)(p + 32);
        if ((char)b != p) return 0;
    }
    return 1;
}

/* Skip ASCII whitespace (space, tab, CR, LF, FF). Returns the offset
 * of the first non-whitespace byte, or `len` if all bytes are
 * whitespace. */
static size_t skip_whitespace(const uint8_t *buf, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t b = buf[i];
        if (b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == '\f')
            i++;
        else
            break;
    }
    return i;
}

/* Locate a literal substring within the first `window` bytes of `buf`,
 * case-insensitively. Returns 1 if found, 0 otherwise. Avoids
 * allocating a temporary lowercase copy - does in-place comparison. */
static int contains_ci(const uint8_t *buf, size_t len, const char *needle,
                         size_t window)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return 0;
    size_t cap = len < window ? len : window;
    if (cap < nlen) return 0;
    for (size_t i = 0; i + nlen <= cap; i++) {
        if (ci_starts_with(buf + i, cap - i, needle))
            return 1;
    }
    return 0;
}

/* SVG: literal "<svg" within the first 4 KiB, optionally preceded by
 * a "<?xml ... ?>" declaration and whitespace. Case-insensitive. */
static int looks_like_svg(const uint8_t *buf, size_t len)
{
    if (!len) return 0;
    size_t off = skip_whitespace(buf, len);
    if (off >= len) return 0;

    /* Direct match: bytes start with "<svg" */
    if (ci_starts_with(buf + off, len - off, "<svg"))
        return 1;

    /* Indirect match: starts with "<?xml" and "<svg" appears nearby */
    if (ci_starts_with(buf + off, len - off, "<?xml"))
        return contains_ci(buf, len, "<svg", HL_MIME_SNIFF_WINDOW);

    return 0;
}

/* HTML: common openers, case-insensitive, with leading-whitespace
 * tolerance. Mirrors WhatWG sniffing's "HTML signatures" subset. */
static int looks_like_html(const uint8_t *buf, size_t len)
{
    if (!len) return 0;
    size_t off = skip_whitespace(buf, len);
    if (off >= len) return 0;
    const uint8_t *p = buf + off;
    size_t plen = len - off;

    static const char *const HTML_PREFIXES[] = {
        "<!doctype html",
        "<html",
        "<head",
        "<body",
        "<script",
        "<title",
        "<table",
        "<div",
        "<p>",
        "<p ",
        "<!--",
        NULL,
    };
    for (size_t i = 0; HTML_PREFIXES[i]; i++) {
        if (ci_starts_with(p, plen, HTML_PREFIXES[i]))
            return 1;
    }
    return 0;
}

/* ── UTF-8 plain-text fallback ────────────────────────────────────── */

/* Validate that `buf[0..min(len, 4096))` is a valid UTF-8 byte sequence
 * with no NUL byte. Truncated trailing multibyte sequences at the very
 * end of the inspected window are tolerated (treat-as-valid) - sniffing
 * shouldn't penalize a buffer for being too short.
 *
 * Rejects:
 *   - NUL byte
 *   - Overlong encodings (e.g. 0xC0 0x80 representing NUL)
 *   - UTF-16 surrogates encoded as UTF-8 (U+D800..U+DFFF)
 *   - Code points above U+10FFFF
 *   - Invalid continuation bytes
 *   - Stray continuation bytes / unexpected high bytes
 */
static int is_plain_text(const uint8_t *buf, size_t len)
{
    if (len == 0) return 0;
    size_t cap = len < HL_MIME_SNIFF_WINDOW ? len : HL_MIME_SNIFF_WINDOW;
    size_t i = 0;
    while (i < cap) {
        uint8_t b = buf[i];

        if (b == 0x00) return 0;            /* NUL - never text */

        if (b < 0x80) {                     /* 1-byte ASCII */
            i++;
            continue;
        }

        /* Determine how many continuation bytes this lead expects */
        int extra;
        uint32_t cp;
        uint8_t  min_lead, max_lead;
        if ((b & 0xE0) == 0xC0) {           /* 2-byte: 110xxxxx */
            extra = 1;
            cp = b & 0x1F;
            min_lead = 0xC2;                /* C0/C1 → overlong */
            max_lead = 0xDF;
        } else if ((b & 0xF0) == 0xE0) {    /* 3-byte: 1110xxxx */
            extra = 2;
            cp = b & 0x0F;
            min_lead = 0xE0;
            max_lead = 0xEF;
        } else if ((b & 0xF8) == 0xF0) {    /* 4-byte: 11110xxx */
            extra = 3;
            cp = b & 0x07;
            min_lead = 0xF0;
            max_lead = 0xF4;                /* > F4 → > U+10FFFF */
        } else {
            return 0;                       /* invalid lead byte */
        }
        if (b < min_lead || b > max_lead) return 0;

        /* Tolerate truncation at the very end of the window - partial
         * multibyte sequence is fine for sniffing. */
        if (i + (size_t)extra >= cap) {
            break;
        }

        for (int k = 1; k <= extra; k++) {
            uint8_t c = buf[i + (size_t)k];
            if ((c & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (c & 0x3FU);
        }

        /* Overlong-encoding rejection by code-point range */
        if (extra == 1 && cp < 0x80)    return 0;     /* C0..C1 caught above */
        if (extra == 2 && cp < 0x800)   return 0;
        if (extra == 3 && cp < 0x10000) return 0;
        if (cp > 0x10FFFF)              return 0;     /* outside Unicode */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;   /* UTF-16 surrogate */

        i += (size_t)(extra + 1);
    }
    return 1;
}

/* ── Public entry point ───────────────────────────────────────────── */

const char *hl_cap_mime_sniff(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return NULL;

    /* 1. Binary magic-byte signatures (highest priority - unambiguous). */
    for (size_t i = 0; i < sizeof(SIGNATURES) / sizeof(SIGNATURES[0]); i++) {
        if (len >= SIGNATURES[i].magic_len &&
            memcmp(buf, SIGNATURES[i].magic, SIGNATURES[i].magic_len) == 0) {
            return SIGNATURES[i].mime;
        }
    }
    if (is_webp(buf, len)) return "image/webp";

    /* 2. Text-shape heuristics. SVG before HTML - some SVGs lack the
     *    XML declaration and start directly with "<svg", which we must
     *    not classify as HTML by accident. */
    if (looks_like_svg(buf, len))  return "image/svg+xml";
    if (looks_like_html(buf, len)) return "text/html";

    /* 3. UTF-8 plain-text fallback. JSON / CSV / config files all land
     *    here - they're valid text by construction. */
    if (is_plain_text(buf, len)) return "text/plain";

    return NULL;
}
