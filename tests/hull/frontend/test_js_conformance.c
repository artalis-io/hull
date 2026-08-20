/*
 * test_js_conformance.c - Slice 2: compile-only conformance harness.
 *
 * Every input is run through BOTH:
 *   - the Hull frontend parser (hull:source:parser, via the restricted tooling session), and
 *   - a QuickJS COMPILE-ONLY oracle (parse + bytecode-gen, no execution) in MODULE mode.
 *
 * The parser's verdict is one of ACCEPT / REJECT (js.syntax) / UNSUPPORTED (js.unsupported, a
 * declined-but-valid construct) / INDETERMINATE (js.limit.* / js.internal -- a host resource or
 * internal outcome, NOT a grammar verdict). The oracle's verdict is ACCEPT / REJECT.
 *
 * Two legs:
 *  1. repo_corpus  -- the committed application JS corpus (design 3/9.2): stdlib/js/hull,
 *     examples, tests/fixtures, enumerated FAIL-CLOSED (regular files only, no symlinks,
 *     `static`/`node_modules`/dot dirs excluded, sorted, full-read validated). The grammar
 *     target was derived from these files, so every QuickJS-accepted corpus file MUST parse
 *     cleanly, and the directional invariant (parser accepts => QuickJS accepts) is checked.
 *  2. curated_matrix -- representative supported ES, malformed negatives, declined-but-valid
 *     constructs, and an explicit EXPECTED-DIVERGENCE allowlist (parser vs oracle differ by a
 *     documented static-semantic / strict-mode rule, not a grammar bug).
 *
 * Buckets, reported and gated:
 *   FALSE-REJECT   parser REJECT, oracle ACCEPT                      -> gated 0
 *   FALSE-ACCEPT   parser ACCEPT, oracle REJECT (outside allowlist)  -> gated 0
 *   UNSUPPORTED    parser declined AND oracle ACCEPTS (valid decline)-> reported
 *   UNSUP-REJECT   parser declined but oracle REJECTS (no allowlist) -> gated 0 (a decline
 *                    must not hide a malformed input)
 *   DIVERGENCE     an allowlisted parser/oracle mismatch             -> reported
 *   INDETERMINATE  parser js.limit.* / js.internal                   -> gated 0
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"
#include "quickjs.h"
#include "sh_json.h"
#include "sh_arena.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

typedef enum { V_ACCEPT, V_REJECT, V_UNSUPPORTED, V_INDETERMINATE } Verdict;
typedef struct { const char *label; const char *src; } Case;

typedef struct {
    int agree, false_reject, false_accept, unsupported, unsup_reject, divergence, indeterminate;
} Tally;

/* -- shared verdict engines -- */
static Verdict parser_verdict(HlJsSession *s, const char *src, size_t len, char **raw_out)
{
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:source:lextest", "parse",
                                   (const uint8_t *)src, len, "c.js", NULL, 0, &out, &out_len);
    if (raw_out) *raw_out = out; else if (out) free(out);
    if (rc != 0 || !out) return V_INDETERMINATE;
    if (strstr(out, "\"code\":\"js.limit.") || strstr(out, "\"code\":\"js.internal\"")) return V_INDETERMINATE;
    if (strstr(out, "\"code\":\"js.syntax\"")) return V_REJECT;
    if (strstr(out, "\"code\":\"js.unsupported\"")) return V_UNSUPPORTED;
    return V_ACCEPT;
}

/* The oracle compiles in MODULE mode. Compile-only RESOLVES imports (via the loader) but does
 * NOT bind-check them, so a permissive loader that returns a minimal module for ANY specifier
 * lets a corpus file's imports compile -- the accept/reject stays a SYNTAX verdict rather than a
 * module-graph verdict. (Compile-only DOES check export-side undeclared bindings, so curated
 * export cases self-declare their bindings.) */
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

static Verdict oracle_verdict(JSContext *octx, const char *src, size_t len, const char *name)
{
    JSValue v = JS_Eval(octx, src, len, name, JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE);
    int ex = JS_IsException(v);
    if (ex) { JSValue e = JS_GetException(octx); JS_FreeValue(octx, e); }
    JS_FreeValue(octx, v);
    return ex ? V_REJECT : V_ACCEPT;
}

static const char *vname(Verdict v)
{
    switch (v) { case V_ACCEPT: return "accept"; case V_REJECT: return "reject";
                 case V_UNSUPPORTED: return "unsupported"; default: return "indeterminate"; }
}

/* Classify one (parser, oracle) pair into the tally. allow_divergence marks a bucket whose
 * parser/oracle mismatches are DOCUMENTED expected divergences, not bugs. Returns 1 if the pair
 * is a hard failure (for per-item reporting by the caller). */
static int classify(Tally *t, Verdict pv, Verdict ov, int allow_divergence)
{
    if (pv == V_INDETERMINATE) { t->indeterminate++; return 1; }
    if (pv == V_UNSUPPORTED) {
        if (ov == V_ACCEPT) { t->unsupported++; return 0; }
        if (allow_divergence) { t->divergence++; return 0; }
        t->unsup_reject++; return 1;                 /* a decline hiding a malformed input */
    }
    if (pv == V_ACCEPT && ov == V_REJECT) {
        if (allow_divergence) { t->divergence++; return 0; }  /* a static-semantic divergence */
        t->false_accept++; return 1;
    }
    if (pv == V_REJECT && ov == V_ACCEPT) { t->false_reject++; return 1; }
    t->agree++; return 0;                            /* accept/accept or reject/reject */
}

/* -- leg 2: the curated matrix -- */

/* Supported ECMAScript the parser ACCEPTS (valid in strict/module mode; the oracle accepts). */
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
    { "export all",            "export * from \"m\";" },
    { "export all as ns",      "export * as ns from \"m\";" },
    { "labeled statement",     "outer: for (;;) break outer;" },
    { "for-in no-decl",        "for (x in obj) { break; }" },
    { "for await of",          "async function fa() { for await (const x of xs) { use(x); } }" },
    { "class full",            "class C extends B { constructor() { super(); } get x() { return 1; } set x(v) {} static s() {} m() {} }" },
    { "if/elseif/else",        "if (a) { b(); } else if (c) { d(); } else { e(); }" },
    { "for c-style",           "for (let i = 0; i < 10; i++) { use(i); }" },
    { "for-of",                "for (const x of xs) { use(x); }" },
    { "for-in",                "for (const k in obj) { use(k); }" },
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
    /* Module-grammar context rules (docs/js_test262_design.md), VALID side -- oracle ACCEPTs. */
    { "top-level await",       "const twa = await p;" },
    { "await in async body",   "async function af2() { return await x; }" },
    { "dyn import in fn",      "function fdi() { return import(\"m\"); }" },
    { "dyn import in block",   "{ const bdi = import(\"m\"); }" },
    { "new.target in fn",      "function fnt() { return new.target; }" },
    { "new.target in param",   "function fnp(x = new.target) { return x; }" },
    { "new.target nested arrow","function fna() { const a = () => new.target; return a; }" },
    { "new.target in method",  "class Cnt { m() { return new.target; } }" },
    /* Reserved-word exemptions: eval/arguments are legal references, and strict-reserved words
     * stay legal as property keys / member names (only bindings + references reject). */
    { "eval as reference",     "eval(1);" },
    { "arguments reference",   "const ar = [arguments];" },
    { "reserved as prop key",  "const o = { public: 1, static: 2 }; o.public;" },
};

/* Malformed source the parser REJECTS (js.syntax) -- invalid in both strict and sloppy. */
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
    /* Module-grammar context rules (docs/js_test262_design.md), INVALID side -- both reject.
     * These exercise the parser's explicit function-context stack + module-item-position flag. */
    { "await in regular fn",   "function g() { return await x; }" },        /* await not an expr here */
    { "await in async params", "async function h(x = await 1) {}" },        /* params region rejects await */
    { "bare await no operand", "const z = await;" },                        /* TLA requires an operand */
    { "return at top level",   "return 1;" },                               /* return needs a function */
    { "static import in block","{ import x from \"m\"; }" },                 /* import is module-top-level only */
    { "static import in fn",   "function k() { import y from \"m\"; }" },
    { "export in block",       "{ export const q = 1; }" },                 /* export is module-top-level only */
    { "export in fn",          "function m2() { export default 1; }" },
    { "escaped import.meta",   "const y = import.m\\u0065ta;" },            /* meta must be exact + unescaped */
    /* Reserved-word + invalid-assignment-target grammar rejects (both reject). */
    { "await as binding",      "var await;" },                              /* await reserved in module code */
    { "await as class name",   "class await {}" },
    { "await as label",        "await: 1;" },
    { "escaped await ident",   "\\u0061wait 0;" },                          /* escaped keyword is a reserved ident */
    { "escaped import kw",     "im\\u0070ort.meta;" },                      /* import may not be escaped */
    { "meta update target",    "import.meta++;" },                          /* MetaProperty is not an update target */
    { "meta for-of target",    "for (import.meta of null) ;" },             /* invalid for-of left */
    { "new.target top level",  "new.target;" },                             /* no enclosing function */
    { "new.target top arrow",  "const gnt = () => new.target;" },           /* top-level arrow: no non-arrow frame */
    /* Strict/module reserved words as bindings, and bare yield outside a generator. */
    { "strict-reserved bind",  "var public;" },                             /* FutureReservedWord binding */
    { "var interface",         "var interface;" },
    { "import-as reserved",    "import { x as eval } from \"m\";" },         /* eval illegal binding target */
    { "import local reserved", "import { eval } from \"m\";" },             /* no-`as` local must be bindable */
    { "bare yield in module",  "yield;" },                                  /* reserved outside a generator */
};

/* Recognized, VALID ECMAScript that Hull deliberately declines with js.unsupported (never a
 * wrong AST). The oracle ACCEPTS every one -- that is what makes it an honest decline. */
static const Case UNSUPPORTED_VALID[] = {
    { "do-while",              "do { x(); } while (c);" },
    { "generator decl",        "function* gen() { yield 1; }" },
    { "generator expr",        "const ge = function*() { yield 1; };" },
    { "async generator",       "async function* ag() { yield 1; }" },
    { "private field",         "class P { #priv = 1; #m() { return this.#priv; } }" },
};

/* Expected, DOCUMENTED divergences -- the structural parser and QuickJS legitimately differ:
 *  - `with` is grammatically a statement Hull declines (js.unsupported) but QuickJS rejects it
 *    as a strict-mode/module early error (both decline; different code).
 *  - duplicate lexical binding / undeclared export are STATIC-SEMANTIC early errors QuickJS
 *    enforces but the structural parser intentionally does not (design 8c) -> parser ACCEPTs,
 *    oracle REJECTs. Allowlisted, never counted as a false-accept. */
static const Case DIVERGENCE[] = {
    { "with (strict early err)",   "with (o) { x; }" },
    { "dup lexical binding",       "let d = 1; let d = 2;" },
    { "undeclared export",         "export { neverDeclared };" },
};

UTEST(js_conformance, curated_matrix)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    JSRuntime *ort = JS_NewRuntime(); ASSERT_TRUE(ort != NULL);
    JSContext *octx = JS_NewContext(ort); ASSERT_TRUE(octx != NULL);
    JS_SetModuleLoaderFunc(ort, NULL, oracle_module_loader, NULL);

    Tally t = {0};
    struct { const Case *cases; size_t n; int allow_div; const char *name; } groups[] = {
        { VALID,           sizeof(VALID)/sizeof(VALID[0]),                     0, "valid" },
        { MALFORMED,       sizeof(MALFORMED)/sizeof(MALFORMED[0]),             0, "malformed" },
        { UNSUPPORTED_VALID, sizeof(UNSUPPORTED_VALID)/sizeof(UNSUPPORTED_VALID[0]), 0, "unsupported" },
        { DIVERGENCE,      sizeof(DIVERGENCE)/sizeof(DIVERGENCE[0]),           1, "divergence" },
    };
    for (size_t g = 0; g < sizeof(groups)/sizeof(groups[0]); g++) {
        for (size_t i = 0; i < groups[g].n; i++) {
            const Case *c = &groups[g].cases[i];
            char *raw = NULL;
            Verdict pv = parser_verdict(s, c->src, strlen(c->src), &raw);
            Verdict ov = oracle_verdict(octx, c->src, strlen(c->src), "c.js");
            if (classify(&t, pv, ov, groups[g].allow_div))
                fprintf(stderr, "[%s] %-24s parser=%s oracle=%s :: %s\n",
                        groups[g].name, c->label, vname(pv), vname(ov), c->src);
            free(raw);
        }
    }
    fprintf(stderr, "\ncurated: agree=%d false-reject=%d false-accept=%d unsupported=%d "
            "unsup-reject=%d divergence=%d indeterminate=%d\n",
            t.agree, t.false_reject, t.false_accept, t.unsupported, t.unsup_reject,
            t.divergence, t.indeterminate);

    EXPECT_EQ(t.false_reject, 0);
    EXPECT_EQ(t.false_accept, 0);
    EXPECT_EQ(t.unsup_reject, 0);
    EXPECT_EQ(t.indeterminate, 0);
    EXPECT_EQ(t.divergence, (int)(sizeof(DIVERGENCE)/sizeof(DIVERGENCE[0])));  /* every allowlisted case diverged */
    EXPECT_TRUE(t.unsupported >= 5);
    EXPECT_TRUE(t.agree > 30);

    JS_FreeContext(octx); JS_FreeRuntime(ort);
    hl_js_session_destroy(s);
}

/* -- leg 1: the committed application-JS corpus (fail-closed enumeration) -- */

typedef struct { char **items; size_t len, cap; } PathVec;
static int pv_push(PathVec *v, const char *p)
{
    if (v->len == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 256;
        char **ni = (char **)realloc(v->items, nc * sizeof(char *));
        if (!ni) return -1;
        v->items = ni; v->cap = nc;
    }
    v->items[v->len] = strdup(p);
    return v->items[v->len++] ? 0 : -1;
}
static void pv_free(PathVec *v) { for (size_t i = 0; i < v->len; i++) free(v->items[i]); free(v->items); }
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int has_js_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcmp(dot, ".js") || !strcmp(dot, ".mjs") || !strcmp(dot, ".cjs"));
}

/* Recursive, FAIL-CLOSED walk: regular files only (symlinks skipped), the D6 exclusions
 * (`static`), plus `node_modules` and dot-directories. Any opendir/lstat/overlong-path failure
 * returns -1 so the leg fails rather than silently under-enumerating. */
static int walk(const char *dir, PathVec *out)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    for (;;) {
        /* readdir returns NULL for BOTH end-of-directory and error; reset errno first so the
         * two are distinguishable. A NULL with errno != 0 is a traversal failure -> fail closed. */
        errno = 0;
        e = readdir(d);
        if (!e) { if (errno != 0) rc = -1; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        int k = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (k < 0 || (size_t)k >= sizeof(path)) { rc = -1; break; }
        struct stat st;
        if (lstat(path, &st) != 0) { rc = -1; break; }
        if (S_ISLNK(st.st_mode)) continue;                       /* no symlinks */
        if (S_ISDIR(st.st_mode)) {
            /* `test262` is the pinned conformance corpus (its own leg), NOT application source. */
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "static") || !strcmp(e->d_name, "node_modules") || !strcmp(e->d_name, "test262")) continue;
            if (walk(path, out) != 0) { rc = -1; break; }
        } else if (S_ISREG(st.st_mode) && has_js_ext(e->d_name)) {
            if (pv_push(out, path) != 0) { rc = -1; break; }
        }
    }
    /* Propagate a close failure only if the walk itself was otherwise clean. */
    if (closedir(d) != 0 && rc == 0) rc = -1;
    return rc;
}

/* Full-read with validation: regular file, exact-size read (no short read), NUL-terminated.
 * Returns NULL (leg fails) on any open/fstat/alloc/short-read failure. */
static char *read_full(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    size_t n = (size_t)st.st_size;
    char *buf = (char *)malloc(n + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != n) { free(buf); return NULL; }                    /* short read -> fail */
    buf[n] = '\0'; *out_len = n;
    return buf;
}

/* The enumeration MUST fail closed, never report a partial success as a clean walk. This
 * exercises the reachable failure modes directly: opendir on a missing path, opendir on a
 * regular file (ENOTDIR), and opendir on an unreadable directory (EACCES). A mid-iteration
 * readdir() error (EIO / ENOMEM) cannot be injected on a real directory without a syscall-level
 * mock (LD_PRELOAD / FUSE), which is unavailable in this unit harness; walk() resets errno
 * before each readdir and returns -1 on a NULL-with-errno, resolving the end-of-directory vs
 * error ambiguity, and propagates a closedir() failure when the walk was otherwise clean. */
UTEST(js_conformance, enumeration_fail_closed)
{
    PathVec pv = {0};
    EXPECT_EQ(walk("stdlib/js/hull/does-not-exist-xyz-123", &pv), -1);   /* opendir ENOENT */
    EXPECT_EQ(walk("stdlib/js/hull/template.js", &pv), -1);              /* opendir ENOTDIR (a file) */
    if (geteuid() != 0) {                                                /* root bypasses perms */
        const char *nd = "build/.conf_noperm_dir";
        rmdir(nd);
        if (mkdir(nd, 0700) == 0 && chmod(nd, 0) == 0) {
            EXPECT_EQ(walk(nd, &pv), -1);                                /* opendir EACCES */
            chmod(nd, 0700);
        }
        rmdir(nd);
    }
    EXPECT_EQ((int)pv.len, 0);                                           /* no partial results banked */
    pv_free(&pv);
}

UTEST(js_conformance, repo_corpus)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    JSRuntime *ort = JS_NewRuntime(); ASSERT_TRUE(ort != NULL);
    JSContext *octx = JS_NewContext(ort); ASSERT_TRUE(octx != NULL);
    JS_SetModuleLoaderFunc(ort, NULL, oracle_module_loader, NULL);

    /* The application-source roots (design 3): run from the repo root by `make test`. */
    const char *roots[] = { "stdlib/js/hull", "examples", "tests/fixtures" };
    PathVec pv = {0};
    for (size_t r = 0; r < sizeof(roots)/sizeof(roots[0]); r++)
        ASSERT_EQ_MSG(walk(roots[r], &pv), 0, "corpus enumeration must not fail-open");
    qsort(pv.items, pv.len, sizeof(char *), cmp_str);
    int enumerated = (int)pv.len;

    int read_ok = 0, analyzed = 0, clean = 0, declined = 0, oracle_reject = 0, false_accept = 0, fail = 0, indeterminate = 0;
    for (size_t i = 0; i < pv.len; i++) {
        size_t len = 0;
        char *bytes = read_full(pv.items[i], &len);
        ASSERT_TRUE_MSG(bytes != NULL, pv.items[i]);             /* full-read validation is fail-closed */
        read_ok++;
        char *raw = NULL;
        Verdict pver = parser_verdict(s, bytes, len, &raw);
        Verdict over = oracle_verdict(octx, bytes, len, pv.items[i]);
        analyzed++;
        if (pver == V_INDETERMINATE) {
            indeterminate++; fail++;
            fprintf(stderr, "[corpus indeterminate] %s :: %.240s\n", pv.items[i], raw ? raw : "");
        } else if (over == V_ACCEPT) {
            if (pver == V_ACCEPT) clean++;
            else { fail++; fprintf(stderr, "[corpus not-clean] %s parser=%s :: %.240s\n",
                                   pv.items[i], vname(pver), raw ? raw : ""); }
        } else {                                                 /* oracle REJECT (e.g. a negative fixture) */
            oracle_reject++;
            if (pver == V_ACCEPT) { false_accept++; fail++;
                fprintf(stderr, "[corpus false-accept] %s :: QuickJS rejects but parser accepts\n", pv.items[i]); }
        }
        free(raw); free(bytes);
    }
    fprintf(stderr, "\ncorpus: enumerated=%d read=%d analyzed=%d clean=%d declined=%d "
            "oracle-reject=%d false-accept=%d indeterminate=%d\n",
            enumerated, read_ok, analyzed, clean, declined, oracle_reject, false_accept, indeterminate);

    EXPECT_TRUE(enumerated >= 150);      /* the design corpus is ~151+ files; a real, exercised set */
    EXPECT_EQ(read_ok, enumerated);      /* every enumerated file fully read */
    EXPECT_EQ(analyzed, enumerated);     /* every read file analyzed */
    EXPECT_EQ(fail, 0);                  /* every QuickJS-accepted file parsed cleanly; no false-accept */

    pv_free(&pv);
    JS_FreeContext(octx); JS_FreeRuntime(ort);
    hl_js_session_destroy(s);
}

/* -- leg 3: the pinned, module-only Test262 subset (docs/js_test262_design.md) --
 *
 * Runs the committed corpus (tests/fixtures/test262) through a THREE-way oracle: Test262 metadata
 * (positive | parse-negative), the QuickJS compile-only MODULE verdict, and the Hull parser. CI is
 * completely OFFLINE: it consumes only the committed subset; scripts/fetch_test262.sh is refresh
 * tooling and is NEVER invoked here. The leg is closed both ways -- an unlisted non-agree outcome
 * fails, and a stale expectation / unused inventory key fails. */

#define T262_DIR      "tests/fixtures/test262"
#define T262_CASES    T262_DIR "/cases"
#define T262_MANIFEST T262_DIR "/manifest.json"
#define T262_MANHASH  T262_DIR "/MANIFEST.sha256"
#define T262_EXPECT   T262_DIR "/expectations.json"
#define T262_PINNED_SHA "3655e7464de3d52643ecddd4b5f9f4f3e7f62398"
#define T262_SCHEMA 1
#define T262_SELECTION_RULES 2

/* Minimal SHA-256 (self-contained; corpus integrity only -- no crypto-cap link dependency). */
typedef struct { uint32_t s[8]; uint64_t n; uint8_t b[64]; size_t bl; } Sha256;
static const uint32_t T262_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static uint32_t t262_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void t262_block(Sha256 *c, const uint8_t *p) {
    uint32_t w[64], a, b_, cc, d, e, f, g, h, i, t1, t2;
    for (i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = t262_ror(w[i-15],7)^t262_ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = t262_ror(w[i-2],17)^t262_ror(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    a=c->s[0];b_=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1=t262_ror(e,6)^t262_ror(e,11)^t262_ror(e,25), ch=(e&f)^(~e&g);
        t1=h+S1+ch+T262_K[i]+w[i];
        uint32_t S0=t262_ror(a,2)^t262_ror(a,13)^t262_ror(a,22), mj=(a&b_)^(a&cc)^(b_&cc);
        t2=S0+mj; h=g;g=f;f=e;e=d+t1;d=cc;cc=b_;b_=a;a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b_;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void t262_sha_hex(const void *data, size_t len, char out[65]) {
    Sha256 c; c.s[0]=0x6a09e667;c.s[1]=0xbb67ae85;c.s[2]=0x3c6ef372;c.s[3]=0xa54ff53a;
    c.s[4]=0x510e527f;c.s[5]=0x9b05688c;c.s[6]=0x1f83d9ab;c.s[7]=0x5be0cd19;c.n=0;c.bl=0;
    const uint8_t *p = (const uint8_t *)data; size_t rem = len;
    while (rem) { size_t k = 64 - c.bl; if (k > rem) k = rem; memcpy(c.b + c.bl, p, k); c.bl += k; p += k; rem -= k;
        if (c.bl == 64) { t262_block(&c, c.b); c.bl = 0; } }
    c.n = (uint64_t)len * 8;
    size_t z = (c.bl < 56) ? (56 - c.bl) : (120 - c.bl);
    Sha256 *cp = &c;
    { const uint8_t one = 0x80; memcpy(cp->b + cp->bl, &one, 1); cp->bl++; z--; if (cp->bl==64){t262_block(cp,cp->b);cp->bl=0;} }
    while (z) { cp->b[cp->bl++] = 0; z--; if (cp->bl==64){t262_block(cp,cp->b);cp->bl=0;} }
    for (int i = 7; i >= 0; i--) { cp->b[cp->bl++] = (uint8_t)(cp->n >> (i*8)); if (cp->bl==64){t262_block(cp,cp->b);cp->bl=0;} }
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) for (int j = 3; j >= 0; j--) {
        uint8_t byte = (uint8_t)(c.s[i] >> (j*8));
        *out++ = hx[byte >> 4]; *out++ = hx[byte & 15];
    }
    *out = '\0';
}

/* A committed case path must be relative, canonical, and contain no `.`/`..`/empty component,
 * no backslash, no `//`, and no leading/trailing slash. */
static int t262_path_ok(const char *p) {
    if (!p || !*p || p[0] == '/') return 0;
    if (strchr(p, '\\') || strstr(p, "//")) return 0;
    size_t n = strlen(p);
    if (p[n-1] == '/') return 0;
    const char *seg = p;
    for (size_t i = 0; ; i++) {
        if (p[i] == '/' || p[i] == '\0') {
            size_t sl = (size_t)(&p[i] - seg);
            if (sl == 0) return 0;
            if (sl == 1 && seg[0] == '.') return 0;
            if (sl == 2 && seg[0] == '.' && seg[1] == '.') return 0;
            if (p[i] == '\0') break;
            seg = &p[i+1];
        }
    }
    return 1;
}

/* js.unsupported message -> closed inventory key (the same 4 keys expectations.json declares). */
static const char *t262_inv_key(const char *raw) {
    if (strstr(raw, "generator functions are not supported")) return "generators";
    if (strstr(raw, "do-while statement is not supported")) return "do-while";
    if (strstr(raw, "private class members are not supported")) return "private-class-members";
    if (strstr(raw, "private member access is not supported")) return "private-member-access";
    return NULL;
}

UTEST(js_conformance, test262)
{
    /* ---- read + integrity-check the manifest and its hash ---- */
    size_t man_len = 0; char *man_bytes = read_full(T262_MANIFEST, &man_len);
    ASSERT_TRUE_MSG(man_bytes != NULL, "manifest.json must be readable");
    size_t mh_len = 0; char *mh = read_full(T262_MANHASH, &mh_len);
    ASSERT_TRUE_MSG(mh != NULL, "MANIFEST.sha256 must be readable");
    char man_hex[65]; t262_sha_hex(man_bytes, man_len, man_hex);
    ASSERT_TRUE_MSG(strncmp(mh, man_hex, 64) == 0, "MANIFEST.sha256 must equal sha256(manifest.json)");
    free(mh);

    SHArena *arena = sh_arena_create(8 * 1024 * 1024);
    ASSERT_TRUE(arena != NULL);
    ShJsonValue *mroot = NULL;
    ASSERT_EQ_MSG(sh_json_parse(man_bytes, man_len, arena, &mroot), SH_JSON_OK, "manifest.json parse");
    ASSERT_EQ(sh_json_as_int(sh_json_get(mroot, "schema_version"), -1), T262_SCHEMA);
    ASSERT_EQ(sh_json_as_int(sh_json_get(mroot, "selection_rules_version"), -1), T262_SELECTION_RULES);
    ASSERT_TRUE_MSG(strcmp(sh_json_as_string(sh_json_get(mroot, "upstream_sha"), ""), T262_PINNED_SHA) == 0, "pinned SHA");
    ShJsonValue *cases = sh_json_get(mroot, "cases");
    ASSERT_TRUE(cases && sh_json_type(cases) == SH_JSON_ARRAY);
    int count = sh_json_as_int(sh_json_get(mroot, "count"), -1);
    ASSERT_EQ_MSG(count, (int)sh_json_array_len(cases), "manifest count == cases length");

    /* ---- read + parse expectations.json ---- */
    size_t exp_len = 0; char *exp_bytes = read_full(T262_EXPECT, &exp_len);
    ASSERT_TRUE_MSG(exp_bytes != NULL, "expectations.json must be readable");
    ShJsonValue *eroot = NULL;
    ASSERT_EQ_MSG(sh_json_parse(exp_bytes, exp_len, arena, &eroot), SH_JSON_OK, "expectations.json parse");
    ASSERT_EQ(sh_json_as_int(sh_json_get(eroot, "schema_version"), -1), T262_SCHEMA);
    ShJsonValue *exp_obj = sh_json_get(eroot, "expectations");
    ShJsonValue *inv_obj = sh_json_get(eroot, "unsupported_inventory");
    ASSERT_TRUE(exp_obj && sh_json_type(exp_obj) == SH_JSON_OBJECT);
    ASSERT_TRUE(inv_obj && sh_json_type(inv_obj) == SH_JSON_OBJECT);
    size_t exp_n = exp_obj->u.object_val.count;
    size_t inv_n = inv_obj->u.object_val.count;
    int *exp_used = (int *)calloc(exp_n ? exp_n : 1, sizeof(int));
    int *inv_used = (int *)calloc(inv_n ? inv_n : 1, sizeof(int));
    ASSERT_TRUE(exp_used && inv_used);

    /* ---- enumerate cases/ on disk (fail-closed) and require a bijection with the manifest ---- */
    PathVec disk = {0};
    ASSERT_EQ_MSG(walk(T262_CASES, &disk), 0, "cases/ enumeration must not fail-open");
    /* only .js (walk already filters to .js/.mjs/.cjs; the corpus is .js) */

    HlJsSession *s = hl_js_session_create(NULL); ASSERT_TRUE(s != NULL);
    JSRuntime *ort = JS_NewRuntime(); ASSERT_TRUE(ort != NULL);
    JSContext *octx = JS_NewContext(ort); ASSERT_TRUE(octx != NULL);
    JS_SetModuleLoaderFunc(ort, NULL, oracle_module_loader, NULL);   /* permissive: resolution never becomes a parser verdict */

    int enumerated = (int)disk.len, read_ok = 0, hashed = 0, analyzed = 0;
    int positive = 0, parse_negative = 0;
    int agree = 0, false_reject = 0, false_accept = 0, unsupported = 0, unsup_reject = 0, divergence = 0, indeterminate = 0;
    int fails = 0;

    for (size_t i = 0; i < sh_json_array_len(cases); i++) {
        ShJsonValue *c = sh_json_array_get(cases, i);
        const char *path = sh_json_as_string(sh_json_get(c, "path"), NULL);
        const char *shash = sh_json_as_string(sh_json_get(c, "source_hash"), NULL);
        ASSERT_TRUE_MSG(path && shash, "case entry needs path + source_hash");
        if (!t262_path_ok(path)) { fprintf(stderr, "[t262 bad path] %s\n", path); fails++; continue; }
        /* no duplicate path (manifest is sorted; adjacent compare) */
        if (i > 0) {
            const char *prev = sh_json_as_string(sh_json_get(sh_json_array_get(cases, i-1), "path"), "");
            if (strcmp(prev, path) >= 0) { fprintf(stderr, "[t262 unsorted/dup] %s\n", path); fails++; }
        }
        char full[4096];
        int k = snprintf(full, sizeof(full), "%s/%s", T262_CASES, path);
        ASSERT_TRUE(k > 0 && (size_t)k < sizeof(full));
        struct stat st;
        ASSERT_EQ_MSG(lstat(full, &st), 0, path);
        ASSERT_TRUE_MSG(S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode), "case must be a regular file, no symlink");
        size_t len = 0; char *bytes = read_full(full, &len);
        ASSERT_TRUE_MSG(bytes != NULL, full);
        read_ok++;
        char hex[65]; t262_sha_hex(bytes, len, hex);
        ASSERT_TRUE_MSG(strcmp(hex, shash) == 0, path);   /* exact committed bytes */
        hashed++;

        ShJsonValue *neg = sh_json_get(c, "negative");
        int is_neg = neg && sh_json_type(neg) == SH_JSON_OBJECT;   /* selection kept only phase:parse */
        if (is_neg) parse_negative++; else positive++;

        char *raw = NULL;
        Verdict P = parser_verdict(s, bytes, len, &raw);
        Verdict O = oracle_verdict(octx, bytes, len, path);
        Verdict E = is_neg ? V_REJECT : V_ACCEPT;
        analyzed++;
        free(bytes);

        /* three-way bucketing (docs/js_test262_design.md section 7) */
        if (P == V_INDETERMINATE) { indeterminate++; fails++; fprintf(stderr, "[t262 indeterminate] %s\n", path); }
        else if (E != O) { divergence++; fails++; fprintf(stderr, "[t262 divergence] %s (meta=%s oracle=%s)\n", path, E==V_REJECT?"neg":"pos", vname(O)); }
        else if (P == V_UNSUPPORTED) {
            const char *key = t262_inv_key(raw ? raw : "");
            if (!key) { fprintf(stderr, "[t262 unknown unsupported key] %s :: %.180s\n", path, raw ? raw : ""); fails++; }
            else {
                /* the key must be declared in the closed inventory (else unexpected unsupported) */
                int found = -1;
                for (size_t m = 0; m < inv_n; m++) if (!strcmp(inv_obj->u.object_val.members[m].key, key)) { found = (int)m; break; }
                if (found < 0) { fprintf(stderr, "[t262 unexpected unsupported] %s key=%s\n", path, key); fails++; }
                else inv_used[found]++;
            }
            if (O == V_ACCEPT) unsupported++;
            else {
                /* unsupported-reject: needs an exact-path reviewed expectation */
                unsup_reject++;
                int found = -1;
                for (size_t m = 0; m < exp_n; m++) if (!strcmp(exp_obj->u.object_val.members[m].key, path)) { found = (int)m; break; }
                if (found < 0) { fprintf(stderr, "[t262 unexpected unsupported-reject] %s\n", path); fails++; }
                else {
                    ShJsonValue *ent = exp_obj->u.object_val.members[found].value;
                    const char *cat = sh_json_as_string(sh_json_get(ent, "category"), "");
                    if (strcmp(cat, "unsupported-reject") != 0) { fprintf(stderr, "[t262 wrong category] %s\n", path); fails++; }
                    else exp_used[found]++;
                }
            }
        }
        else if (P == V_ACCEPT && O == V_REJECT) {
            /* false-accept: allowed ONLY with an exact-path static-semantic-omission expectation */
            false_accept++;
            int found = -1;
            for (size_t m = 0; m < exp_n; m++) if (!strcmp(exp_obj->u.object_val.members[m].key, path)) { found = (int)m; break; }
            if (found < 0) { fprintf(stderr, "[t262 UNEXPECTED false-accept] %s\n", path); fails++; }
            else {
                ShJsonValue *ent = exp_obj->u.object_val.members[found].value;
                const char *cat = sh_json_as_string(sh_json_get(ent, "category"), "");
                if (strcmp(cat, "static-semantic-omission") != 0) { fprintf(stderr, "[t262 wrong category] %s\n", path); fails++; }
                else exp_used[found]++;
            }
        }
        else if (P == V_REJECT && O == V_ACCEPT) { false_reject++; fails++; fprintf(stderr, "[t262 false-reject] %s :: %.180s\n", path, raw ? raw : ""); }
        else agree++;
        free(raw);
    }

    /* ---- bijection: every disk .js is in the manifest, and vice versa ---- */
    /* build a sorted manifest-path vector to compare against the sorted disk vector */
    PathVec mpaths = {0};
    for (size_t i = 0; i < sh_json_array_len(cases); i++) {
        const char *path = sh_json_as_string(sh_json_get(sh_json_array_get(cases, i), "path"), "");
        char full[4096]; snprintf(full, sizeof(full), "%s/%s", T262_CASES, path);
        pv_push(&mpaths, full);
    }
    qsort(disk.items, disk.len, sizeof(char *), cmp_str);
    qsort(mpaths.items, mpaths.len, sizeof(char *), cmp_str);
    ASSERT_EQ_MSG((int)disk.len, (int)mpaths.len, "disk .js count == manifest count (bijection)");
    for (size_t i = 0; i < disk.len && i < mpaths.len; i++)
        ASSERT_TRUE_MSG(strcmp(disk.items[i], mpaths.items[i]) == 0, "every disk .js appears in the manifest and vice versa");

    /* ---- closed both ways: no stale expectation, no unused inventory key ---- */
    int stale = 0;
    for (size_t m = 0; m < exp_n; m++)
        if (exp_used[m] != 1) { stale++; fprintf(stderr, "[t262 stale/unexercised expectation] %s (used=%d)\n", exp_obj->u.object_val.members[m].key, exp_used[m]); }
    int unused_inv = 0;
    for (size_t m = 0; m < inv_n; m++)
        if (inv_used[m] == 0) { unused_inv++; fprintf(stderr, "[t262 unused inventory key] %s\n", inv_obj->u.object_val.members[m].key); }

    fprintf(stderr, "\ntest262: enumerated=%d read=%d hashed=%d analyzed=%d positive=%d parse-negative=%d\n"
            "test262: agree=%d false-reject=%d false-accept=%d unsupported=%d unsup-reject=%d divergence=%d indeterminate=%d\n"
            "test262: expectations=%d (all exercised once: %s) inventory=%d (all exercised: %s)\n",
            enumerated, read_ok, hashed, analyzed, positive, parse_negative,
            agree, false_reject, false_accept, unsupported, unsup_reject, divergence, indeterminate,
            (int)exp_n, stale ? "NO" : "yes", (int)inv_n, unused_inv ? "NO" : "yes");

    /* ---- gates ---- */
    EXPECT_EQ(read_ok, enumerated);
    EXPECT_EQ(hashed, enumerated);
    EXPECT_EQ(analyzed, enumerated);
    EXPECT_EQ(analyzed, count);                 /* every manifest case analyzed */
    EXPECT_EQ(false_reject, 0);
    EXPECT_EQ(indeterminate, 0);
    EXPECT_EQ(divergence, 0);
    EXPECT_EQ(stale, 0);
    EXPECT_EQ(unused_inv, 0);
    EXPECT_EQ(fails, 0);                         /* every unexpected false-accept/unsupported/unsupported-reject */

    free(exp_used); free(inv_used); free(man_bytes); free(exp_bytes);
    pv_free(&disk); pv_free(&mpaths);
    sh_arena_free(arena);
    JS_FreeContext(octx); JS_FreeRuntime(ort);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
