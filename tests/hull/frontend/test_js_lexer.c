/*
 * test_js_lexer.c - Slice 2: the byte-oriented ECMAScript lexer (hull:source:lexer),
 * driven through the restricted tooling session via hull:source:lextest.
 *
 * Covers the token contract the parser depends on: exact 1-based byte ranges, UTF-8 /
 * astral handling, token shapes, string/template escapes, template modes, regex-vs-division,
 * ASI nlBefore metadata, comments, numeric forms, punctuator maximal munch, diagnostics +
 * recovery, malformed UTF-8, and the token limit.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>

static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

/* Lex `src` (len bytes) and return the malloc'd JSON result (caller frees). */
static char *lex_json(HlJsSession *s, const uint8_t *src, size_t len)
{
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "lex", src, len, "t.js",
                                   NULL, 0, &out, &out_len);
    (void)rc;
    return out;
}
static char *lex_str(HlJsSession *s, const char *src) { return lex_json(s, (const uint8_t *)src, strlen(src)); }

UTEST(js_lexer, basic_tokens_and_ranges)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "const x = 1;");
    ASSERT_TRUE(o != NULL);
    EXPECT_TRUE(has(o, "\"status\":\"ok\""));
    EXPECT_TRUE(has(o, "\"type\":\"identifier\",\"value\":\"const\""));
    EXPECT_TRUE(has(o, "\"value\":\"const\",\"escaped\":false,\"start\":1,\"stop\":6"));  /* [1,6) = "const" */
    EXPECT_TRUE(has(o, "\"type\":\"identifier\",\"value\":\"x\""));
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"=\""));
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\"1\""));
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\";\""));
    EXPECT_TRUE(has(o, "\"type\":\"eof\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, exact_byte_ranges)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "ab cd");   /* ab=[1,3) cd=[4,6) */
    EXPECT_TRUE(has(o, "\"value\":\"ab\",\"escaped\":false,\"start\":1,\"stop\":3"));
    EXPECT_TRUE(has(o, "\"value\":\"cd\",\"escaped\":false,\"start\":4,\"stop\":6"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, utf8_identifier_spans_bytes)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* café : c a f é(0xC3 0xA9) -> 5 bytes, identifier [1,6) */
    const uint8_t src[] = { 'c','a','f',0xc3,0xa9 };
    char *o = lex_json(s, src, sizeof(src));
    EXPECT_TRUE(has(o, "\"type\":\"identifier\""));
    EXPECT_TRUE(has(o, "\"start\":1,\"stop\":6"));   /* range spans all 5 bytes */
    EXPECT_FALSE(has(o, "js.syntax"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, astral_char_in_string_spans_four_bytes)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* "😀" : 0x22 F0 9F 98 80 0x22 -> string [1,7) */
    const uint8_t src[] = { 0x22, 0xf0, 0x9f, 0x98, 0x80, 0x22 };
    char *o = lex_json(s, src, sizeof(src));
    EXPECT_TRUE(has(o, "\"type\":\"string\""));
    EXPECT_TRUE(has(o, "\"start\":1,\"stop\":7"));   /* all 4 astral bytes + 2 quotes */
    EXPECT_FALSE(has(o, "js.syntax"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, string_escapes_cooked)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "\"a\\n\\t\\x41\\u0042\"");   /* "a\n\t\x41B" -> a<LF><TAB>AB */
    EXPECT_TRUE(has(o, "\"type\":\"string\""));
    EXPECT_TRUE(has(o, "\"value\":\"a\\n\\tAB\""));   /* cooked, JSON-escaped */
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, template_modes)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "`a${x}b${y}c`");
    EXPECT_TRUE(has(o, "\"type\":\"template\",\"mode\":\"head\",\"value\":\"a\""));
    EXPECT_TRUE(has(o, "\"type\":\"identifier\",\"value\":\"x\""));
    EXPECT_TRUE(has(o, "\"type\":\"template\",\"mode\":\"middle\",\"value\":\"b\""));
    EXPECT_TRUE(has(o, "\"type\":\"identifier\",\"value\":\"y\""));
    EXPECT_TRUE(has(o, "\"type\":\"template\",\"mode\":\"tail\",\"value\":\"c\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, template_no_substitution_and_nested_braces)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "`hello`");
    EXPECT_TRUE(has(o, "\"mode\":\"noSubstitution\",\"value\":\"hello\""));
    free(o); o = NULL;
    /* a block object inside a substitution: the inner } must NOT close the template */
    o = lex_str(s, "`x${ {a:1} }y`");
    EXPECT_TRUE(has(o, "\"mode\":\"head\",\"value\":\"x\""));
    EXPECT_TRUE(has(o, "\"mode\":\"tail\",\"value\":\"y\""));
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"{\""));
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"}\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, regex_vs_division)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* division: value / value */
    char *o = lex_str(s, "a / b");
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"/\""));
    EXPECT_FALSE(has(o, "\"type\":\"regex\""));
    free(o); o = NULL;
    /* regex after '=' */
    o = lex_str(s, "x = /ab+c/gi");
    EXPECT_TRUE(has(o, "\"type\":\"regex\",\"pattern\":\"ab+c\",\"flags\":\"gi\""));
    free(o); o = NULL;
    /* regex after 'return' */
    o = lex_str(s, "return /x/;");
    EXPECT_TRUE(has(o, "\"type\":\"regex\",\"pattern\":\"x\""));
    free(o); o = NULL;
    /* regex char class with a slash inside must not close early */
    o = lex_str(s, "y = /[/]/");
    EXPECT_TRUE(has(o, "\"type\":\"regex\",\"pattern\":\"[/]\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, asi_newline_metadata)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "a b");           /* same line */
    EXPECT_TRUE(has(o, "\"value\":\"b\",\"escaped\":false,\"start\":3,\"stop\":4,\"nlBefore\":false"));
    free(o); o = NULL;
    o = lex_str(s, "a\nb");                 /* newline between */
    EXPECT_TRUE(has(o, "\"value\":\"b\",\"escaped\":false,\"start\":3,\"stop\":4,\"nlBefore\":true"));
    free(o); o = NULL;
    o = lex_str(s, "a\r\nb");               /* CRLF -> one break, b at byte 4 */
    EXPECT_TRUE(has(o, "\"value\":\"b\",\"escaped\":false,\"start\":4,\"stop\":5,\"nlBefore\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, comments_and_asi)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "a /* c */ b");   /* single-line block comment: no newline; b at byte 11 */
    EXPECT_TRUE(has(o, "\"value\":\"b\""));
    EXPECT_TRUE(has(o, "\"value\":\"b\",\"escaped\":false,\"start\":11,\"stop\":12,\"nlBefore\":false"));
    free(o); o = NULL;
    o = lex_str(s, "a /*\n*/ b");           /* block comment spanning a newline -> nlBefore true */
    EXPECT_TRUE(has(o, "\"value\":\"b\",\"escaped\":false"));
    EXPECT_TRUE(has(o, ",\"nlBefore\":true"));
    free(o); o = NULL;
    o = lex_str(s, "a // trailing\nb");     /* line comment */
    EXPECT_TRUE(has(o, "\"value\":\"b\",\"escaped\":false"));
    EXPECT_TRUE(has(o, ",\"nlBefore\":true"));
    EXPECT_FALSE(has(o, "trailing"));       /* comment text is not a token */
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, numeric_forms)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "0xFF 0b1010 0o17 1.5e3 .25 42n");
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\"0xFF\""));
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\"0b1010\""));
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\"0o17\""));
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\"1.5e3\""));
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\".25\""));
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\"42n\",\"bigint\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, punctuator_maximal_munch)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "a >>>= b === c ?? d => e ... f");
    EXPECT_TRUE(has(o, "\"value\":\">>>=\""));
    EXPECT_TRUE(has(o, "\"value\":\"===\""));
    EXPECT_TRUE(has(o, "\"value\":\"??\""));
    EXPECT_TRUE(has(o, "\"value\":\"=>\""));
    EXPECT_TRUE(has(o, "\"value\":\"...\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, optional_chaining_vs_conditional_digit)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "a?.b");
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"?.\""));
    free(o); o = NULL;
    /* `a?.5:b` -> `?` then `.5` (number) then `:` : ?. must NOT form before a digit */
    o = lex_str(s, "a?.5:b");
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"?\""));
    EXPECT_TRUE(has(o, "\"type\":\"number\",\"value\":\".5\""));
    EXPECT_FALSE(has(o, "\"value\":\"?.\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, malformed_utf8_diagnostic)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const uint8_t src[] = { 'a', 0xff, 'b' };   /* lone 0xFF is invalid UTF-8 */
    char *o = lex_json(s, src, sizeof(src));
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_TRUE(has(o, "invalid UTF-8"));
    /* recovery: still lexes a and b as identifiers */
    EXPECT_TRUE(has(o, "\"value\":\"a\""));
    EXPECT_TRUE(has(o, "\"value\":\"b\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, embedded_nul_does_not_truncate)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const uint8_t src[] = { 'a', 0x00, 'b' };   /* NUL is an unexpected char, not a terminator */
    char *o = lex_json(s, src, sizeof(src));
    EXPECT_TRUE(has(o, "\"value\":\"a\""));
    EXPECT_TRUE(has(o, "\"value\":\"b\""));      /* scanning continued past the NUL */
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, unterminated_string_recovers)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "\"abc\ndef");
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_TRUE(has(o, "unterminated string"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, unterminated_block_comment_and_regex)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "a /* nope");
    EXPECT_TRUE(has(o, "unterminated block comment"));
    free(o); o = NULL;
    o = lex_str(s, "x = /ab\n");
    EXPECT_TRUE(has(o, "unterminated regular expression"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, token_limit)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    /* maxTokens=3 via options -> js.limit.tokens */
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "lex",
                                   (const uint8_t *)"a b c d e f", 11, "t.js",
                                   "{\"maxTokens\":3}", 15, &out, &out_len);
    (void)rc;
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.tokens\""));
    free(out);
    hl_js_session_destroy(s);
}

UTEST(js_lexer, imports_and_hull_scheme_strings)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* hull:-scheme specifiers are ordinary string literals to the lexer */
    char *o = lex_str(s, "import { db } from \"hull:db\";");
    EXPECT_TRUE(has(o, "\"type\":\"identifier\",\"value\":\"import\""));
    EXPECT_TRUE(has(o, "\"type\":\"string\",\"value\":\"hull:db\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
