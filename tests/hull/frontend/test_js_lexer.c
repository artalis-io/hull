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

/* count non-overlapping occurrences of `needle` in `hay`. */
static int count(const char *hay, const char *needle) {
    int c = 0; size_t nl = strlen(needle);
    if (!hay || nl == 0) return 0;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += nl) c++;
    return c;
}

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
    EXPECT_TRUE(has(o, "\"kind\":\"line\""));   /* the comment is collected, with its text */
    EXPECT_TRUE(has(o, "\"text\":\" trailing\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Comments are returned as a collection with exact ranges, raw/text, and kind. */
UTEST(js_lexer, comment_collection)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = lex_str(s, "// line\n/* block */\n/** jsdoc */\nx");
    EXPECT_TRUE(has(o, "\"kind\":\"line\",\"start\":1,\"stop\":8,\"raw\":\"// line\",\"text\":\" line\""));
    EXPECT_TRUE(has(o, "\"kind\":\"block\""));
    EXPECT_TRUE(has(o, "\"kind\":\"jsdoc\",\"start\":21,\"stop\":33"));   // the /** ... */ jsdoc block
    EXPECT_TRUE(has(o, "\"text\":\"* jsdoc \""));   // raw inner content (leading * kept; Slice 3 strips it)
    // an empty jsdoc-looking block is a plain block, not jsdoc
    free(o); o = NULL;
    o = lex_str(s, "/**/x");
    EXPECT_TRUE(has(o, "\"kind\":\"block\",\"start\":1,\"stop\":5"));
    EXPECT_FALSE(has(o, "\"kind\":\"jsdoc\""));
    free(o); o = NULL;
    /* CRLF inside a block comment + a trailing (unterminated) comment */
    o = lex_str(s, "a /*x\r\ny*/ b /* nope");
    EXPECT_TRUE(has(o, "\"kind\":\"block\""));
    EXPECT_TRUE(has(o, "unterminated block comment"));
    free(o);
    hl_js_session_destroy(s);
}

/* Regex-vs-division must be correct for supported control flow (structural context model). */
UTEST(js_lexer, regex_vs_division_control_flow)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* regex after a control-flow `)` */
    char *o = lex_str(s, "if (ok) /x/.test(s);");
    EXPECT_TRUE(has(o, "\"type\":\"regex\",\"pattern\":\"x\""));
    free(o); o = NULL;
    /* division after a call `)` */
    o = lex_str(s, "f(a) / b");
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"/\""));
    EXPECT_FALSE(has(o, "\"type\":\"regex\""));
    free(o); o = NULL;
    /* division after an object-literal `}` value */
    o = lex_str(s, "const x = {}; x / y;");
    EXPECT_TRUE(has(o, "\"type\":\"punctuator\",\"value\":\"/\""));
    EXPECT_FALSE(has(o, "\"type\":\"regex\""));
    free(o); o = NULL;
    /* regex after a block `}` (statement position) */
    o = lex_str(s, "function f(){} /re/.test(x)");
    EXPECT_TRUE(has(o, "\"type\":\"regex\",\"pattern\":\"re\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Identifier start/continue validation, including escaped code points. */
UTEST(js_lexer, identifier_validation)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* abc == abc (valid) */
    char *o = lex_str(s, "\\u0061bc");
    EXPECT_TRUE(has(o, "\"type\":\"identifier\",\"value\":\"abc\",\"escaped\":true"));
    EXPECT_FALSE(has(o, "js.syntax"));
    free(o); o = NULL;
    /* 0abc: escaped start is '0' (a digit) -> invalid identifier start */
    o = lex_str(s, "\\u0030abc");
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_TRUE(has(o, "invalid escaped identifier start"));
    free(o); o = NULL;
    /* café (U+00E9 is a valid ID_Continue) -> clean identifier */
    const uint8_t cafe[] = { 'c','a','f',0xc3,0xa9 };
    o = lex_json(s, cafe, sizeof(cafe));
    EXPECT_TRUE(has(o, "\"type\":\"identifier\""));
    EXPECT_FALSE(has(o, "js.syntax"));
    free(o); o = NULL;
    /* an emoji (U+1F600) is NOT a valid identifier char -> unexpected character */
    const uint8_t emoji[] = { 0xf0, 0x9f, 0x98, 0x80 };
    o = lex_json(s, emoji, sizeof(emoji));
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_TRUE(has(o, "unexpected character"));
    EXPECT_FALSE(has(o, "\"type\":\"identifier\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Numeric separator validation matrix. */
UTEST(js_lexer, numeric_separators)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* valid: 1_000, 0xFF_FF */
    char *o = lex_str(s, "1_000 + 0xFF_FF");
    EXPECT_FALSE(has(o, "js.syntax"));
    free(o); o = NULL;
    o = lex_str(s, "1__2");   EXPECT_TRUE(has(o, "js.syntax")); free(o); o = NULL;   /* doubled */
    o = lex_str(s, "1_");     EXPECT_TRUE(has(o, "js.syntax")); free(o); o = NULL;   /* trailing */
    o = lex_str(s, "0x_FF");  EXPECT_TRUE(has(o, "js.syntax")); free(o); o = NULL;   /* leading in hex */
    o = lex_str(s, "1_.5");   EXPECT_TRUE(has(o, "js.syntax")); free(o); o = NULL;   /* before '.' */
    o = lex_str(s, "1e_5");   EXPECT_TRUE(has(o, "js.syntax")); free(o); o = NULL;   /* after 'e' */
    hl_js_session_destroy(s);
}

/* U+2028 / U+2029 terminate a regex literal (unterminated). */
UTEST(js_lexer, regex_line_separators)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* x = /ab<U+2028>c/  -> the LS terminates the regex */
    const uint8_t src[] = { 'x',' ','=',' ','/','a','b', 0xe2,0x80,0xa8, 'c','/' };
    char *o = lex_json(s, src, sizeof(src));
    EXPECT_TRUE(has(o, "unterminated regular expression"));
    free(o);
    hl_js_session_destroy(s);
}

/* maxDiagnostics caps ordinary diagnostics but retains terminal js.limit.* diagnostics. */
UTEST(js_lexer, diagnostic_budget)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    /* many stray '@' chars, each an ordinary js.syntax, capped at maxDiagnostics=2 */
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "lex",
                                   (const uint8_t *)"@@@@@@@@", 8, "t.js",
                                   "{\"maxDiagnostics\":2}", 20, &out, &out_len);
    (void)rc;
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.diagnostics\""));   /* terminal cap diagnostic retained */
    free(out);
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

/* Parser-directed slash: the parser controls regex-vs-division for braces the token stream
 * alone cannot disambiguate. Driven through hull:source:lexDirected with a forced goal. */
UTEST(js_lexer, parser_directed_slash)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    /* `if (ok) {} /re/.test(s)` : after a statement block, the parser allows a regex. */
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "lexDirected",
                                   (const uint8_t *)"if (ok) {} /re/.test(s)", 23, "t.js",
                                   "{\"forceRegex\":true}", 19, &out, &out_len);
    (void)rc;
    EXPECT_TRUE(has(out, "\"type\":\"regex\",\"pattern\":\"re\""));
    free(out); out = NULL;
    /* `const f = function() {} / 2` : after a function EXPRESSION, `/` is division. */
    rc = hl_js_session_analyze(s, "hull:source:lextest", "lexDirected",
                               (const uint8_t *)"const f = function() {} / 2", 27, "t.js",
                               "{\"forceRegex\":false}", 20, &out, &out_len);
    EXPECT_TRUE(has(out, "\"type\":\"punctuator\",\"value\":\"/\""));
    EXPECT_FALSE(has(out, "\"type\":\"regex\""));
    free(out);
    hl_js_session_destroy(s);
}

/* Invalid BigInt + legacy-leading-zero numeric forms. */
UTEST(js_lexer, bigint_and_legacy_numeric)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* valid BigInts */
    char *o = lex_str(s, "10n + 0xFFn");
    EXPECT_FALSE(has(o, "js.syntax"));
    free(o); o = NULL;
    o = lex_str(s, "1.0n");   EXPECT_TRUE(has(o, "js.syntax")); EXPECT_TRUE(has(o, "invalid BigInt")); free(o); o = NULL;
    o = lex_str(s, ".5n");    EXPECT_TRUE(has(o, "js.syntax")); EXPECT_TRUE(has(o, "invalid BigInt")); free(o); o = NULL;
    o = lex_str(s, "1e5n");   EXPECT_TRUE(has(o, "js.syntax")); free(o); o = NULL;   /* exponent + n */
    o = lex_str(s, "0_1");    EXPECT_TRUE(has(o, "js.syntax")); EXPECT_TRUE(has(o, "legacy octal")); free(o); o = NULL;
    hl_js_session_destroy(s);
}

/* Invalid UTF-8 immediately after a backslash escape, in a string and in a template. */
UTEST(js_lexer, escaped_invalid_utf8)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* "a\<0xFF>" : the byte after the backslash is invalid UTF-8 */
    const uint8_t str[] = { 0x22, 'a', 0x5c, 0xff, 0x22 };
    char *o = lex_json(s, str, sizeof(str));
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_TRUE(has(o, "invalid UTF-8 in escape"));
    free(o); o = NULL;
    /* `a\<0xFF>` : same, inside a template */
    const uint8_t tpl[] = { 0x60, 'a', 0x5c, 0xff, 0x60 };
    o = lex_json(s, tpl, sizeof(tpl));
    EXPECT_TRUE(has(o, "invalid UTF-8 in escape"));
    free(o);
    hl_js_session_destroy(s);
}

/* maxDiagnostics=0: retain NO ordinary diagnostics, but the terminal js.limit.diagnostics
 * stays visible. Assert exact diagnostic counts, not just presence. */
UTEST(js_lexer, diagnostic_budget_zero)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    /* 4 stray '@' -> would be 4 ordinary js.syntax; with budget 0, only the terminal remains */
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "lex",
                                   (const uint8_t *)"@@@@", 4, "t.js",
                                   "{\"maxDiagnostics\":0}", 20, &out, &out_len);
    (void)rc;
    EXPECT_EQ(count(out, "\"code\":"), 1);                       /* exactly one diagnostic */
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.diagnostics\""));  /* and it is the terminal */
    free(out); out = NULL;
    /* budget 2 over 5 stray chars -> 2 ordinary + 1 terminal = 3 diagnostics */
    rc = hl_js_session_analyze(s, "hull:source:lextest", "lex",
                               (const uint8_t *)"@@@@@", 5, "t.js",
                               "{\"maxDiagnostics\":2}", 20, &out, &out_len);
    EXPECT_EQ(count(out, "\"code\":"), 3);
    EXPECT_EQ(count(out, "\"code\":\"js.syntax\""), 2);
    EXPECT_EQ(count(out, "\"code\":\"js.limit.diagnostics\""), 1);
    free(out);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
