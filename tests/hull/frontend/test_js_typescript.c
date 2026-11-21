/*
 * test_js_typescript.c - the JS frontend's TypeScript mode, first slice
 * (docs/typescript_design.md sections 3 and 5): parse(bytes, { lang: "ts" }), the
 * lexer's splitGreater(), and hull:source:ts_erase.
 *
 * This slice adds the mode and its hook points; every TypeScript construct is refused with a
 * ts.unsupported diagnostic and JS mode goes on as before. The suite holds three things:
 *   - valid JavaScript parses the same in TS mode: over the frozen golden inputs and the repo's
 *     own JS, every file that is valid JS gives a TS-mode parse byte-identical to the JS-mode
 *     one, with no refusal and no type span (no hook fires on JavaScript, no TypeScript peek
 *     changes a token);
 *   - each hook refuses its construct, at the right range, and JS uses of the same words
 *     (`import type from "m"`, `a ? (b) : c`, `case (x):`) stay JS;
 *   - the erasure byte rules: span bytes become spaces, CR / LF are kept, U+2028 / U+2029 and
 *     malformed span lists are refused;
 *   - the corpus (tests/fixtures/ts_frontend), docs/typescript_design.md section 5:
 *       accept/NAME.ts parses cleanly in TS mode and its erasure keeps every byte rule (M1),
 *       parses as JavaScript to exactly the TS-mode AST (M2), compiles in QuickJS as a module
 *       (M3, compile only) and equals accept/NAME.js byte for byte (M4);
 *       refuse/NAME.ts is invalid, and its refusals equal refuse/NAME.expect.
 *     A change that is MEANT: regenerate the .js / .expect files with HULL_TS_CORPUS_UPDATE=1
 *     and review the diff.
 * JS mode itself is held by test_js_golden and scripts/js_frontend_diff.sh.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "js_frontend_dump.h"
#include "quickjs.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }
static int count(const char *hay, const char *needle)
{
    int c = 0; size_t nl = strlen(needle);
    if (!hay || nl == 0) return 0;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += nl) c++;
    return c;
}

/* Run one lextest method; returns the malloc'd JSON (caller frees). */
static char *run(HlJsSession *s, const char *method, const char *src, size_t len,
                 const char *path, const char *opts, size_t *out_len)
{
    char *out = NULL; size_t n = 0;
    hl_js_session_analyze(s, "hull:source:lextest", method, (const uint8_t *)src, len, path,
                          opts, opts ? strlen(opts) : 0, &out, &n);
    if (out_len) *out_len = out ? n : 0;
    return out;
}
static char *ts(HlJsSession *s, const char *src) { return run(s, "parseTs", src, strlen(src), "t.ts", NULL, NULL); }

/* ---- TS mode adds refusals and nothing else ---- */

static const char *const CORPUS[] = {
    "tests/fixtures/test262/cases", "tests/fixtures/js_frontend_golden/cases",
    "stdlib/js", "stdlib/cli/js", "examples", NULL,
};
static const char *const CORPUS_EXT[] = { ".js", ".mjs", ".src", NULL };

UTEST(js_typescript, valid_js_parses_the_same_in_ts_mode)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    JfdPaths pv = {0};
    for (int r = 0; CORPUS[r]; r++) ASSERT_EQ(0, jfd_walk(CORPUS[r], CORPUS_EXT, &pv));
    jfd_sort(&pv);
    int compared = 0, differ = 0;
    for (size_t i = 0; i < pv.len; i++) {
        size_t n = 0;
        char *src = jfd_read(pv.items[i], &n);
        ASSERT_TRUE(src != NULL);
        size_t jl = 0, tl = 0;
        char *js = run(s, "parse", src, n, pv.items[i], NULL, &jl);
        /* Invalid JS may be TypeScript (a Test262 negative, a ts_syntax_is_js_error case):
         * TS mode is free to read it differently. */
        if (!has(js, "\"valid\":true")) { free(js); free(src); continue; }
        char *tj = run(s, "parseTs", src, n, pv.items[i], "{\"asJs\":true}", &tl);
        if (!js || !tj || jl != tl || memcmp(js, tj, jl) != 0) {
            if (differ < 20) fprintf(stderr, "[ts-mode] TS mode changed the JS result: %s\n", pv.items[i]);
            differ++;
        }
        compared++;
        free(js); free(tj); free(src);
    }
    jfd_paths_free(&pv);
    EXPECT_EQ(0, differ);
    EXPECT_GE(compared, 650);                                  /* the corpus was walked */
    hl_js_session_destroy(s);
}

UTEST(js_typescript, typescript_is_off_by_default)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *src = "let x: number = 1;";
    /* JS mode, even for a .ts path: a JS syntax error, no TypeScript refusal, no typeSpans. */
    char *o = run(s, "parse", src, strlen(src), "app.ts", NULL, NULL);
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_FALSE(has(o, "\"code\":\"ts."));
    EXPECT_FALSE(has(o, "typeSpans"));
    free(o);
    o = ts(s, src);
    EXPECT_TRUE(has(o, "\"valid\":true,\"typeSpans\":[{\"start\":6,\"stop\":14}]"));
    free(o);
    hl_js_session_destroy(s);
}

/* lang: "js" is the default, "ts" turns the mode on, and anything else is refused - never a
 * silent JS parse. */
UTEST(js_typescript, lang_option)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *src = "let x: number = 1;";
    size_t n = strlen(src);
    char *dflt = run(s, "parse", src, n, "t.ts", NULL, NULL);
    char *js = run(s, "parse", src, n, "t.ts", "{\"lang\":\"js\"}", NULL);
    ASSERT_TRUE(dflt != NULL && js != NULL);
    EXPECT_STREQ(dflt, js);
    free(dflt); free(js);
    char *o = run(s, "parse", src, n, "t.ts", "{\"lang\":\"ts\"}", NULL);
    EXPECT_TRUE(has(o, "\"valid\":true"));                  /* the annotation is TypeScript */
    EXPECT_FALSE(has(o, "\"code\":"));
    free(o);
    static const char *const BAD[] = {
        "{\"lang\":\"tsx\"}", "{\"lang\":\"jsx\"}", "{\"lang\":\"TS\"}", "{\"lang\":\"typescript\"}",
        "{\"lang\":\"\"}", "{\"lang\":1}", "{\"lang\":true}", "{\"lang\":null}",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        o = run(s, "parse", src, n, "t.ts", BAD[i], NULL);
        if (!has(o, "\"code\":\"js.internal\",\"message\":\"unknown lang "))
            fprintf(stderr, "[ts-mode] %s\n  got %s\n", BAD[i], o ? o : "(null)");
        EXPECT_TRUE(has(o, "\"code\":\"js.internal\",\"message\":\"unknown lang "));
        EXPECT_EQ(1, count(o, "\"severity\":"));                /* nothing parsed */
        EXPECT_TRUE(has(o, "\"body\":[]"));
        EXPECT_TRUE(has(o, "\"valid\":false"));
        free(o);
    }
    o = run(s, "parse", src, n, "t.ts", "{\"lang\":\"tsx\"}", NULL);
    EXPECT_TRUE(has(o, "unknown lang 'tsx' (expected \\\"js\\\" or \\\"ts\\\")"));
    free(o);
    hl_js_session_destroy(s);
}

/* ---- every hook refuses its construct ---- */

typedef struct { const char *src; const char *what; int start, stop; } Refusal;
static const Refusal REFUSALS[] = {
    { "function f(a, this: W) {}",             "'this' parameters after the first parameter", 15, 19 },
    { "class C { constructor(private x) {} }", "parameter properties",               23, 30 },
    { "if (x) function f(a);",                 "overload signatures",                21, 22 },
    { "const a = <T>x;",                       "angle-bracket assertions",           11, 12 },
    { "const b = <T>(x);",                     "angle-bracket assertions",           11, 12 },
    { "enum E { A }",                          "enums",                               1,  5 },
    { "namespace N {}",                        "namespaces",                          1, 10 },
    { "module M {}",                           "module declarations",                 1,  7 },
    { "import x = require(\"m\");",            "import-require declarations",        10, 11 },
    { "export = x;",                           "export assignments",                  8,  9 },
    { "declare x;",                            "'declare' declarations",              1,  8 },
    { "if (x) interface I {}",                 "declarations outside a statement list", 8, 17 },
};

UTEST(js_typescript, each_hook_refuses)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    for (size_t i = 0; i < sizeof(REFUSALS) / sizeof(REFUSALS[0]); i++) {
        const Refusal *r = &REFUSALS[i];
        char *o = ts(s, r->src);
        char want[256];
        snprintf(want, sizeof(want),
                 "\"code\":\"ts.unsupported\",\"message\":\"TypeScript %s are not supported\",\"path\":\"t.ts\",\"range\":{\"start\":%d,\"stop\":%d}",
                 r->what, r->start, r->stop);
        if (!has(o, want)) fprintf(stderr, "[ts-mode] %s\n  want %s\n  got  %s\n", r->src, want, o ? o : "(null)");
        EXPECT_TRUE(has(o, want));
        EXPECT_TRUE(has(o, "\"valid\":false"));
        free(o);
    }
    hl_js_session_destroy(s);
}

/* A construct is refused once, even when JS recovery comes back to its token. */
UTEST(js_typescript, one_refusal_per_position)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = ts(s, "const a = <T>x;");
    EXPECT_EQ(1, count(o, "\"code\":\"ts.unsupported\""));
    free(o);
    hl_js_session_destroy(s);
}

/* A refused declaration is read whole: its body is not parsed as JavaScript, so the
 * refusal is the only diagnostic, and the declaration after it is still read. */
static const char *const REFUSED_WHOLE[] = {
    "enum E { A, B = 2 }",
    "const enum K { B }",
    "export enum E { A }",
    "export const enum E { A }",
    "namespace N { export const a = 1; }",
    "namespace N.M { const r = /}/; }",
    "export namespace N {}",
    "module M { let x = 1; }",
    "module \"m\" {}",
    "if (x) interface I { a: 1 }",
    "if (x) type T = 1;",
};

UTEST(js_typescript, refused_declaration_read_whole)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    for (size_t i = 0; i < sizeof(REFUSED_WHOLE) / sizeof(REFUSED_WHOLE[0]); i++) {
        char src[256];
        snprintf(src, sizeof(src), "%s\nfunction after(a) { return a; }\n", REFUSED_WHOLE[i]);
        char *o = ts(s, src);
        int ok = count(o, "\"code\":\"ts.unsupported\"") == 1 && count(o, "\"code\":\"js.") == 0 &&
                 has(o, "\"name\":\"after\"");
        if (!ok) fprintf(stderr, "[ts-mode] %s\n  got %s\n", REFUSED_WHOLE[i], o ? o : "(null)");
        EXPECT_TRUE(ok);
        free(o);
    }
    hl_js_session_destroy(s);
}

/* The same words in JavaScript stay JavaScript in TS mode. */
static const char *const JS_KEEPS[] = {
    "import type from \"m\";",
    "import type, { a } from \"m\";",
    "import { type } from \"m\"; import { type as t2 } from \"n\";",
    "export { type }; export { type as t3 };",
    "let type = 1; type = 2; type\nx;",
    "let as = 1, satisfies = 2; as\nsatisfies;",
    "let interface_ = 1; declare\nx; abstract\nclass_;",
    "const r = a ? (b) : c;",
    "const r2 = a ? x + (b) : c;",
    "switch (k) { case (a): break; case (b) + 1: break; }",
    "x\n!y;",
    "a != b; a !== b;",
    "class C { public = 1; declare\nx; readonly() {} static private = 2 }",
    "function f(a, b = 1, ...c) { return a < b > c; }",
    "x >>= 1; y >>>= 2; z = a >> b >>> c >= d;",
    "for (let i = 0; i < n; i++) {} const t = a < b > c, u = a < b >= c, v = a << b >> c, w = a < b && c > d;",
    "const r = a < /x/.source; const q = a < b > -c; const p = a < b > +c; const o = a < (b) > [c];",
};

UTEST(js_typescript, js_uses_of_typescript_words_stay_js)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    for (size_t i = 0; i < sizeof(JS_KEEPS) / sizeof(JS_KEEPS[0]); i++) {
        const char *src = JS_KEEPS[i];
        char *j = run(s, "parse", src, strlen(src), "t.ts", NULL, NULL);
        char *o = ts(s, src);
        if (!has(j, "\"valid\":true") || has(o, "\"code\":\"ts."))
            fprintf(stderr, "[ts-mode] %s\n  js %s\n  ts %s\n", src, j ? j : "(null)", o ? o : "(null)");
        EXPECT_TRUE(has(j, "\"valid\":true"));                 /* it IS valid JS */
        EXPECT_FALSE(has(o, "\"code\":\"ts."));
        EXPECT_TRUE(has(o, "\"valid\":true"));
        free(j); free(o);
    }
    hl_js_session_destroy(s);
}

/* `<` speculation is bounded: input built so that every `<` is a long failed attempt (each
 * reads the nested rest of the file, a long tuple included, as type arguments) stops
 * speculating, and refuses, once the rewound tokens pass four times the kept ones plus 65536 -
 * instead of going quadratic. */
UTEST(js_typescript, speculation_budget)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* Shallow enough for a sanitizer build's stack, wide enough to pass the budget: each of the
     * 16 attempts rewinds the ~16000-token tuple. */
    const int depth = 16, terms = 8000;
    size_t cap = (size_t)depth * 8 + (size_t)terms * 4 + 16, n = 0;
    char *src = (char *)malloc(cap);
    ASSERT_TRUE(src != NULL);
    n += (size_t)snprintf(src + n, cap - n, "x = ");
    for (int i = 0; i < depth; i++) n += (size_t)snprintf(src + n, cap - n, "a < (");
    n += (size_t)snprintf(src + n, cap - n, "[");
    for (int i = 0; i < terms; i++) n += (size_t)snprintf(src + n, cap - n, i ? ", b" : "b");
    n += (size_t)snprintf(src + n, cap - n, "]");
    for (int i = 0; i < depth; i++) n += (size_t)snprintf(src + n, cap - n, ")");
    n += (size_t)snprintf(src + n, cap - n, ";");
    char *o = run(s, "parseTs", src, n, "t.ts", NULL, NULL);
    const char *dg = o ? strstr(o, "\"diagnostics\"") : NULL;
    if (!has(o, "lookahead budget")) fprintf(stderr, "[ts-mode] budget: %.400s\n", dg ? dg : (o ? o : "(null)"));
    EXPECT_TRUE(has(o, "TypeScript type lists past this file's lookahead budget are not supported"));
    EXPECT_TRUE(has(o, "\"valid\":false"));
    free(o);
    /* The same input as JavaScript parses cleanly. */
    o = run(s, "parse", src, n, "t.ts", NULL, NULL);
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    free(src);
    hl_js_session_destroy(s);
}

/* ---- the lexer's splitGreater ---- */

UTEST(js_typescript, split_greater)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* `>>>=` splits into `>` then `>>=` (split again: `>` then `>=`, then `>` `=`). */
    const char *src = "a>>>=b>>c>=d";
    char *o = run(s, "lexSplit", src, strlen(src), "t.ts", NULL, NULL);
    ASSERT_TRUE(o != NULL);
    for (int k = 2; k <= 4; k++) {
        char want[96];
        snprintf(want, sizeof(want), "\"value\":\">\",\"start\":%d,\"stop\":%d", k, k + 1);
        EXPECT_TRUE(has(o, want));
    }
    EXPECT_TRUE(has(o, "\"value\":\"=\",\"start\":5,\"stop\":6"));
    EXPECT_TRUE(has(o, "\"value\":\">\",\"start\":7,\"stop\":8"));
    EXPECT_TRUE(has(o, "\"value\":\">\",\"start\":8,\"stop\":9"));
    EXPECT_TRUE(has(o, "\"value\":\">\",\"start\":10,\"stop\":11"));
    EXPECT_TRUE(has(o, "\"value\":\"=\",\"start\":11,\"stop\":12"));
    EXPECT_FALSE(has(o, "\">>"));
    EXPECT_TRUE(has(o, "\"split_none\":true"));               /* eof is not split */
    EXPECT_FALSE(has(o, "\"code\":"));
    free(o);
    hl_js_session_destroy(s);
}

/* ---- erasure ---- */

static void hex_of(const char *s, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[(unsigned char)s[i] >> 4]; out[2 * i + 1] = H[(unsigned char)s[i] & 15]; }
    out[2 * n] = '\0';
}

/* eraseTs with injected spans: the result is `want` byte for byte. */
static int erases_to(HlJsSession *s, const char *src, const char *spans, const char *want)
{
    char opts[256]; snprintf(opts, sizeof(opts), "{\"spans\":%s}", spans);
    char *o = run(s, "eraseTs", src, strlen(src), "t.ts", opts, NULL);
    char hx[512], needle[560];
    hex_of(want, strlen(want), hx);
    snprintf(needle, sizeof(needle), "\"erased_hex\":\"%s\"", hx);
    int ok = has(o, "\"erased_ok\":true") && has(o, needle);
    if (!ok) fprintf(stderr, "[ts-erase] %s %s\n  got %s\n", src, spans, o ? o : "(null)");
    free(o);
    return ok;
}
static int erase_refused(HlJsSession *s, const char *src, const char *spans, const char *why)
{
    char opts[256]; snprintf(opts, sizeof(opts), "{\"spans\":%s}", spans);
    char *o = run(s, "eraseTs", src, strlen(src), "t.ts", opts, NULL);
    int ok = has(o, "\"erased_ok\":false") && has(o, "\"erased_hex\":null") && has(o, why);
    if (!ok) fprintf(stderr, "[ts-erase] %s %s\n  got %s\n", src, spans, o ? o : "(null)");
    free(o);
    return ok;
}

UTEST(js_typescript, erase_byte_rules)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(erases_to(s, "let x: number = 1;", "[{\"start\":6,\"stop\":14}]", "let x         = 1;"));
    EXPECT_TRUE(erases_to(s, "abc", "[]", "abc"));
    EXPECT_TRUE(erases_to(s, "abc", "[{\"start\":2,\"stop\":2}]", "abc"));          /* empty span */
    EXPECT_TRUE(erases_to(s, "abc", "[{\"start\":1,\"stop\":4}]", "   "));          /* to the end */
    EXPECT_TRUE(erases_to(s, "ab", "[{\"start\":1,\"stop\":2},{\"start\":2,\"stop\":3}]", "  "));   /* adjacent */
    /* CR and LF survive, so lines and columns do not move. */
    EXPECT_TRUE(erases_to(s, "a:\r\n T\nb", "[{\"start\":2,\"stop\":7}]", "a \r\n  \nb"));
    /* A multi-byte character becomes one space per byte (offsets kept). */
    EXPECT_TRUE(erases_to(s, "x: \xc3\xa9t\xe2\x82\xac;", "[{\"start\":2,\"stop\":10}]", "x        ;"));
    /* U+2028 / U+2029 are line terminators: refused, never blanked. */
    EXPECT_TRUE(erase_refused(s, "a:\xe2\x80\xa8T", "[{\"start\":2,\"stop\":7}]", "line separator"));
    EXPECT_TRUE(erase_refused(s, "a:\xe2\x80\xa9T", "[{\"start\":3,\"stop\":4}]", "line separator"));
    /* Malformed span lists. */
    EXPECT_TRUE(erase_refused(s, "abcd", "[{\"start\":3,\"stop\":4},{\"start\":1,\"stop\":2}]", "out of order"));
    EXPECT_TRUE(erase_refused(s, "abcd", "[{\"start\":1,\"stop\":3},{\"start\":2,\"stop\":4}]", "out of order"));
    EXPECT_TRUE(erase_refused(s, "abcd", "[{\"start\":0,\"stop\":2}]", "out of range"));
    EXPECT_TRUE(erase_refused(s, "abcd", "[{\"start\":2,\"stop\":6}]", "out of range"));
    EXPECT_TRUE(erase_refused(s, "abcd", "[{\"start\":3,\"stop\":2}]", "out of range"));
    EXPECT_TRUE(erase_refused(s, "abcd", "[{\"start\":1.5,\"stop\":2}]", "non-integer"));
    EXPECT_TRUE(erase_refused(s, "abcd", "[3]", "not an object"));
    EXPECT_TRUE(erase_refused(s, "abcd", "{}", "not an array"));
    hl_js_session_destroy(s);
}

/* typeErasure: the spans of the erased syntax, and every refusal. */
UTEST(js_typescript, type_erasure_reports_refusals)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *src = "let x: number = 1;\nenum E { A }\nconst a = <T>b;";
    char *o = run(s, "eraseTs", src, strlen(src), "t.ts", NULL, NULL);
    EXPECT_TRUE(has(o, "\"ok\":true,\"spans\":[{\"start\":6,\"stop\":14}],\"refusals\":[{"));
    /* Two refusals (enum, angle-bracket assertion), each in `refusals` and again in `diagnostics`. */
    EXPECT_EQ(4, count(o, "\"code\":\"ts.unsupported\""));
    EXPECT_TRUE(has(o, "TypeScript enums are not supported"));
    EXPECT_TRUE(has(o, "TypeScript angle-bracket assertions are not supported"));
    free(o);
    /* Plain JS through TS mode: nothing to erase, nothing refused, bytes unchanged. */
    const char *js = "const a = 1;\n";
    o = run(s, "eraseTs", js, strlen(js), "t.ts", NULL, NULL);
    char hx[64]; hex_of(js, strlen(js), hx);
    char needle[96]; snprintf(needle, sizeof(needle), "\"erased_hex\":\"%s\"", hx);
    EXPECT_TRUE(has(o, "\"spans\":[],\"refusals\":[],\"valid\":true"));
    EXPECT_TRUE(has(o, needle));
    free(o);
    hl_js_session_destroy(s);
}

/* ---- the corpus ---- */

#define TS_CORPUS "tests/fixtures/ts_frontend"
static const char *const EXT_TS[] = { ".ts", NULL };

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The JSON string value of `key` in `json`, unescaped (\n, \", \\ and \uXXXX below 0x80).
 * malloc'd; NULL when absent or not a string. */
static char *json_str(const char *json, const char *key)
{
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) return NULL;
    p += strlen(pat);
    size_t cap = strlen(p) + 1, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    for (; *p && *p != '"'; p++) {
        if (*p != '\\') { out[n++] = *p; continue; }
        p++;
        switch (*p) {
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case 'u': {
                int v = 0;
                for (int k = 1; k <= 4; k++) { int d = hex_digit(p[k]); if (d < 0) { free(out); return NULL; } v = v * 16 + d; }
                out[n++] = (char)v; p += 4; break;
            }
            default: out[n++] = *p; break;
        }
    }
    out[n] = '\0';
    return out;
}

/* Decode a lowercase hex string; returns the byte count, or (size_t)-1. By hand: scanf's
 * field width is not honoured alike everywhere (the cosmo build decoded every byte wrong). */
static size_t unhex(const char *hx, char *out)
{
    size_t n = strlen(hx);
    if (n % 2) return (size_t)-1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_digit(hx[2 * i]), lo = hex_digit(hx[2 * i + 1]);
        if (hi < 0 || lo < 0) return (size_t)-1;
        out[i] = (char)(hi * 16 + lo);
    }
    return n / 2;
}

static int write_file(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

/* path with its extension replaced by `ext` (".js", ".expect"); malloc'd. */
static char *sibling(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    size_t base = dot ? (size_t)(dot - path) : strlen(path);
    char *s = (char *)malloc(base + strlen(ext) + 1);
    if (!s) return NULL;
    memcpy(s, path, base); strcpy(s + base, ext);
    return s;
}

/* M3: QuickJS compiles the erased module (compile only - nothing runs). Imports resolve to a
 * stub module, so the verdict is about the syntax alone. */
static JSModuleDef *stub_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque;
    static const char *SYN = "export const a=1; export default 1;";
    JSValue v = JS_Eval(ctx, SYN, strlen(SYN), name, JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, v);
    return m;
}
static int quickjs_compiles(const char *src, size_t n, const char *name)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return 0;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 0; }
    JS_SetModuleLoaderFunc(rt, NULL, stub_loader, NULL);
    char *z = (char *)malloc(n + 1);                /* JS_Eval wants a NUL after the input */
    int ok = 0;
    if (z) {
        memcpy(z, src, n); z[n] = '\0';
        JSValue v = JS_Eval(ctx, z, n, name, JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE);
        ok = !JS_IsException(v);
        if (!ok) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, v);
        free(z);
    }
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return ok;
}

/* M1: the erased bytes are the source with some bytes turned into spaces, never CR / LF. */
static int byte_rules_hold(const char *src, const char *er, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (er[i] == src[i]) continue;
        if (er[i] != ' ' || src[i] == '\n' || src[i] == '\r') return 0;
    }
    return 1;
}

UTEST(js_typescript, corpus_accept)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const int update = getenv("HULL_TS_CORPUS_UPDATE") != NULL;
    JfdPaths pv = {0};
    ASSERT_EQ(0, jfd_walk(TS_CORPUS "/accept", EXT_TS, &pv));
    jfd_sort(&pv);
    int checked = 0;
    for (size_t i = 0; i < pv.len; i++) {
        const char *path = pv.items[i];
        size_t n = 0;
        char *src = jfd_read(path, &n);
        ASSERT_TRUE(src != NULL);
        char *o = run(s, "eraseTs", src, n, path, NULL, NULL);
        char *hx = json_str(o, "erased_hex");
        char *er = hx ? (char *)malloc(strlen(hx) / 2 + 1) : NULL;
        size_t en = (hx && er) ? unhex(hx, er) : (size_t)-1;
        int clean = has(o, "\"valid\":true") && has(o, "\"diagnostics\":[]") && has(o, "\"refusals\":[]");
        int m1 = en == n && byte_rules_hold(src, er, n);
        int m2 = has(o, "\"erased_valid\":true") && has(o, "\"ast_matches\":true");
        int m3 = m1 && quickjs_compiles(er, en, path);
        char *jsp = sibling(path, ".js");
        int m4 = 0;
        if (m1 && update) m4 = write_file(jsp, er, en) == 0;
        else if (m1) { size_t wn = 0; char *want = jfd_read(jsp, &wn); m4 = want && wn == en && memcmp(want, er, en) == 0; free(want); }
        if (!(clean && m1 && m2 && m3 && m4))
            fprintf(stderr, "[ts-corpus] %s: clean=%d M1=%d M2=%d M3=%d M4=%d\n  %s\n", path, clean, m1, m2, m3, m4, o ? o : "(null)");
        EXPECT_TRUE(clean && m1 && m2 && m3 && m4);
        checked++;
        free(jsp); free(er); free(hx); free(o); free(src);
    }
    jfd_paths_free(&pv);
    EXPECT_GE(checked, 22);
    hl_js_session_destroy(s);
}

UTEST(js_typescript, corpus_refuse)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const int update = getenv("HULL_TS_CORPUS_UPDATE") != NULL;
    JfdPaths pv = {0};
    ASSERT_EQ(0, jfd_walk(TS_CORPUS "/refuse", EXT_TS, &pv));
    jfd_sort(&pv);
    int checked = 0;
    for (size_t i = 0; i < pv.len; i++) {
        const char *path = pv.items[i];
        size_t n = 0;
        char *src = jfd_read(path, &n);
        ASSERT_TRUE(src != NULL);
        char *o = run(s, "eraseTs", src, n, path, NULL, NULL);
        char *lines = json_str(o, "refusal_lines");
        int refused = lines && lines[0] && has(o, "\"valid\":false");
        char *exp = sibling(path, ".expect");
        int same = 0;
        if (refused && update) {
            size_t ln = strlen(lines);
            char *withnl = (char *)malloc(ln + 2);
            if (withnl) { memcpy(withnl, lines, ln); withnl[ln] = '\n'; withnl[ln + 1] = '\0'; same = write_file(exp, withnl, ln + 1) == 0; free(withnl); }
        } else if (refused) {
            size_t wn = 0; char *want = jfd_read(exp, &wn);
            same = want && wn == strlen(lines) + 1 && memcmp(want, lines, wn - 1) == 0 && want[wn - 1] == '\n';
            free(want);
        }
        if (!(refused && same)) fprintf(stderr, "[ts-corpus] %s: refused=%d expect=%d\n  %s\n", path, refused, same, lines ? lines : "(none)");
        EXPECT_TRUE(refused && same);
        checked++;
        free(exp); free(lines); free(o); free(src);
    }
    jfd_paths_free(&pv);
    EXPECT_GE(checked, 30);
    hl_js_session_destroy(s);
}

/* ---- audit 49 ---- */

/* The erasure of `src` must parse cleanly, refuse nothing, and give exactly `want`. */
static int erases_clean_to(HlJsSession *s, const char *src, const char *want)
{
    char *o = run(s, "eraseTs", src, strlen(src), "t.ts", NULL, NULL);
    char *hx = json_str(o, "erased_hex");
    char *er = hx ? (char *)malloc(strlen(hx) / 2 + 1) : NULL;
    size_t en = (hx && er) ? unhex(hx, er) : (size_t)-1;
    int ok = has(o, "\"valid\":true") && has(o, "\"refusals\":[]") && has(o, "\"erased_valid\":true") &&
             en == strlen(want) && memcmp(er, want, en) == 0;
    if (!ok) fprintf(stderr, "[audit49] %s\n  %s\n", src, o ? o : "(null)");
    free(er); free(hx); free(o);
    return ok;
}

/* Inside a conditional's consequent in a case test, `(x = 5): number => 1` followed by `:` is
 * an arrow with a return type for TypeScript; read as `(x = 5)` the consequent and
 * `number => 1` the alternate, the leftover `:` ended the clause and `w` became a label - both
 * readings parsed, and Hull ran the other one. */
UTEST(parser_audit49, case_conditional_arrow_return_type)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(erases_clean_to(s,
        "switch (x) { case a ? (x = 5): number => 1 : w: f(); }\n",
        "switch (x) { case a ? (x = 5)         => 1 : w: f(); }\n"));
    EXPECT_TRUE(erases_clean_to(s,
        "switch (x) { case a ? async (x = 5): number => 1 : w: f(); }\n",
        "switch (x) { case a ? async (x = 5)         => 1 : w: f(); }\n"));
    /* Nested: the inner arrow, then the outer, each followed by its `:`. */
    EXPECT_TRUE(erases_clean_to(s,
        "switch (x) { case a ? (y): T => (z): U => 1 : w: f(); }\n",
        "switch (x) { case a ? (y)    => (z)    => 1 : w: f(); }\n"));
    /* Not followed by `:`: JavaScript's reading, as TypeScript's. */
    EXPECT_TRUE(erases_clean_to(s, "const g = a ? (x) : y => 1;\n", "const g = a ? (x) : y => 1;\n"));
    EXPECT_TRUE(erases_clean_to(s, "switch (x) { case (y): z => 1; }\n", "switch (x) { case (y): z => 1; }\n"));
    /* Not a parameter list (TypeScript reads no arrow): JavaScript's reading. */
    EXPECT_TRUE(erases_clean_to(s, "switch (x) { case a ? (b + 1): T => 1 : w: f(); }\n",
                                   "switch (x) { case a ? (b + 1): T => 1 : w: f(); }\n"));
    EXPECT_TRUE(erases_clean_to(s, "switch (x) { case a ? async (b.c): T => 1 : w: f(); }\n",
                                   "switch (x) { case a ? async (b.c): T => 1 : w: f(); }\n"));
    /* Read ahead to the `:` with a diagnostic on the way: refused. */
    char *o = ts(s, "switch (x) { case a ? (y): T => <U>z : w: f(); }\n");
    EXPECT_TRUE(has(o, "\"valid\":false") && has(o, "unless it parses cleanly"));
    free(o);
    hl_js_session_destroy(s);
}

/* A statement starting with a regular expression after a declaration erased whole and ended by
 * automatic semicolon insertion: the declaration's last token had read the `/` as division. */
UTEST(parser_audit49, regex_after_erased_declaration)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(erases_clean_to(s, "let z = 1;\ntype T = number\n/x/.test(\"x\");\n",
                             "let z = 1;\n               \n/x/.test(\"x\");\n"));
    EXPECT_TRUE(erases_clean_to(s, "let z = 1;\ninterface I { a: number }\n/x/g.test(\"x\");\n",
                             "let z = 1;\n                         \n/x/g.test(\"x\");\n"));
    EXPECT_TRUE(erases_clean_to(s, "let z = 1;\ndeclare const q: number\n/=x/.test(\"x\");\n",
                             "let z = 1;\n                       \n/=x/.test(\"x\");\n"));
    EXPECT_TRUE(erases_clean_to(s, "let z = 1;\nimport type { X } from \"./m.ts\"\n/x/.test(\"x\");\n",
                             "let z = 1;\n                               \n/x/.test(\"x\");\n"));
    hl_js_session_destroy(s);
}

/* ---- audit 50 ---- */

/* `src` is refused in TS mode, with a diagnostic holding `needle`. */
static int refused_with(HlJsSession *s, const char *src, const char *needle)
{
    char *o = ts(s, src);
    int ok = has(o, "\"valid\":false") && has(o, needle);
    if (!ok) fprintf(stderr, "[audit50] %s\n  %s\n", src, o ? o : "(null)");
    free(o);
    return ok;
}

/* `as` / `satisfies` groups the expression before it; erasing the type removed the grouping, so
 * `price + tax as number * qty` (tsc: `(price + tax) * qty`) ran as `price + tax * qty`. */
UTEST(js_typescript_audit50, as_precedence_barrier)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    static const char *const BAD[] = {
        "x = price + tax as number * qty;\n",
        "x = a < b as any + c;\n",
        "x = a - b as any ** 2;\n",
        "x = a in b as any + c;\n",
        "x = a ** b as any ** c;\n",
        "x = a + b satisfies number * c;\n",
        "x = -a as any ** 2;\n",                    /* `-a ** 2` is a SyntaxError */
        "x = a << b as any + 1;\n",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++)
        EXPECT_TRUE(refused_with(s, BAD[i], "parenthesize the expression before"));
    /* `??` mixed with `||` / `&&` (TypeScript reads `(a ?? b) || c`): a JS syntax error. */
    EXPECT_TRUE(refused_with(s, "x = a ?? b as any || c;\n", "cannot be mixed with"));
    EXPECT_TRUE(refused_with(s, "x = a || b as any ?? c;\n", "cannot be mixed with"));
    EXPECT_TRUE(refused_with(s, "x = a ?? b || c;\n", "cannot be mixed with"));
    /* Same grouping once erased: kept. */
    EXPECT_TRUE(erases_clean_to(s, "x = a * b as any + c;\n", "x = a * b        + c;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = (a + b) as any * c;\n", "x = (a + b)        * c;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = a + b as any > c;\n", "x = a + b        > c;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = (-a) as any ** 2;\n", "x = (-a)        ** 2;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = a ?? (b || c) as any;\n", "x = a ?? (b || c)       ;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = (a || b) as any ?? c;\n", "x = (a || b)        ?? c;\n"));
    hl_js_session_destroy(s);
}

/* After the closing `>` of a type argument list, TypeScript rescans a following `>` / `=` with
 * it: `a < b >>> c` is a comparison. Hull split `>>>`, read `a<b>` as an instantiation and ran
 * `a >> c`. */
UTEST(js_typescript_audit50, unsigned_shift_after_lt)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(erases_clean_to(s, "x = a < b >>> c;\n", "x = a < b >>> c;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = (a<b, c>>>d);\n", "x = (a<b, c>>>d);\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = a<b<c>>>d;\n", "x = a<b<c>>>d;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = f<T>>>(1);\n", "x = f<T>>>(1);\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = a < b >> c;\n", "x = a < b >> c;\n"));
    /* `a<b>==c` / `a<b>>=c` / `f<T> >> 1`: comparisons (TypeScript rejects them too). */
    char *o = ts(s, "x = a<b>==c;\n");
    EXPECT_TRUE(has(o, "\"valid\":false"));
    free(o);
    o = ts(s, "x = a<b>>=c;\n");
    EXPECT_TRUE(has(o, "\"valid\":false"));
    free(o);
    o = ts(s, "x = f<T> >> 1;\n");
    EXPECT_TRUE(has(o, "\"valid\":false"));
    free(o);
    /* Still type arguments. */
    EXPECT_TRUE(erases_clean_to(s, "x = f<A<B>>(c);\n", "x = f      (c);\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = f<T> == 1;\n", "x = f    == 1;\n"));
    hl_js_session_destroy(s);
}

/* An ambient body was skipped with the operator slash goal, so a regex holding `}` ended
 * `declare module "m" { ... }` early and what followed ran as JavaScript. */
UTEST(js_typescript_audit50, ambient_block_regex_brace)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(refused_with(s, "declare module \"m\" { const r = /}/.source; const s = \"/; console.log(1); //\"; }\nexport {};\n",
                             "inside an ambient declaration body"));
    EXPECT_TRUE(refused_with(s, "declare namespace N { const r = /}/.source; const s = \"/; console.log(1); //\"; }\nexport {};\n",
                             "inside an ambient declaration body"));
    EXPECT_TRUE(refused_with(s, "declare namespace N { const r = /{/; }\nconsole.log(1);\n", "inside an ambient declaration body"));
    EXPECT_TRUE(refused_with(s, "declare enum E { A = typeof /}/ }\n", "inside an ambient declaration body"));
    /* An enum initializer's division stays. */
    EXPECT_TRUE(erases_clean_to(s, "declare const enum F { A = 8 / 2, B = (A) / 2 }\nx;\n",
                                   "                                               \nx;\n"));
    hl_js_session_destroy(s);
}

/* The type grammar skipped `[ ... ]` / `( ... )` groups with the operator slash goal; TypeScript
 * reads a computed key there as an expression, so `{ [/]: 1 }; console.log(1); ... /.source]`
 * is one regex to tsc, and Hull ran the `console.log(1)` inside it. */
UTEST(js_typescript_audit50, type_bracket_skip_regex)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(refused_with(s, "let f: { [/]: 1 }; console.log(1); declare namespace Q { /.source]: 1 };\nexport {};\n",
                             "a '/' inside a type is not supported"));
    EXPECT_TRUE(refused_with(s, "let f: { [/]/.source]: 1 };\n", "a '/' inside a type is not supported"));
    char *o = ts(s, "let g: ({ a = /)/ }) => void;\nconsole.log(1);\n");
    EXPECT_TRUE(has(o, "\"valid\":false"));
    free(o);
    EXPECT_TRUE(erases_clean_to(s, "let k: { [Symbol.iterator]: number };\n", "let k                               ;\n"));
    hl_js_session_destroy(s);
}

/* `a as const | 0` is a union type to TypeScript (it emits `a`); erased it was `a | 0`. And an
 * assignment after an assertion, which TypeScript rejects, erased to a chained assignment. */
UTEST(js_typescript_audit50, as_const_union)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_TRUE(refused_with(s, "x = a as const | 0;\n", "after 'as const'"));
    EXPECT_TRUE(refused_with(s, "x = a as const & 0;\n", "after 'as const'"));
    EXPECT_TRUE(refused_with(s, "x = a as const\n| 0;\n", "after 'as const'"));
    EXPECT_TRUE(refused_with(s, "x = y as T\n= 3;\n", "an assignment after the type"));
    EXPECT_TRUE(refused_with(s, "x = y as any = 3;\n", "an assignment after the type"));
    EXPECT_TRUE(refused_with(s, "y as any += 3;\n", "an assignment after the type"));
    EXPECT_TRUE(erases_clean_to(s, "x = a as const;\n", "x = a         ;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = a as T | 0;\n", "x = a         ;\n"));
    EXPECT_TRUE(erases_clean_to(s, "(y as any) = 3;\n", "(y       ) = 3;\n"));
    EXPECT_TRUE(erases_clean_to(s, "x = a as const || b;\n", "x = a          || b;\n"));
    hl_js_session_destroy(s);
}

UTEST_MAIN()
