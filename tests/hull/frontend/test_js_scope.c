/*
 * test_js_scope.c - Slice 4: the structural scope / binding resolver (hull:source:scope),
 * driven through hull:source:resolveScope. Asserts on the JSON { ok, bindings, refs } model.
 *
 * Covers the locked semantics (docs/js_frontend_slice4_scope.md): function-body lexical scope
 * vs function-scoped params/var, recursive var collection stopping at nested functions,
 * parameter-default forward resolution, loop-head visibility + body nesting, catch env + body,
 * switch-wide lexical scope, redeclaration coalescing + first-binding lookup, named
 * function/class-expression self-bindings, compound read-then-write refs, closure/global
 * classification, range-sorted output, and structured js.internal recovery.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }
static int count(const char *hay, const char *needle) {
    int c = 0; size_t nl = strlen(needle);
    if (!hay || nl == 0) return 0;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += nl) c++;
    return c;
}
static char *scope_of(HlJsSession *s, const char *src)
{
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", "resolveScope",
                          (const uint8_t *)src, strlen(src), "a.js", NULL, 0, &out, &out_len);
    return out;
}

/* Function-body lexical scope vs function-scoped params/var: a param + var live in the function
 * scope; a body-top let lives in a nested block scope of the same function. */
UTEST(js_scope, function_body_lexical_vs_params_var)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "function f(p) { var v; let x = 1; }");
    ASSERT_TRUE(o != NULL);
    EXPECT_TRUE(has(o, "\"ok\":true"));
    EXPECT_TRUE(has(o, "\"name\":\"p\",\"kind\":\"param\",\"scope\":\"function\""));
    EXPECT_TRUE(has(o, "\"name\":\"v\",\"kind\":\"var\",\"scope\":\"function\""));
    EXPECT_TRUE(has(o, "\"name\":\"x\",\"kind\":\"let\",\"scope\":\"block\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Recursive var collection: a var deep inside nested blocks/if/for hoists to the function; a var
 * inside a NESTED FUNCTION does not leak out. */
UTEST(js_scope, recursive_var_stops_at_functions)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "function outer() { if (c) { for (;;) { var deep = 1; } } function inner() { var hidden = 2; } }");
    /* `deep` hoisted to outer (function scope, funcId 1) */
    EXPECT_TRUE(has(o, "\"name\":\"deep\",\"kind\":\"var\",\"scope\":\"function\",\"funcId\":1"));
    /* `hidden` belongs to inner (funcId 2), NOT outer */
    EXPECT_TRUE(has(o, "\"name\":\"hidden\",\"kind\":\"var\",\"scope\":\"function\",\"funcId\":2"));
    free(o);
    hl_js_session_destroy(s);
}

/* Parameter-default forward resolution: all params predeclared, so a default referencing a later
 * param resolves to the param binding (local), not a global. */
UTEST(js_scope, param_default_forward)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "function g(a = b, b = 1) { return a; }");
    /* the `b` in a's default (a read) resolves local to the param b, not global */
    EXPECT_TRUE(has(o, "\"name\":\"b\",\"range\":{\"start\":16,\"stop\":17},\"kind\":\"local\",\"access\":\"read\""));
    EXPECT_FALSE(has(o, "\"name\":\"b\",\"range\":{\"start\":16,\"stop\":17},\"kind\":\"global\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Loop-head visibility (incl. self-reference in the RHS) + body nesting. */
UTEST(js_scope, loop_head_and_body)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* for (const x of x): the RHS x resolves to the loop binding (reads incl. the RHS + body) */
    char *o = scope_of(s, "for (const x of x) use(x);");
    EXPECT_TRUE(has(o, "\"name\":\"x\",\"kind\":\"const\",\"scope\":\"loop-head\""));
    EXPECT_TRUE(has(o, "\"reads\":2"));                       /* RHS x + body x both resolved local */
    free(o); o = NULL;
    /* classic for (let i = i; ...): the initializer i resolves to the loop binding */
    o = scope_of(s, "for (let i = i; i < 3; i++) {}");
    EXPECT_TRUE(has(o, "\"name\":\"i\",\"kind\":\"let\",\"scope\":\"loop-head\""));
    EXPECT_FALSE(has(o, "\"name\":\"i\",\"range\":{\"start\":14,\"stop\":15},\"kind\":\"global\""));
    free(o); o = NULL;
    /* a var loop head hoists to the function/module, not a loop-head binding */
    o = scope_of(s, "for (var j = 0; j < 3; j++) {}");
    EXPECT_TRUE(has(o, "\"name\":\"j\",\"kind\":\"var\",\"scope\":\"module\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Catch environment + body block: the catch param is a catch binding, visible in the body. */
UTEST(js_scope, catch_env)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "try { f(); } catch (e) { let m = e; }");
    EXPECT_TRUE(has(o, "\"name\":\"e\",\"kind\":\"catch\",\"scope\":\"catch\""));
    EXPECT_TRUE(has(o, "\"name\":\"m\",\"kind\":\"let\",\"scope\":\"block\""));
    /* the body read of e resolves to the catch binding (a read) */
    EXPECT_TRUE(has(o, "\"name\":\"e\",\"range\":{\"start\":34,\"stop\":35},\"kind\":\"local\",\"access\":\"read\""));
    free(o); o = NULL;
    /* catch {} (no param) binds nothing */
    o = scope_of(s, "try { f(); } catch { g(); }");
    EXPECT_FALSE(has(o, "\"kind\":\"catch\""));
    EXPECT_TRUE(has(o, "\"ok\":true"));
    free(o);
    hl_js_session_destroy(s);
}

/* Switch is ONE shared block scope: a let in one case is visible in a later case. */
UTEST(js_scope, switch_wide_block)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "switch (v) { case 1: let a = 1; break; case 2: use(a); }");
    /* one `a` binding, block scope; the case-2 read resolves to it (reads >= 1) */
    EXPECT_EQ(count(o, "\"name\":\"a\",\"kind\":\"let\""), 1);
    EXPECT_TRUE(has(o, "\"name\":\"a\",\"kind\":\"let\",\"scope\":\"block\""));
    EXPECT_TRUE(has(o, "\"reads\":1"));
    free(o);
    hl_js_session_destroy(s);
}

/* Redeclaration coalescing + deterministic first-binding lookup. */
UTEST(js_scope, redeclaration_identity)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* param + var same name -> ONE binding, kind param (earliest by range), no shadow */
    char *o = scope_of(s, "function f(x) { var x; }");
    EXPECT_EQ(count(o, "\"name\":\"x\""), 1);
    EXPECT_TRUE(has(o, "\"name\":\"x\",\"kind\":\"param\""));
    EXPECT_TRUE(has(o, "\"shadows\":null"));
    free(o); o = NULL;
    /* var + function same module scope -> one binding, function (earliest is `function g` at col 1) */
    o = scope_of(s, "function g(){} var g;");
    EXPECT_EQ(count(o, "\"name\":\"g\""), 1);
    EXPECT_TRUE(has(o, "\"name\":\"g\",\"kind\":\"function\""));
    free(o); o = NULL;
    /* duplicate lexical: TWO records for fidelity, but references resolve to the FIRST */
    o = scope_of(s, "let d = 1; let d = 2; use(d);");
    EXPECT_EQ(count(o, "\"name\":\"d\",\"kind\":\"let\""), 2);   /* both records kept */
    /* the read of d resolves to the FIRST declaration (declRange start 5) */
    EXPECT_TRUE(has(o, "\"access\":\"read\",\"declRange\":{\"start\":5,\"stop\":6}"));
    free(o);
    hl_js_session_destroy(s);
}

/* Named function-expression + class-expression self-bindings are body-only. */
UTEST(js_scope, expression_self_bindings)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* const g = function f(){ f(); }: f bound inside (a local self-ref), not in module scope */
    char *o = scope_of(s, "const g = function f() { return f; };");
    EXPECT_TRUE(has(o, "\"name\":\"f\",\"kind\":\"function\",\"scope\":\"function\""));
    EXPECT_TRUE(has(o, "\"name\":\"f\",\"range\":{\"start\":33,\"stop\":34},\"kind\":\"local\""));
    /* only g is at module scope; f is not visible outside */
    EXPECT_TRUE(has(o, "\"name\":\"g\",\"kind\":\"const\",\"scope\":\"module\""));
    free(o); o = NULL;
    /* const K = class C { m(){ return C; } }: C bound inside the class scope (17-18); a method is
     * a function, so the body ref to C is a CLOSURE to the class-name binding. */
    o = scope_of(s, "const K = class C { m() { return C; } };");
    EXPECT_TRUE(has(o, "\"name\":\"C\",\"kind\":\"class\""));
    EXPECT_TRUE(has(o, "\"kind\":\"closure\",\"access\":\"read\",\"declRange\":{\"start\":17,\"stop\":18}"));
    free(o);
    hl_js_session_destroy(s);
}

/* Compound access emits a read ref THEN a write ref at the same range; counters both increment. */
UTEST(js_scope, compound_read_then_write)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "let n = 0; n += 1; n++;");
    /* n binding: reads 2, writes 2 */
    EXPECT_TRUE(has(o, "\"name\":\"n\",\"kind\":\"let\",\"scope\":\"module\",\"funcId\":0,\"scopeId\":1,\"range\":{\"start\":5,\"stop\":6},\"reads\":2,\"writes\":2"));
    /* n += 1 at range 12-13: a read then a write, both at the same range */
    EXPECT_TRUE(has(o, "\"range\":{\"start\":12,\"stop\":13},\"kind\":\"local\",\"access\":\"read\""));
    EXPECT_TRUE(has(o, "\"range\":{\"start\":12,\"stop\":13},\"kind\":\"local\",\"access\":\"write\""));
    /* n++ at range 20-21: read then write */
    EXPECT_TRUE(has(o, "\"range\":{\"start\":20,\"stop\":21},\"kind\":\"local\",\"access\":\"read\""));
    EXPECT_TRUE(has(o, "\"range\":{\"start\":20,\"stop\":21},\"kind\":\"local\",\"access\":\"write\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Closure vs global classification + property/reference rules. */
UTEST(js_scope, closure_global_and_property)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "const y = 1; function k() { return y + z; }");
    EXPECT_TRUE(has(o, "\"kind\":\"closure\",\"access\":\"read\",\"declRange\":{\"start\":7,\"stop\":8}"));  /* y crosses a function boundary */
    EXPECT_TRUE(has(o, "\"name\":\"z\",\"range\":{\"start\":40,\"stop\":41},\"kind\":\"global\",\"access\":\"read\",\"declRange\":null"));  /* z unbound */
    free(o); o = NULL;
    /* a.b: `a` is a ref (read), `b` (property name) is NOT a reference */
    o = scope_of(s, "const a = {}; a.b;");
    EXPECT_TRUE(has(o, "\"kind\":\"local\",\"access\":\"read\",\"declRange\":{\"start\":7,\"stop\":8}"));
    EXPECT_FALSE(has(o, "\"name\":\"b\""));                    /* property name never a ref */
    free(o); o = NULL;
    /* assignment: x = write (no read); shorthand object { x }: x is a read at 32-33 */
    o = scope_of(s, "let x = 0; x = 1; const o2 = { x };");
    EXPECT_TRUE(has(o, "\"name\":\"x\",\"range\":{\"start\":12,\"stop\":13},\"kind\":\"local\",\"access\":\"write\""));
    EXPECT_TRUE(has(o, "\"name\":\"x\",\"range\":{\"start\":32,\"stop\":33},\"kind\":\"local\",\"access\":\"read\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Imports are module-scoped bindings; export { a } reads the local binding. */
UTEST(js_scope, imports_and_exports)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "import def, { a, b as c } from \"m\";\nimport * as ns from \"n\";\nfunction f() { return a + ns; }");
    EXPECT_TRUE(has(o, "\"name\":\"def\",\"kind\":\"import\",\"scope\":\"module\""));
    EXPECT_TRUE(has(o, "\"name\":\"c\",\"kind\":\"import\""));
    EXPECT_TRUE(has(o, "\"name\":\"ns\",\"kind\":\"import\""));
    /* a reference to an import from inside a function is a CLOSURE to an import binding (a at 15-16) */
    EXPECT_TRUE(has(o, "\"kind\":\"closure\",\"access\":\"read\",\"declRange\":{\"start\":15,\"stop\":16}"));
    free(o); o = NULL;
    /* export { a } reads the local binding a (at 7-8) */
    o = scope_of(s, "const a = 1; export { a };");
    EXPECT_TRUE(has(o, "\"access\":\"read\",\"declRange\":{\"start\":7,\"stop\":8}"));
    free(o);
    hl_js_session_destroy(s);
}

/* Shadowing: an inner block binding shadows an outer, with the shadow range pointing at the outer. */
UTEST(js_scope, shadowing)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = scope_of(s, "let w = 1;\n{ let w = 2; }");
    /* outer w at 5-6; inner w shadows it */
    EXPECT_TRUE(has(o, "\"range\":{\"start\":18,\"stop\":19},\"reads\":0,\"writes\":0,\"shadows\":{\"start\":5,\"stop\":6}"));
    free(o);
    hl_js_session_destroy(s);
}

/* bindings[] is sorted by declaration range regardless of the predeclare traversal order. */
UTEST(js_scope, range_sorted_output)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* a hoisted function is predeclared before the top let, but must sort AFTER it by range */
    char *o = scope_of(s, "let first = 1; function second() {}");
    const char *pf = strstr(o, "\"name\":\"first\"");
    const char *ps = strstr(o, "\"name\":\"second\"");
    EXPECT_TRUE(pf != NULL && ps != NULL);
    EXPECT_TRUE(pf < ps);                                     /* source order: first before second */
    free(o);
    hl_js_session_destroy(s);
}

/* Failure contract: a recovered syntax-error AST yields ok:true with a partial model and no
 * js.internal; a forced internal defect yields ok:false + one js.internal, never a partial ok. */
UTEST(js_scope, failure_contract)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* recovered syntax error: local degradation, ok:true */
    char *o = scope_of(s, "const = ;\nconst y = 2;\nuse(y);");
    EXPECT_TRUE(has(o, "\"ok\":true"));
    EXPECT_FALSE(has(o, "\"code\":\"js.internal\""));
    EXPECT_TRUE(has(o, "\"name\":\"y\""));                     /* the recovered part still resolved */
    free(o); o = NULL;
    /* forced internal defect: ok:false, exactly one js.internal, no ok:true */
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", "resolveScope",
                          (const uint8_t *)"const x = 1;", 12, "a.js", "{\"corruptAst\":true}", 19, &out, &out_len);
    EXPECT_TRUE(has(out, "\"ok\":false"));
    EXPECT_FALSE(has(out, "\"ok\":true"));
    EXPECT_EQ(count(out, "\"code\":\"js.internal\""), 1);
    free(out);
    hl_js_session_destroy(s);
}

/* Corpus regression: resolving every committed application-JS file yields ok:true with no
 * js.internal - the whole real corpus goes through the resolver without an internal fault.
 * Fail-closed enumeration (regular files, no symlinks, static/node_modules/dot dirs excluded). */
static int has_js_ext(const char *name) {
    const char *d = strrchr(name, '.');
    return d && (!strcmp(d, ".js") || !strcmp(d, ".mjs") || !strcmp(d, ".cjs"));
}
static int scope_walk(HlJsSession *s, const char *dir, int *okc, int *badc)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e; int rc = 0;
    for (;;) {
        errno = 0; e = readdir(d);
        if (!e) { if (errno != 0) rc = -1; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        int k = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (k < 0 || (size_t)k >= sizeof(path)) { rc = -1; break; }
        struct stat stt;
        if (lstat(path, &stt) != 0) { rc = -1; break; }
        if (S_ISLNK(stt.st_mode)) continue;
        if (S_ISDIR(stt.st_mode)) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "static") || !strcmp(e->d_name, "node_modules")) continue;
            if (scope_walk(s, path, okc, badc) != 0) { rc = -1; break; }
        } else if (S_ISREG(stt.st_mode) && has_js_ext(e->d_name)) {
            int fd = open(path, O_RDONLY);
            if (fd < 0) { rc = -1; break; }
            struct stat fs; if (fstat(fd, &fs) != 0) { close(fd); rc = -1; break; }
            size_t n = (size_t)fs.st_size; char *buf = (char *)malloc(n + 1);
            if (!buf) { close(fd); rc = -1; break; }
            size_t got = 0; ssize_t r;
            while (got < n && (r = read(fd, buf + got, n - got)) != 0) { if (r < 0) { if (errno == EINTR) continue; break; } got += (size_t)r; }
            close(fd);
            if (got != n) { free(buf); rc = -1; break; }
            buf[n] = '\0';
            char *out = NULL; size_t out_len = 0;
            int arc = hl_js_session_analyze(s, "hull:source:lextest", "resolveScope", (const uint8_t *)buf, n, path, NULL, 0, &out, &out_len);
            if (arc == 0 && out && strstr(out, "\"ok\":true") && !strstr(out, "\"code\":\"js.internal\"")) (*okc)++;
            else { (*badc)++; fprintf(stderr, "[scope corpus] %s :: %.160s\n", path, out ? out : "(null)"); }
            free(out); free(buf);
        }
    }
    if (closedir(d) != 0 && rc == 0) rc = -1;
    return rc;
}
UTEST(js_scope, corpus_regression)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *roots[] = { "stdlib/js/hull", "examples", "tests/fixtures" };
    int okc = 0, badc = 0;
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++)
        ASSERT_EQ_MSG(scope_walk(s, roots[r], &okc, &badc), 0, "corpus enumeration must not fail-open");
    fprintf(stderr, "\nscope corpus: ok=%d bad=%d\n", okc, badc);
    EXPECT_TRUE(okc >= 150);       /* the ~165-file corpus all resolved */
    EXPECT_EQ(badc, 0);            /* zero internal faults / non-ok results */
    hl_js_session_destroy(s);
}

UTEST_MAIN()
