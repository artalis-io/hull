/*
 * test_js_parser.c - Slice 2: the recursive-descent ECMAScript parser (hull:source:parser),
 * driven through the restricted tooling session via hull:source:parse.
 *
 * The parser drives createTokenizer (never lex) and passes an explicit grammatical slash goal
 * at every advance. These tests assert the AST/diagnostics, and prove the two ambiguous slash
 * cases are resolved from GRAMMAR (via `parse`, not lexDirected forcing).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>

static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }
static int count(const char *hay, const char *needle) {
    int c = 0; size_t nl = strlen(needle);
    if (!hay || nl == 0) return 0;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += nl) c++;
    return c;
}

/* parse `src` and return the malloc'd JSON SourceUnit (caller frees). */
static char *parse_str(HlJsSession *s, const char *src)
{
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", "parse",
                          (const uint8_t *)src, strlen(src), "t.js", NULL, 0, &out, &out_len);
    return out;
}

UTEST(js_parser, variable_declaration)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const x = 1;");
    ASSERT_TRUE(o != NULL);
    EXPECT_TRUE(has(o, "\"type\":\"Program\""));
    EXPECT_TRUE(has(o, "\"type\":\"VariableDeclaration\",\"start\":1,\"stop\":13,\"kind\":\"const\""));
    EXPECT_TRUE(has(o, "\"type\":\"Identifier\",\"start\":7,\"stop\":8,\"name\":\"x\""));
    EXPECT_TRUE(has(o, "\"type\":\"Literal\",\"start\":11,\"stop\":12"));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* GRAMMAR-DRIVEN slash: a regex follows a statement block; division follows a function expr.
 * These use `parse` (not lexDirected) so the decision comes from the grammar. */
UTEST(js_parser, grammar_directed_slash_regex_after_block)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "if (ok) {} /re/.test(s)");
    EXPECT_TRUE(has(o, "\"type\":\"IfStatement\""));
    EXPECT_TRUE(has(o, "\"regex\":{\"pattern\":\"re\""));   /* the `/re/` is a regex Literal */
    EXPECT_TRUE(has(o, "\"type\":\"CallExpression\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, grammar_directed_slash_division_after_function_expr)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const f = function(){} / 2");
    EXPECT_TRUE(has(o, "\"type\":\"BinaryExpression\",\"start\":11,\"stop\":27,\"operator\":\"/\""));
    EXPECT_TRUE(has(o, "\"type\":\"FunctionExpression\""));
    EXPECT_FALSE(has(o, "\"regex\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, functions_arrows_async)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "function f(a, b = 1, ...rest) { return a; }");
    EXPECT_TRUE(has(o, "\"type\":\"FunctionDeclaration\""));
    EXPECT_TRUE(has(o, "\"type\":\"AssignmentPattern\""));
    EXPECT_TRUE(has(o, "\"type\":\"RestElement\""));
    free(o); o = NULL;
    o = parse_str(s, "const g = (x) => x + 1;");
    EXPECT_TRUE(has(o, "\"type\":\"ArrowFunctionExpression\""));
    free(o); o = NULL;
    o = parse_str(s, "const h = async x => await x;");
    EXPECT_TRUE(has(o, "\"type\":\"ArrowFunctionExpression\",\"start\":11") || has(o, "\"async\":true"));
    EXPECT_TRUE(has(o, "\"type\":\"AwaitExpression\""));
    free(o); o = NULL;
    o = parse_str(s, "async function af() { return 1; }");
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, destructuring)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const [ok, err] = f();");
    EXPECT_TRUE(has(o, "\"type\":\"ArrayPattern\""));
    EXPECT_TRUE(has(o, "\"name\":\"ok\""));
    EXPECT_TRUE(has(o, "\"name\":\"err\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "const { a, b: c, d = 1 } = obj;");
    EXPECT_TRUE(has(o, "\"type\":\"ObjectPattern\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, classes)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "class C extends B { constructor(x) { this.x = x; } m() { return 1; } }");
    EXPECT_TRUE(has(o, "\"type\":\"ClassDeclaration\""));
    EXPECT_TRUE(has(o, "\"superClass\""));
    EXPECT_TRUE(has(o, "\"type\":\"MethodDefinition\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, member_call_optional_chaining)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "a.b.c(x)[0]?.d ?? e;");
    EXPECT_TRUE(has(o, "\"type\":\"MemberExpression\""));
    EXPECT_TRUE(has(o, "\"type\":\"CallExpression\""));
    EXPECT_TRUE(has(o, "\"optional\":true"));
    EXPECT_TRUE(has(o, "\"type\":\"LogicalExpression\",\"start\":1"));
    EXPECT_TRUE(has(o, "\"operator\":\"??\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "new Foo(1, 2)");
    EXPECT_TRUE(has(o, "\"type\":\"NewExpression\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, templates_and_tagged)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const t = `a${x + 1}b`;");
    EXPECT_TRUE(has(o, "\"type\":\"TemplateLiteral\""));
    EXPECT_TRUE(has(o, "\"type\":\"TemplateElement\""));
    EXPECT_TRUE(has(o, "\"type\":\"BinaryExpression\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, imports_exports_hull_scheme)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "import { db as database, fs } from \"hull:db\";");
    EXPECT_TRUE(has(o, "\"type\":\"ImportDeclaration\""));
    EXPECT_TRUE(has(o, "\"type\":\"ImportSpecifier\""));
    EXPECT_TRUE(has(o, "\"value\":\"hull:db\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "export const x = 1; export default function f(){}");
    EXPECT_TRUE(has(o, "\"type\":\"ExportNamedDeclaration\""));
    EXPECT_TRUE(has(o, "\"type\":\"ExportDefaultDeclaration\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "const m = await import(\"hull:jwt\");");
    EXPECT_TRUE(has(o, "\"type\":\"ImportExpression\""));
    free(o);
    hl_js_session_destroy(s);
}

UTEST(js_parser, control_flow)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "for (const x of xs) { if (x) continue; else break; }");
    EXPECT_TRUE(has(o, "\"type\":\"ForOfStatement\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "try { f(); } catch (e) { g(e); } finally { h(); }");
    EXPECT_TRUE(has(o, "\"type\":\"TryStatement\""));
    EXPECT_TRUE(has(o, "\"type\":\"CatchClause\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "switch (x) { case 1: y(); break; default: z(); }");
    EXPECT_TRUE(has(o, "\"type\":\"SwitchStatement\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* Declined-but-valid constructs -> js.unsupported (never a wrong AST). */
UTEST(js_parser, unsupported_constructs)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "for (const k in obj) { use(k); }");
    EXPECT_TRUE(has(o, "\"code\":\"js.unsupported\""));
    EXPECT_TRUE(has(o, "for-in"));
    free(o); o = NULL;
    o = parse_str(s, "do { x(); } while (c);");
    EXPECT_TRUE(has(o, "\"code\":\"js.unsupported\""));
    free(o); o = NULL;
    o = parse_str(s, "function* gen() { yield 1; }");
    EXPECT_TRUE(has(o, "\"code\":\"js.unsupported\""));
    EXPECT_TRUE(has(o, "generator"));
    free(o);
    hl_js_session_destroy(s);
}

/* A syntax error emits js.syntax, sets valid=false, and recovery continues (later statements
 * still parse). Tokenizer + parser diagnostics are both preserved. */
UTEST(js_parser, recovery_and_validity)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const x = ;\nconst y = 2;");
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_TRUE(has(o, "\"valid\":false"));
    EXPECT_TRUE(has(o, "\"name\":\"y\""));    /* recovered: the second declaration still parsed */
    free(o); o = NULL;
    /* a lexical diagnostic (bad char) is preserved alongside the parse */
    o = parse_str(s, "let a = 1; @ let b = 2;");
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_TRUE(has(o, "\"name\":\"b\""));
    EXPECT_TRUE(has(o, "\"valid\":false"));
    free(o);
    hl_js_session_destroy(s);
}

/* Comments are collected on the SourceUnit even though the parser ignores them. */
UTEST(js_parser, comments_preserved)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "// hi\nconst x = 1; /** doc */");
    EXPECT_TRUE(has(o, "\"kind\":\"line\""));
    EXPECT_TRUE(has(o, "\"kind\":\"jsdoc\""));
    EXPECT_TRUE(has(o, "\"type\":\"VariableDeclaration\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    EXPECT_EQ(count(o, "\"text\":"), 2);    /* exactly two comments (only comments carry text) */
    free(o);
    hl_js_session_destroy(s);
}

/* ASI: statements separated only by newlines parse without explicit semicolons. */
UTEST(js_parser, automatic_semicolon_insertion)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "const a = 1\nconst b = 2\nreturn a + b");
    EXPECT_TRUE(has(o, "\"name\":\"a\""));
    EXPECT_TRUE(has(o, "\"name\":\"b\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
