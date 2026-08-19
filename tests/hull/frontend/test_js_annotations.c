/*
 * test_js_annotations.c - Slice 3: JSDoc @tag scan + structural declaration attachment,
 * driven through hull:source:parse (the parser attaches during parseInternal).
 *
 * Asserts on the JSON SourceUnit: comment.annotationList (a jsdoc comment's own tags) and the
 * declaration node's annotationList / annotations (the flattened leading run). Covers the
 * design's explicit cases (docs/js_frontend_slice3_annotations.md section 7 + 3.2 + 9.1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }
static int count(const char *hay, const char *needle) {
    int c = 0; size_t nl = strlen(needle);
    if (!hay || nl == 0) return 0;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += nl) c++;
    return c;
}
static char *parse_str(HlJsSession *s, const char *src)
{
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", "parse",
                          (const uint8_t *)src, strlen(src), "a.js", NULL, 0, &out, &out_len);
    return out;
}

/* Occurrences of a tag name in the JSON. Each tag appears once on comment.annotationList; an
 * ATTACHED tag additionally appears in the target's annotationList AND its annotations index, so
 * an attached single tag -> 3 and an unattached one -> 1. This is the robust discriminator for
 * "did it attach to a declaration?" (3) vs "recorded on the comment only" (1); per-declarator
 * over-attachment on a multi-declarator would read 5. */
static int name_count(const char *o, const char *name)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"name\":\"%s\"", name);
    return count(o, needle);
}

/* Generic + whitelist-free: an app's own tags are recorded like standard ones, with the record
 * shapes @name / @name(args) / @name rest / @name(args) rest, exact per-tag ranges + raw. */
UTEST(js_annotations, generic_record_shapes)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s,
        "/**\n * @query\n * @route(GET, /users) list users\n * @derive x\n * @custom(a, b)\n */\n"
        "function getUsers() {}");
    ASSERT_TRUE(o != NULL);
    /* attached to the FunctionDeclaration */
    EXPECT_TRUE(has(o, "\"type\":\"FunctionDeclaration\""));
    EXPECT_TRUE(has(o, "\"annotationList\":["));
    /* @name only */
    EXPECT_TRUE(has(o, "\"name\":\"query\",\"raw\":\"@query\""));
    /* @name(args) rest */
    EXPECT_TRUE(has(o, "\"name\":\"route\",\"args\":\"GET, /users\",\"text\":\"list users\""));
    /* @name rest */
    EXPECT_TRUE(has(o, "\"name\":\"derive\",\"text\":\"x\""));
    /* @name(args) with no trailing text */
    EXPECT_TRUE(has(o, "\"name\":\"custom\",\"args\":\"a, b\""));
    /* whitelist-free: the app tags are present with the same fidelity */
    EXPECT_TRUE(has(o, "\"name\":\"custom\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* Exact byte range + raw provenance: three tags in one block -> three DISTINCT ranges, each raw
 * verbatim, half-open 1-based. */
UTEST(js_annotations, exact_ranges_and_raw)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* Each tag is on its OWN line (a tag opens only at a line-leading @, so a same-line @b would
     * be prose folded into @a's text - see no_false_matches). Three lines -> three tags with
     * three DISTINCT exact ranges, each raw verbatim, half-open 1-based. */
    char *o = parse_str(s, "/**\n * @a A\n * @b B\n * @c C\n */\nconst z = 1;");
    EXPECT_TRUE(has(o, "\"name\":\"a\",\"text\":\"A\",\"raw\":\"@a A\",\"range\":{\"start\":8,\"stop\":12}"));
    EXPECT_TRUE(has(o, "\"name\":\"b\",\"text\":\"B\",\"raw\":\"@b B\",\"range\":{\"start\":16,\"stop\":20}"));
    EXPECT_TRUE(has(o, "\"name\":\"c\",\"text\":\"C\",\"raw\":\"@c C\",\"range\":{\"start\":24,\"stop\":28}"));
    free(o);
    hl_js_session_destroy(s);
}

/* Unknown-tag survival + repeat tags: every @param kept in annotationList; annotations indexes
 * the FIRST by name. */
UTEST(js_annotations, unknown_survival_and_repeat)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s,
        "/**\n * @whatever unrecognized\n * @param a\n * @param b\n * @param c\n */\n"
        "function f() {}");
    EXPECT_TRUE(has(o, "\"name\":\"whatever\""));           /* unknown kept */
    /* all three @param raws survive in the list (repeat tags are not collapsed) */
    EXPECT_TRUE(has(o, "\"raw\":\"@param a\""));
    EXPECT_TRUE(has(o, "\"raw\":\"@param b\""));
    EXPECT_TRUE(has(o, "\"raw\":\"@param c\""));
    /* annotations index keys the FIRST @param (text "a") */
    EXPECT_TRUE(has(o, "\"annotations\":{"));
    EXPECT_TRUE(has(o, "\"param\":{\"name\":\"param\",\"text\":\"a\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Malformed tags (docs 3.2): unmatched ( -> text; nested parens balanced; quoted ) closes
 * early; prose @ on a continuation line folded into text. None emits js.internal. */
UTEST(js_annotations, malformed_tags_deterministic)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* unmatched ( -> the whole remainder is text, raw preserved */
    char *o = parse_str(s, "/** @foo(bar baz */\nconst a = 1;");
    EXPECT_TRUE(has(o, "\"name\":\"foo\",\"text\":\"(bar baz\""));
    EXPECT_FALSE(has(o, "\"code\":\"js.internal\""));
    free(o); o = NULL;
    /* nested parens balanced */
    o = parse_str(s, "/** @foo((a), b) rest */\nconst a = 1;");
    EXPECT_TRUE(has(o, "\"name\":\"foo\",\"args\":\"(a), b\",\"text\":\"rest\""));
    free(o); o = NULL;
    /* quoted ) closes the group early (quote-agnostic): args stops at the first ) */
    o = parse_str(s, "/** @foo(\"a) b\") */\nconst a = 1;");
    EXPECT_TRUE(has(o, "\"name\":\"foo\",\"args\":\"\\\"a\""));
    EXPECT_FALSE(has(o, "\"code\":\"js.internal\""));
    free(o); o = NULL;
    /* a would-be tag (@ignore) MID continuation-line is folded into text, not a new tag */
    o = parse_str(s, "/**\n * @param x\n *   see @ignore for more\n */\nconst a = 1;");
    EXPECT_TRUE(has(o, "\"name\":\"param\""));
    EXPECT_FALSE(has(o, "\"name\":\"ignore\""));            /* the mid-line @ignore is NOT a tag */
    EXPECT_TRUE(has(o, "see @ignore for more"));           /* it is in the param text */
    free(o);
    hl_js_session_destroy(s);
}

/* Multi-declarator: attaches to the VariableDeclaration statement, not the declarators. */
UTEST(js_annotations, multi_declarator)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "/** @foo */\nconst a = 1, b = 2;");
    /* the VariableDeclaration carries the annotation; the declarators do not. Attached-once reads
     * 3 (comment + statement list + statement index); per-declarator would read 5. */
    EXPECT_TRUE(has(o, "\"type\":\"VariableDeclaration\""));
    EXPECT_EQ(name_count(o, "foo"), 3);
    free(o);
    hl_js_session_destroy(s);
}

/* Exports: export const / export function / export default function attach to the INNER
 * declaration via the export line; export { a } and export default <expr> attach to nothing. */
UTEST(js_annotations, exports)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "/** @route */\nexport const handler = 1;");
    EXPECT_TRUE(has(o, "\"declaration\":{\"type\":\"VariableDeclaration\""));
    EXPECT_EQ(name_count(o, "route"), 3);                   /* attached to the inner declaration */
    free(o); o = NULL;
    o = parse_str(s, "/** @api */\nexport function g() {}");
    EXPECT_TRUE(has(o, "\"declaration\":{\"type\":\"FunctionDeclaration\""));
    EXPECT_EQ(name_count(o, "api"), 3);
    free(o); o = NULL;
    o = parse_str(s, "/** @api */\nexport default function f() {}");
    EXPECT_EQ(name_count(o, "api"), 3);
    free(o); o = NULL;
    /* export { a } - a specifier list, no inner declaration: NO attach (tags stay on the comment) */
    o = parse_str(s, "const a = 1;\n/** @nope */\nexport { a };");
    EXPECT_EQ(name_count(o, "nope"), 1);                    /* recorded on the comment only */
    free(o); o = NULL;
    /* export default <expr> - no inner declaration node: NO attach */
    o = parse_str(s, "/** @nope */\nexport default 42;");
    EXPECT_EQ(name_count(o, "nope"), 1);
    free(o);
    hl_js_session_destroy(s);
}

/* The comment-only-line rule (the section-5 correction): code after the JSDoc, or code sharing
 * an intervening comment's line, breaks the run; a clean intervening comment-only line does not;
 * a blank line breaks it. */
UTEST(js_annotations, comment_only_line_rule)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* code AFTER the jsdoc on its own line (its line is not comment-only) -> no attach */
    char *o = parse_str(s, "/** @foo */ doThing();\nconst x = 1;");
    EXPECT_EQ(name_count(o, "foo"), 1);                      /* comment only */
    free(o); o = NULL;
    /* clean intervening comment-only line -> the JSDoc still attaches through the run */
    o = parse_str(s, "/** @foo */\n// an ordinary note\nconst x = 1;");
    EXPECT_TRUE(has(o, "\"type\":\"VariableDeclaration\""));
    EXPECT_EQ(name_count(o, "foo"), 3);                      /* attached through the run */
    free(o); o = NULL;
    /* intervening comment SHARING its line with code -> run broken */
    o = parse_str(s, "/** @foo */\ndoThing(); // note\nconst x = 1;");
    EXPECT_EQ(name_count(o, "foo"), 1);                      /* only on the comment */
    free(o); o = NULL;
    /* blank line breaks the run */
    o = parse_str(s, "/** @foo */\n\nconst x = 1;");
    EXPECT_EQ(name_count(o, "foo"), 1);
    free(o);
    hl_js_session_destroy(s);
}

/* Unsupported declaration forms: tags are recorded on the comment but attach to no declaration. */
UTEST(js_annotations, unsupported_forms)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* an expression / call statement */
    char *o = parse_str(s, "/** @foo */\ndoThing();");
    EXPECT_EQ(name_count(o, "foo"), 1);                      /* comment only */
    free(o); o = NULL;
    /* a property-assignment function expression (an assignment, not a declaration) */
    o = parse_str(s, "/** @foo */\nretry.run = function() {};");
    EXPECT_EQ(name_count(o, "foo"), 1);
    free(o); o = NULL;
    /* a declined js.unsupported statement (generator) */
    o = parse_str(s, "/** @foo */\nfunction* gen() { yield 1; }");
    EXPECT_TRUE(has(o, "\"name\":\"foo\""));
    EXPECT_TRUE(has(o, "\"code\":\"js.unsupported\""));
    EXPECT_FALSE(has(o, "\"code\":\"js.internal\""));         /* declined-not-target is not an internal fault */
    free(o);
    hl_js_session_destroy(s);
}

/* Unattached-tag exposure + field shape: an @file/@module header block's tags live on
 * comment.annotationList (always an array on jsdoc, absent on line/block comments). */
UTEST(js_annotations, unattached_and_field_shape)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* header block above a non-declaration: tags on the comment, attached to nothing */
    char *o = parse_str(s, "/**\n * @file hull:x\n * @module hull:x\n */\nfoo();");
    EXPECT_TRUE(has(o, "\"kind\":\"jsdoc\""));
    EXPECT_TRUE(has(o, "\"name\":\"file\",\"text\":\"hull:x\""));
    EXPECT_TRUE(has(o, "\"name\":\"module\",\"text\":\"hull:x\""));
    EXPECT_EQ(name_count(o, "file"), 1);                     /* unattached: comment only */
    EXPECT_EQ(name_count(o, "module"), 1);
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    /* a jsdoc block with NO tags still gets an (empty) array; a line comment gets no field, and
     * the const is not attached (empty run) -> the ONLY annotationList is the empty jsdoc one. */
    o = parse_str(s, "/** just prose, no tags */\n// a line comment\nconst x = 1;");
    EXPECT_TRUE(has(o, "\"annotationList\":[]"));             /* the jsdoc comment's empty array */
    EXPECT_EQ(count(o, "\"annotationList\""), 1);             /* no line-comment field, no node attach */
    EXPECT_TRUE(has(o, "\"kind\":\"line\""));
    free(o);
    hl_js_session_destroy(s);
}

/* No false matches: an @ inside a string / template / regex / prose is never a tag. */
UTEST(js_annotations, no_false_matches)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const a = \"@foo not a tag\";\nconst b = `@bar ${x}`;\nconst c = /@baz/;\n");
    EXPECT_FALSE(has(o, "\"annotationList\""));               /* nothing scanned from code tokens */
    EXPECT_FALSE(has(o, "\"name\":\"foo\""));
    EXPECT_FALSE(has(o, "\"name\":\"bar\""));
    EXPECT_FALSE(has(o, "\"name\":\"baz\""));
    free(o); o = NULL;
    /* an @ mid-prose inside a jsdoc block (not line-leading) is not a tag */
    o = parse_str(s, "/** contact me@host for help */\nconst x = 1;");
    EXPECT_TRUE(has(o, "\"annotationList\":[]"));             /* the block scanned, zero tags */
    EXPECT_FALSE(has(o, "\"name\":\"host\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Multiple comments ending on one physical line are all collected as a group (source order),
 * mixed jsdoc/ordinary in either order: the run is not collapsed to a single comment. */
UTEST(js_annotations, same_line_comment_group)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* jsdoc then ordinary on one comment-only line -> the jsdoc still attaches */
    char *o = parse_str(s, "/** @query */ /* ordinary note */\nconst q = 1;");
    EXPECT_EQ(name_count(o, "query"), 3);
    free(o); o = NULL;
    /* ordinary then jsdoc */
    o = parse_str(s, "/* ordinary note */ /** @query */\nconst q = 1;");
    EXPECT_EQ(name_count(o, "query"), 3);
    free(o); o = NULL;
    /* two jsdoc blocks on one line -> BOTH attach, in source order */
    o = parse_str(s, "/** @a */ /** @b */\nconst q = 1;");
    EXPECT_EQ(name_count(o, "a"), 3);
    EXPECT_EQ(name_count(o, "b"), 3);
    free(o);
    hl_js_session_destroy(s);
}

/* U+2028 / U+2029 / CR / CRLF between the JSDoc and the declaration are line terminators, so the
 * declaration attaches (the terminator bytes are not treated as code, and do not leak into
 * ranges/text). */
UTEST(js_annotations, line_terminators)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "/** @foo */\r\nconst x = 1;");            /* CRLF */
    EXPECT_EQ(name_count(o, "foo"), 3);
    free(o); o = NULL;
    o = parse_str(s, "/** @foo */\rconst x = 1;");                    /* lone CR */
    EXPECT_EQ(name_count(o, "foo"), 3);
    free(o); o = NULL;
    o = parse_str(s, "/** @foo */\xe2\x80\xa8" "const x = 1;");       /* U+2028 */
    EXPECT_EQ(name_count(o, "foo"), 3);
    free(o); o = NULL;
    o = parse_str(s, "/** @foo */\xe2\x80\xa9" "const x = 1;");       /* U+2029 */
    EXPECT_EQ(name_count(o, "foo"), 3);
    free(o); o = NULL;
    /* a U+2028 line terminator inside the block does not leak into a tag range/raw */
    o = parse_str(s, "/**\xe2\x80\xa8 * @foo bar\xe2\x80\xa8 */\nconst x = 1;");
    EXPECT_TRUE(has(o, "\"name\":\"foo\",\"text\":\"bar\",\"raw\":\"@foo bar\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Single-internal latch (docs 9.1): multiple invalid ranges in one unit produce EXACTLY ONE
 * js.internal and abort. Exercised via a synthetic unit (the parser never emits bad ranges). */
UTEST(js_annotations, single_internal_latch)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    /* mode 'c': several jsdoc comments with invalid ranges + a bad declaration */
    hl_js_session_analyze(s, "hull:source:lextest", "attachCorrupt",
                          (const uint8_t *)"c", 1, "a.js", NULL, 0, &out, &out_len);
    EXPECT_TRUE(has(out, "\"internal_count\":1"));
    free(out); out = NULL;
    /* mode 'd': several declaration targets with invalid ranges */
    hl_js_session_analyze(s, "hull:source:lextest", "attachCorrupt",
                          (const uint8_t *)"d", 1, "a.js", NULL, 0, &out, &out_len);
    EXPECT_TRUE(has(out, "\"internal_count\":1"));
    free(out);
    hl_js_session_destroy(s);
}

/* Idempotency (the REAL test): parse once, then invoke attach() AGAIN on the SAME ast/comments,
 * and assert the projection is byte-identical (no duplicated node/comment annotations). */
UTEST(js_annotations, idempotent_reattach)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    const char *src = "/** @a */ /** @b */\nexport const q = 1, r = 2;\n/** @c */\nfunction f() {}";
    hl_js_session_analyze(s, "hull:source:lextest", "reattach",
                          (const uint8_t *)src, strlen(src), "a.js", NULL, 0, &out, &out_len);
    EXPECT_TRUE(has(out, "\"ast_identical\":true"));
    EXPECT_TRUE(has(out, "\"comments_identical\":true"));
    EXPECT_TRUE(has(out, "\"reattach_diagnostics\":0"));     /* the second attach found no internal fault */
    free(out);
    hl_js_session_destroy(s);
}

/* Hardened contract (docs 9.1): a jsdoc-free file parses identically to before (no
 * annotationList anywhere), and a clean file with tags stays diagnostics-clean (empty
 * diagnostics guarantees attachment did not internally fail). */
UTEST(js_annotations, hardened_no_internal_on_clean)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const x = 1;\nfunction f() { return x; }\n");
    EXPECT_FALSE(has(o, "\"annotationList\""));
    EXPECT_TRUE(has(o, "\"diagnostics\":[]"));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "/** @a */\nexport function g() {}");
    EXPECT_FALSE(has(o, "\"code\":\"js.internal\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* Guardrail: a recovered syntax-error AST legitimately contains Error nodes and incomplete
 * non-target nodes. Attachment SKIPS those without js.internal - js.internal is reserved for a
 * recognized declaration target with an invalid range (or an attach exception). A malformed but
 * recovering source with a JSDoc emits js.syntax and, when it recovers to a real declaration,
 * still attaches - but never js.internal. */
UTEST(js_annotations, recovered_errors_no_internal)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* JSDoc above a malformed statement that recovers, then a clean declaration */
    const char *cases[] = {
        "/** @foo */\nconst = ;\nconst y = 2;",             /* malformed const -> Error, recovers */
        "/** @foo */\nlet 3 = x;\nfunction g() {}",         /* numeric binding -> Error, recovers */
        "/** @foo */\n})(\nconst z = 3;",                   /* stray tokens -> Error, recovers */
        "/** @foo */\nclass {",                             /* unterminated class */
    };
    for (int i = 0; i < 4; i++) {
        char *o = parse_str(s, cases[i]);
        ASSERT_TRUE(o != NULL);
        EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));       /* it is malformed */
        EXPECT_FALSE(has(o, "\"code\":\"js.internal\""));    /* but attachment did not fault */
        free(o);
    }
    hl_js_session_destroy(s);
}

UTEST_MAIN()
