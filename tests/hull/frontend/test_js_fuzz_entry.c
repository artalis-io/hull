/*
 * test_js_fuzz_entry.c - regression for the fuzz-parse test entry's recovery classification.
 *
 * The libFuzzer harness's nesting exemption (docs/js_source_fuzz_design.md 4.2) is gated on a
 * `recovery` classification. The locked requirement: an UNSUPPORTED-ONLY unit stays STRICT even
 * when a small diagnostic budget suppresses js.unsupported into js.limit.diagnostics --
 * js.limit.diagnostics is NOT by itself evidence of syntax recovery. hull:source:tests:fuzz_parse
 * surfaces `recovery` in its verdict so this can be asserted directly (the fuzzer ignores it).
 *
 * Drives the entry through the restricted QuickJS session (precompiled bytecode; no raw JS_Eval),
 * linking the TEST cli-js registry that carries fuzz_parse.
 *
 * The TypeScript leg (lang "ts", docs/typescript_design.md section 5, M6) adds the span and
 * erasure invariants; its cases below prove they hold for real TypeScript and that the check
 * runs (a clean unit is erased and re-parsed).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int has(const char *h, const char *n) { return h && strstr(h, n) != NULL; }

/* Run hull:source:tests:fuzz_parse on `src` with maxDiagnostics `md`; malloc'd verdict (free). */
static char *fuzz(HlJsSession *s, const char *src, long md)
{
    char opts[48];
    snprintf(opts, sizeof opts, "{\"maxDiagnostics\":%ld}", md);
    char *out = NULL; size_t ol = 0;
    hl_js_session_analyze(s, "hull:source:tests:fuzz_parse", "fuzz",
                          (const uint8_t *)src, strlen(src), "t.js", opts, strlen(opts), &out, &ol);
    return out;
}

/* THE regression: `with (x) { y; }` is valid syntax but DECLINED (js.unsupported), consumed
 * cleanly. At maxDiagnostics=0 the js.unsupported is suppressed and replaced by
 * js.limit.diagnostics; the reparse reveals NO js.syntax, so the unit stays STRICT. */
UTEST(js_fuzz_entry, unsupported_only_maxdiag0_is_strict)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fuzz(s, "with (x) { y; }", 0);
    ASSERT_TRUE(o != NULL);
    EXPECT_TRUE(has(o, "\"ok\":true"));
    EXPECT_TRUE(has(o, "\"recovery\":false"));     /* NOT relaxed to recovery */
    free(o);
    /* And with an ample budget the same input is still strict (js.unsupported visible). */
    o = fuzz(s, "with (x) { y; }", 4096);
    EXPECT_TRUE(has(o, "\"ok\":true") && has(o, "\"recovery\":false"));
    free(o);
    hl_js_session_destroy(s);
}

/* Contrast: a real syntax error at maxDiagnostics=0 reparses and classifies as recovery. */
UTEST(js_fuzz_entry, syntax_error_maxdiag0_is_recovery)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fuzz(s, "const = = )", 0);
    ASSERT_TRUE(o != NULL);
    EXPECT_TRUE(has(o, "\"ok\":true"));
    EXPECT_TRUE(has(o, "\"recovery\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* A clean parse is recovery:false. */
UTEST(js_fuzz_entry, clean_is_strict)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fuzz(s, "const a = 1; function f() { return a; }", 4096);
    ASSERT_TRUE(o != NULL);
    EXPECT_TRUE(has(o, "\"ok\":true"));
    EXPECT_TRUE(has(o, "\"recovery\":false"));
    free(o);
    hl_js_session_destroy(s);
}

/* Run the entry in TypeScript mode. */
static char *fuzz_ts(HlJsSession *s, const char *src, long md)
{
    char opts[64];
    snprintf(opts, sizeof opts, "{\"maxDiagnostics\":%ld,\"lang\":\"ts\"}", md);
    char *out = NULL; size_t ol = 0;
    hl_js_session_analyze(s, "hull:source:tests:fuzz_parse", "fuzz",
                          (const uint8_t *)src, strlen(src), "t.ts", opts, strlen(opts), &out, &ol);
    return out;
}

/* Clean TypeScript passes the erasure invariants; refused and broken TypeScript is bounded. */
UTEST(js_fuzz_entry, typescript_leg)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    static const char *const OK[] = {
        "let x: Map<string, Array<number>> = new Map();\nconst f = (a: number, b?: string): number => a;",
        "interface I { a: string }\ntype T<U> = U[];\nimport type { A } from \"m\";\nexport { type B, c };",
        "class C { x?: number; y!: string; m(a: T): void {} }\nconst v = obj!.a as unknown as T satisfies U;",
        "/** @route GET / */\ninterface Q {}\nexport function h(q: Q): void {}",
        "enum E { A }",                                   /* refused: still ok:true, bounded */
        /* a refused declaration read whole stays inside the statement that holds it */
        "// ts\nif (ready) type T = number;\n",          /* fuzz_js_source crash, PR #1124 */
        "if (x) interface I { a: 1 }\nelse f();",
        "while (x) type T = 1;",
        "{ enum E { A } }\nfunction g() { namespace N { const r = /}/; } }",
        "export const enum E { A }\nexport namespace N {}\nmodule \"m\" {}",
        "if (x) const enum K { B }",
        "let x: <",                                       /* malformed type */
        "let a = b\ntype T = number\n(c)",              /* the ASI refusal */
        "let s: \"\xe2\x80\xa8\" = 1;",              /* U+2028 inside a type: erase refuses, allowed */
    };
    for (size_t i = 0; i < sizeof(OK) / sizeof(OK[0]); i++) {
        for (long md = 0; md <= 4096; md = md ? md * 4096 : 1) {
            char *o = fuzz_ts(s, OK[i], md);
            if (!has(o, "\"ok\":true")) fprintf(stderr, "[fuzz-ts] md=%ld %s\n  %s\n", md, OK[i], o ? o : "(null)");
            EXPECT_TRUE(has(o, "\"ok\":true"));
            free(o);
            if (md == 4096) break;
        }
    }
    hl_js_session_destroy(s);
}

UTEST_MAIN()
