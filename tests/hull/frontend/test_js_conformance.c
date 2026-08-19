/*
 * test_js_conformance.c - Slice 2: compile-only conformance harness.
 *
 * Each corpus item is run through BOTH:
 *   - the Hull frontend parser (hull:source:parser, via the restricted tooling session), and
 *   - a QuickJS COMPILE-ONLY oracle (parse + bytecode-gen, no execution) in MODULE mode.
 *
 * The parser's verdict is one of ACCEPT / REJECT (js.syntax) / UNSUPPORTED (js.unsupported,
 * a declined-but-valid construct) / INDETERMINATE (js.limit.* / js.internal -- a host resource
 * or internal outcome, NOT a grammar verdict). The oracle's verdict is ACCEPT / REJECT.
 *
 * Results are separated into four buckets, reported clearly, and gated:
 *   - FALSE-REJECT : parser REJECT but oracle ACCEPT  -> a real parser bug (FAILS the test).
 *   - FALSE-ACCEPT : parser ACCEPT but oracle REJECT  -> a real parser bug (FAILS the test).
 *   - UNSUPPORTED  : parser declined (js.unsupported) -- reported, never a failure.
 *   - INDETERMINATE: parser js.limit.* / js.internal  -- reported, excluded from the grammar
 *                    comparison (no corpus item here should be indeterminate; flagged if so).
 * AGREE (both ACCEPT, or both REJECT) is the success path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"
#include "quickjs.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef enum { V_ACCEPT, V_REJECT, V_UNSUPPORTED, V_INDETERMINATE } Verdict;

typedef struct { const char *label; const char *src; } Case;

/* Supported ECMAScript the parser should ACCEPT (valid in strict/module mode, so the oracle
 * accepts too). Kept free of strict/sloppy-divergent constructs so the comparison is clean. */
static const Case VALID[] = {
    { "const decl",            "const x = 1;" },
    { "multi var",             "let a, b = 2, c;" },
    { "object destructure",    "var {x, y = 3, ...rest} = obj;" },
    { "array destructure",     "const [a, , b, ...c] = arr;" },
    { "function + defaults",   "function f(a, b = 1, ...r) { return a + b; }" },
    { "arrow",                 "const g = (x) => x * 2;" },
    { "arrow block body",      "const g2 = (x) => { return x; };" },
    { "async arrow await",     "const h = async (x) => await x;" },
    { "async function decl",   "async function af() { return 1; }" },
    { "class full",            "class C extends B { constructor() { super(); } get x() { return 1; } set x(v) {} static s() {} m() {} }" },
    { "if/elseif/else",        "if (a) { b(); } else if (c) { d(); } else { e(); }" },
    { "for c-style",           "for (let i = 0; i < 10; i++) { use(i); }" },
    { "for-of",                "for (const x of xs) { use(x); }" },
    { "while",                 "while (c) { work(); }" },
    { "switch",                "switch (x) { case 1: a(); break; default: b(); }" },
    { "try/catch/finally",     "try { f(); } catch (e) { g(e); } finally { h(); }" },
    { "optional catch",        "try { f(); } catch { g(); }" },
    { "template",              "const t = `a${x + 1}b${y}c`;" },
    { "tagged template",       "const tt = tag`x${y}z`;" },
    { "object literal",        "const o = { a: 1, b, [c]: 2, m() {}, get p() { return 1; }, ...spread };" },
    { "optional chaining",     "const r = a?.b?.[c]?.();" },
    { "nullish",               "const n = a ?? b ?? c;" },
    { "spread array",          "const arr2 = [1, 2, ...rest, 3];" },
    { "exponent",              "const e = a ** b ** c;" },
    { "new + args",            "new Foo(1, 2);" },
    { "conditional",           "const cond = a ? b : c;" },
    { "sequence",              "const seq = (a, b, c);" },
    { "throw",                 "throw new Error('x');" },
    { "empty statements",      "work(); ;; ;" },
    { "asi",                   "const a1 = 1\nconst b1 = 2\nfoo()" },
    { "slash regex after blk", "if (ok) {} /re/.test(s)" },
    { "slash div after fnexpr","const q = function(){} / 2;" },
    { "import named+alias",    "import { a, b as c } from \"m\";" },
    { "import default+named",  "import def, { x } from \"m\";" },
    { "import namespace",      "import * as ns from \"m\";" },
    { "import bare",           "import \"m\";" },
    { "export const",          "export const ex = 1;" },
    { "export list+alias",     "const a = 1; const b = 2; export { a, b as c };" },
    { "export default fn",     "export default function f() {}" },
    { "export default expr",   "export default 42;" },
    { "dynamic import",        "const di = import(\"m\");" },
    { "import.meta",           "const im = import.meta;" },
    { "unary + update",        "const u = -a + +b - --c + typeof d;" },
    { "member/call chain",     "obj.a.b().c[d].e();" },
};

/* Malformed source the parser should REJECT (js.syntax) -- invalid in both strict and sloppy,
 * so the oracle rejects too. */
static const Case MALFORMED[] = {
    { "missing rhs",           "const a = ;" },
    { "unclosed function",     "function () {" },
    { "unbalanced paren",      "let x = (1 + ;" },
    { "empty object value",    "const o = {a: };" },
    { "class no name/body",    "class {" },
    { "unclosed for",          "for (;;" },
    { "dangling binary",       "1 +" },
    { "no binding name",       "const = 5;" },
    { "unclosed if",           "if (a" },
    { "stray close",           "})" },
    { "unterminated template", "const x = `unterminated" },
    { "stray at",              "let a = @;" },
    { "numeric binding",       "let 3 = x;" },
    { "adjacent operands",     "const a = [1, 2 3];" },
    { "case no test",          "switch (x) { case: break; }" },
    { "unclosed object",       "x = {" },
    { "double comma call",     "f(1,, 2);" },
};

/* Valid ECMAScript the parser DECLINES with js.unsupported (never a wrong AST). The oracle
 * ACCEPTS most (they are real ES); `with` is strict-invalid so the oracle rejects it -- either
 * way the parser has DECLINED, which is the point of this bucket. */
static const Case UNSUPPORTED[] = {
    { "for-in",                "for (const k in obj) { use(k); }" },
    { "do-while",              "do { x(); } while (c);" },
    { "generator decl",        "function* gen() { yield 1; }" },
    { "generator expr",        "const ge = function*() { yield 1; };" },
    { "async generator",       "async function* ag() { yield 1; }" },
    { "private field",         "class P { #priv = 1; #m() { return this.#priv; } }" },
    { "export all",            "export * from \"m\";" },
    { "with (strict-invalid)", "with (o) { x; }" },
};

/* -- the parser verdict (via the tooling session) -- */
static Verdict parser_verdict(HlJsSession *s, const char *src, char **raw_out)
{
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "parse",
                                   (const uint8_t *)src, strlen(src), "c.js", NULL, 0, &out, &out_len);
    if (raw_out) *raw_out = out; else if (out) free(out);
    if (rc != 0 || !out) return V_INDETERMINATE;                       /* host failure */
    if (strstr(out, "\"code\":\"js.limit.") || strstr(out, "\"code\":\"js.internal\"")) return V_INDETERMINATE;
    if (strstr(out, "\"code\":\"js.syntax\"")) return V_REJECT;         /* malformed */
    if (strstr(out, "\"code\":\"js.unsupported\"")) return V_UNSUPPORTED;
    return V_ACCEPT;                                                    /* valid:true */
}

/* The oracle compiles in MODULE mode, which RESOLVES imports and LINKS exports -- more than
 * pure syntax. A no-op loader returns a synthetic module exporting the names the corpus imports,
 * so an `import { a } from "m"` links instead of failing "could not load module". (Export corpus
 * items self-declare their exported bindings for the same reason.) This keeps the oracle's
 * accept/reject a SYNTAX verdict, not a module-graph verdict. */
static JSModuleDef *oracle_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque;
    static const char *SYN = "export const a=1,b=2,c=3,x=4,def=5; export default def;";
    JSValue v = JS_Eval(ctx, SYN, strlen(SYN), name, JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); return NULL; }
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, v);
    return m;
}

/* -- the compile-only oracle: parse + bytecode-gen, no execution, MODULE mode -- */
static Verdict oracle_verdict(JSContext *octx, const char *src)
{
    JSValue v = JS_Eval(octx, src, strlen(src), "c.js",
                        JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE);
    int ex = JS_IsException(v);
    if (ex) { JSValue e = JS_GetException(octx); JS_FreeValue(octx, e); }  /* clear the pending exception */
    JS_FreeValue(octx, v);
    return ex ? V_REJECT : V_ACCEPT;
}

static const char *vname(Verdict v)
{
    switch (v) { case V_ACCEPT: return "accept"; case V_REJECT: return "reject";
                 case V_UNSUPPORTED: return "unsupported"; default: return "indeterminate"; }
}

UTEST(js_conformance, corpus_vs_quickjs_oracle)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);

    /* A plain QuickJS oracle context WITH eval (compile-only parsing needs the eval intrinsic).
     * Independent of the session -- this is the reference grammar. */
    JSRuntime *ort = JS_NewRuntime();
    ASSERT_TRUE(ort != NULL);
    JSContext *octx = JS_NewContext(ort);
    ASSERT_TRUE(octx != NULL);
    JS_SetModuleLoaderFunc(ort, NULL, oracle_module_loader, NULL);

    int agree = 0, false_reject = 0, false_accept = 0, unsupported = 0, indeterminate = 0;

    /* One pass over each labeled bucket; verdicts are compared, not assumed. */
    const Case *buckets[] = { VALID, MALFORMED, UNSUPPORTED };
    const size_t counts[] = { sizeof(VALID)/sizeof(VALID[0]),
                              sizeof(MALFORMED)/sizeof(MALFORMED[0]),
                              sizeof(UNSUPPORTED)/sizeof(UNSUPPORTED[0]) };
    for (int b = 0; b < 3; b++) {
        for (size_t i = 0; i < counts[b]; i++) {
            const Case *c = &buckets[b][i];
            char *raw = NULL;
            Verdict pv = parser_verdict(s, c->src, &raw);
            Verdict ov = oracle_verdict(octx, c->src);

            if (pv == V_INDETERMINATE) {
                indeterminate++;
                fprintf(stderr, "[indeterminate] %-24s parser=%s oracle=%s :: %s\n",
                        c->label, vname(pv), vname(ov), raw ? raw : "");
            } else if (pv == V_UNSUPPORTED) {
                unsupported++;   /* declined-but-valid; expected, never a failure */
            } else if (pv == V_ACCEPT && ov == V_REJECT) {
                false_accept++;
                fprintf(stderr, "[FALSE-ACCEPT] %-24s :: %s\n", c->label, c->src);
            } else if (pv == V_REJECT && ov == V_ACCEPT) {
                false_reject++;
                fprintf(stderr, "[FALSE-REJECT] %-24s :: %s\n", c->label, c->src);
            } else {
                agree++;         /* accept/accept or reject/reject */
            }
            free(raw);
        }
    }

    fprintf(stderr,
            "\nconformance: agree=%d  false-reject=%d  false-accept=%d  unsupported=%d  indeterminate=%d\n",
            agree, false_reject, false_accept, unsupported, indeterminate);

    /* The gate: zero grammar mismatches. Unsupported + indeterminate are reported, not failed. */
    EXPECT_EQ(false_reject, 0);
    EXPECT_EQ(false_accept, 0);
    EXPECT_EQ(indeterminate, 0);   /* no curated corpus item should be a host/internal outcome */
    /* Sanity: the buckets did exercise their intended shapes. */
    EXPECT_TRUE(agree > 30);
    EXPECT_TRUE(unsupported >= 6);

    JS_FreeContext(octx);
    JS_FreeRuntime(ort);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
