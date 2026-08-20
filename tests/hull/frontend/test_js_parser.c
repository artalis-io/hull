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
#include <stdio.h>

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
    char *o = parse_str(s, "do { x(); } while (c);");
    EXPECT_TRUE(has(o, "\"code\":\"js.unsupported\""));
    free(o); o = NULL;
    o = parse_str(s, "function* gen() { yield 1; }");
    EXPECT_TRUE(has(o, "\"code\":\"js.unsupported\""));
    EXPECT_TRUE(has(o, "generator"));
    free(o);
    hl_js_session_destroy(s);
}

/* for-in and for-await-of are SUPPORTED (the committed corpus uses both). */
UTEST(js_parser, for_in_and_for_await_of)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = parse_str(s, "for (const k in obj) { use(k); }");
    EXPECT_TRUE(has(o, "\"type\":\"ForInStatement\""));
    EXPECT_FALSE(has(o, "\"code\":\"js.unsupported\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    o = parse_str(s, "async function fa() { for await (const x of xs) { use(x); } }");
    EXPECT_TRUE(has(o, "\"type\":\"ForOfStatement\""));
    EXPECT_TRUE(has(o, "\"await\":true"));
    EXPECT_TRUE(has(o, "\"valid\":true"));
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
    /* Three newline-separated statements, no explicit semicolons. All valid at module top
     * level (a bare `return` is a module-grammar error, so ASI is exercised with a trailing
     * expression statement instead). */
    char *o = parse_str(s, "const a = 1\nconst b = 2\na + b");
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

/* analyze `src` with an options JSON; returns rc and sets *out (caller frees). */
static int analyze_opts(HlJsSession *s, const char *method, const char *src, const char *opts, char **out)
{
    size_t out_len = 0; *out = NULL;
    return hl_js_session_analyze(s, "hull:source:lextest", method,
                                 (const uint8_t *)src, strlen(src), "t.js",
                                 opts, opts ? strlen(opts) : 0, out, &out_len);
}

/* Blocker #1 -- async-arrow parameter DEFAULTS may hold a division OR a regex; the speculative
 * parse must lex each with the correct grammatical slash goal (real parse, not a one-goal
 * pre-scan), and a failed arrow guess must fully rewind so a call re-lexes correctly. */
UTEST(js_parser, async_arrow_slash_defaults)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* arrow whose default uses DIVISION: `a / b` after an operand -> BinaryExpression `/` */
    char *o = parse_str(s, "const f = async (x = a / b) => x;");
    EXPECT_TRUE(has(o, "\"type\":\"ArrowFunctionExpression\""));
    EXPECT_TRUE(has(o, "\"type\":\"BinaryExpression\""));
    EXPECT_FALSE(has(o, "\"regex\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    /* arrow whose default is a REGEX with inner parens: `/a(b)/` after `=` -> regex Literal */
    o = parse_str(s, "const g = async (y = /a(b)/) => y;");
    EXPECT_TRUE(has(o, "\"type\":\"ArrowFunctionExpression\""));
    EXPECT_TRUE(has(o, "\"regex\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o); o = NULL;
    /* NOT an arrow: `async(z = /p/)` with no `=>` -> a CALL; the failed arrow guess rewound, and
     * the regex default still lexes correctly on the call re-parse (no desynchronization). */
    o = parse_str(s, "const r = async(z = /p/);");
    EXPECT_TRUE(has(o, "\"type\":\"CallExpression\""));
    EXPECT_TRUE(has(o, "\"regex\""));
    EXPECT_FALSE(has(o, "\"type\":\"ArrowFunctionExpression\""));
    EXPECT_TRUE(has(o, "\"valid\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* Blocker #2 -- the combined budget is authoritative across BOTH producers even when a lexical
 * diagnostic is emitted AFTER a parser one: `const a = ;` (parser) then `@` (late lexer) then
 * `const b = ;` (parser). Budgets 0/1/2 keep exactly 0/1/2 ordinary diagnostics + a terminal. */
UTEST(js_parser, interleaved_diagnostic_budget)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *src = "const a = ;@const b = ;";
    char *out = NULL;
    /* unbudgeted: at least three ordinary diagnostics exist (so budget 2 genuinely truncates,
     * and the late lexer `@` is one of them). */
    analyze_opts(s, "parse", src, NULL, &out);
    EXPECT_TRUE(count(out, "\"code\":\"js.syntax\"") >= 3);
    free(out); out = NULL;
    analyze_opts(s, "parse", src, "{\"maxDiagnostics\":0}", &out);
    EXPECT_EQ(count(out, "\"code\":\"js.syntax\""), 0);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.diagnostics\""));
    free(out); out = NULL;
    analyze_opts(s, "parse", src, "{\"maxDiagnostics\":1}", &out);
    EXPECT_EQ(count(out, "\"code\":\"js.syntax\""), 1);   /* the late lexer diag did NOT bust the cap */
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.diagnostics\""));
    free(out); out = NULL;
    analyze_opts(s, "parse", src, "{\"maxDiagnostics\":2}", &out);
    EXPECT_EQ(count(out, "\"code\":\"js.syntax\""), 2);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.diagnostics\""));
    free(out);
    hl_js_session_destroy(s);
}

/* Blocker #3 -- resource-ness is NON-FORGEABLE. No JS-observable property of the thrown error
 * (its .name or its .message) can promote an internal defect to a resource limit. Every spoof
 * -- an ordinary Error / TypeError whose message contains a resource phrase, AND a MANUALLY
 * RENAMED error whose .name is exactly "InternalError" (the type QuickJS uses for genuine
 * breaches) -- is contained as js.internal (rc 0), never js.limit.*. */
UTEST(js_parser, spoofed_resource_errors_are_contained)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *specs[] = {
        "{\"inject\":\"error-out of memory\"}",     /* arbitrary resource phrase, ordinary Error */
        "{\"inject\":\"type-stack overflow\"}",      /* arbitrary resource phrase, TypeError */
        "{\"inject\":\"error-interrupted\"}",        /* arbitrary resource phrase */
        "{\"inject\":\"internal-out of memory\"}",   /* .name spoofed to "InternalError" + heap phrase */
        "{\"inject\":\"internal-stack overflow\"}",  /* .name spoofed to "InternalError" + stack phrase */
    };
    for (int i = 0; i < 5; i++) {
        char *out = NULL;
        int rc = analyze_opts(s, "parseInject", "const x = 1;", specs[i], &out);
        ASSERT_EQ(rc, 0);                                 /* contained: a SourceUnit, not host failure */
        EXPECT_TRUE(has(out, "\"status\":\"ok\""));
        EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
        EXPECT_FALSE(has(out, "\"code\":\"js.limit."));    /* NEVER promoted to a resource limit */
        EXPECT_TRUE(has(out, "\"valid\":false"));
        free(out);
    }
    hl_js_session_destroy(s);
}

/* Blocker #3 -- GENUINE resource breaches DURING parsing stay host-classified from AUTHORITATIVE
 * session state (self-enforcing allocator / stack guard / interrupt counter), not the error
 * object: a real heap breach (small heap + large source) -> js.limit.heap; a real stack overflow
 * (deep nesting, depth guard lifted) -> js.limit.stack; a real interrupt (tiny budget) ->
 * js.limit.instructions. */
UTEST(js_parser, genuine_resource_breach_stays_host_classified)
{
    /* genuine heap breach: an 8 MiB heap loads the parser but cannot hold the AST for a large
     * source, so the self-enforcing allocator refuses -> oom_hit -> js.limit.heap. */
    HlJsSessionLimits hlim = (HlJsSessionLimits)HL_JS_SESSION_LIMITS_DEFAULT;
    hlim.max_heap_bytes = (size_t)8 * 1024 * 1024;
    HlJsSession *sh = hl_js_session_create(&hlim);
    ASSERT_TRUE(sh != NULL);
    const int decls = 80000;
    char *hsrc = (char *)malloc((size_t)decls * 48 + 16);   /* "const w79999 = [79999,...];\n" ~= 42 bytes */
    ASSERT_TRUE(hsrc != NULL);
    { int k = 0; for (int i = 0; i < decls; i++) k += sprintf(hsrc + k, "const w%d = [%d,%d,%d];\n", i, i, i, i); hsrc[k] = '\0'; }
    char *oh = NULL;
    int rch = analyze_opts(sh, "parse", hsrc, NULL, &oh);
    free(hsrc);
    ASSERT_EQ(rch, -1);
    EXPECT_TRUE(has(oh, "\"code\":\"js.limit.heap\""));
    free(oh);
    hl_js_session_destroy(sh);

    /* genuine stack overflow: 128 KiB stack guard, ~20k nested parens, parser depth guard lifted
     * so the machine stack (not maxDepth) is what gives way. */
    HlJsSessionLimits lim = (HlJsSessionLimits)HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_stack_bytes = (size_t)128 * 1024;
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    const int nest = 20000;
    char *deep = (char *)malloc((size_t)nest * 2 + 8);
    ASSERT_TRUE(deep != NULL);
    { int k = 0; for (int i = 0; i < nest; i++) deep[k++] = '('; for (int i = 0; i < nest; i++) deep[k++] = ')'; deep[k] = '\0'; }
    char *out = NULL;
    int rc = analyze_opts(s, "parse", deep, "{\"maxDepth\":100000000}", &out);
    free(deep);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.stack\""));
    free(out);
    hl_js_session_destroy(s);

    /* genuine instruction breach: a tiny interrupt budget trips during parsing. The interrupt
     * error is uncatchable, so it bypasses the wrapper's catch entirely and is host-classified.
     * QuickJS invokes the interrupt handler only periodically, so the source must be large enough
     * to run the parser through at least one interrupt tick (~500 statements is comfortably so). */
    HlJsSessionLimits lim2 = (HlJsSessionLimits)HL_JS_SESSION_LIMITS_DEFAULT;
    lim2.max_instructions = 1;
    HlJsSession *s2 = hl_js_session_create(&lim2);
    ASSERT_TRUE(s2 != NULL);
    const int stmts = 1000;
    char *big = (char *)malloc((size_t)stmts * 32 + 16);   /* "const v999 = 999;\n" ~= 18 bytes */
    ASSERT_TRUE(big != NULL);
    { int k = 0; for (int i = 0; i < stmts; i++) k += sprintf(big + k, "const v%d = %d;\n", i, i); big[k] = '\0'; }
    char *out2 = NULL;
    int rc2 = analyze_opts(s2, "parse", big, NULL, &out2);
    free(big);
    ASSERT_EQ(rc2, -1);
    EXPECT_TRUE(has(out2, "\"code\":\"js.limit.instructions\""));
    free(out2);
    hl_js_session_destroy(s2);
}

UTEST_MAIN()
