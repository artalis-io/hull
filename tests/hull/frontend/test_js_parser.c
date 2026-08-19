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

/* Non-destructive speculation: valid forms where a keyword-like token is actually a name /
 * call, and the tokenizer must NOT desynchronize after a failed arrow/modifier guess. */
UTEST(js_parser, speculation_valid_forms)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* async(x) is a call, not an arrow; and the `/ 2` after it must survive the failed
     * arrow speculation as a division (tokenizer not desynchronized). */
    char *o = parse_str(s, "async(x) / 2");
    EXPECT_TRUE(has(o, "\"type\":\"BinaryExpression\",\"start\":1") || has(o, "\"operator\":\"/\""));
    EXPECT_TRUE(has(o, "\"type\":\"CallExpression\""));
    EXPECT_TRUE(has(o, "\"name\":\"async\""));
    EXPECT_FALSE(has(o, "\"type\":\"ArrowFunctionExpression\""));
    EXPECT_FALSE(has(o, "\"regex\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    /* { async: 1 } and { get: 1 } -- async/get are property keys, not modifiers */
    o = parse_str(s, "const o = { async: 1, get: 2, set: 3 };");
    EXPECT_TRUE(has(o, "\"type\":\"ObjectExpression\""));
    EXPECT_TRUE(has(o, "\"name\":\"async\""));
    EXPECT_TRUE(has(o, "\"name\":\"get\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    /* { async() {} } -- a method literally named `async` */
    o = parse_str(s, "const m = { async() { return 1; } };");
    EXPECT_TRUE(has(o, "\"name\":\"async\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    /* a class field named `static` */
    o = parse_str(s, "class C { static = 1; m() {} }");
    EXPECT_TRUE(has(o, "\"type\":\"PropertyDefinition\""));
    EXPECT_TRUE(has(o, "\"name\":\"static\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* async function DECLARATION (not an expression statement), plus async arrows. */
UTEST(js_parser, async_declaration_and_arrows)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "async function run() { return 1; }");
    EXPECT_TRUE(has(o, "\"type\":\"FunctionDeclaration\""));
    EXPECT_TRUE(has(o, "\"async\":true"));
    EXPECT_FALSE(has(o, "\"type\":\"ExpressionStatement\""));
    EXPECT_TRUE(has(o, "\"name\":\"run\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "const f = async x => await x;");
    EXPECT_TRUE(has(o, "\"type\":\"ArrowFunctionExpression\""));
    EXPECT_TRUE(has(o, "\"async\":true"));
    EXPECT_TRUE(has(o, "\"type\":\"AwaitExpression\""));
    free(o); o = NULL;
    o = parse_str(s, "const g = async (a, b) => a + b;");
    EXPECT_TRUE(has(o, "\"type\":\"ArrowFunctionExpression\""));
    EXPECT_TRUE(has(o, "\"async\":true"));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    /* `async` as an ordinary identifier reference */
    o = parse_str(s, "const h = async + 1;");
    EXPECT_TRUE(has(o, "\"name\":\"async\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* Combined diagnostic budget (tokenizer + parser) is authoritative; maxDiagnostics=0 keeps
 * only the terminal marker. */
UTEST(js_parser, combined_diagnostic_budget)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    /* several parser syntax errors, budget 0 -> only js.limit.diagnostics terminal(s) */
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "parse",
                                   (const uint8_t *)"const a = ; const b = ; const c = ;", 35, "t.js",
                                   "{\"maxDiagnostics\":0}", 20, &out, &out_len);
    (void)rc;
    EXPECT_EQ(count(out, "\"code\":\"js.syntax\""), 0);            /* no ordinary parser diags kept */
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.diagnostics\""));   /* terminal retained */
    free(out); out = NULL;
    /* budget 2 over many parser errors -> exactly 2 ordinary + terminal */
    rc = hl_js_session_analyze(s, "hull:source:lextest", "parse",
                               (const uint8_t *)"const a = ; const b = ; const c = ; const d = ;", 47, "t.js",
                               "{\"maxDiagnostics\":2}", 20, &out, &out_len);
    EXPECT_EQ(count(out, "\"code\":\"js.syntax\""), 2);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.diagnostics\""));
    free(out);
    hl_js_session_destroy(s);
}

/* The public "never raises" contract: an injected internal defect yields a SourceUnit with a
 * structured js.internal diagnostic (rc 0), not a session-level failure. */
UTEST(js_parser, internal_failure_is_contained)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "parse",
                                   (const uint8_t *)"const x = 1;", 12, "t.js",
                                   "{\"_throwInternal\":true}", 23, &out, &out_len);
    ASSERT_EQ(rc, 0);                                  /* the boundary produced a result, not an exception */
    EXPECT_TRUE(has(out, "\"status\":\"ok\""));
    EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
    EXPECT_TRUE(has(out, "\"valid\":false"));
    free(out);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
