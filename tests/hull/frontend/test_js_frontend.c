/*
 * test_js_frontend.c - Slice 5: the JS frontend adapter (hull:source:frontend_javascript),
 * driven through the frontendAnalyze / frontendSemantics / frontendScope drivers. Asserts on the
 * normalized facts, declaration_semantics records, the scope capability, and the handle lifetime.
 *
 * decl_id / unit_id are session-local counters, so each test uses a FRESH session (decl_ids start
 * at 1 in pre-order walk order); the id passed to semantics is that deterministic value.
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
static char *fe_analyze(HlJsSession *s, const char *src) {
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", "frontendAnalyze",
                          (const uint8_t *)src, strlen(src), "a.js", NULL, 0, &out, &out_len);
    return out;
}
static char *fe_call(HlJsSession *s, const char *method, int id, const char *key) {
    char opts[64]; snprintf(opts, sizeof(opts), "{\"%s\":%d}", key, id);
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", method,
                          (const uint8_t *)"", 0, "a.js", opts, strlen(opts), &out, &out_len);
    return out;
}
static char *fe_sem(HlJsSession *s, int declId) { return fe_call(s, "frontendSemantics", declId, "declId"); }
static char *fe_scope(HlJsSession *s, int unitId) { return fe_call(s, "frontendScope", unitId, "unitId"); }
static char *fe_analyze_fail(HlJsSession *s, const char *src) {
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", "frontendAnalyzeFail",
                          (const uint8_t *)src, strlen(src), "a.js", NULL, 0, &out, &out_len);
    return out;
}
static char *fe_mutate(HlJsSession *s, const char *spec) {
    char *out = NULL; size_t out_len = 0;
    hl_js_session_analyze(s, "hull:source:lextest", "frontendMutate",
                          (const uint8_t *)"", 0, "a.js", spec, strlen(spec), &out, &out_len);
    return out;
}

/* The facts shape: status, unit_id, diagnostics, and the exact Decl shape with normalized ranges;
 * every facts range is { start, stop, line, col }. */
UTEST(js_frontend, facts_shape)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fe_analyze(s, "/** @query users */\nexport const getUsers = 1;");
    ASSERT_TRUE(o != NULL);
    EXPECT_TRUE(has(o, "\"status\":\"analyzed\""));
    EXPECT_TRUE(has(o, "\"unit_id\":1"));
    EXPECT_TRUE(has(o, "\"diagnostics\":[]"));
    /* kind/name/range{start,stop,line,col}/group_range/is_method/annotations/decl_id */
    EXPECT_TRUE(has(o, "\"kind\":\"const\",\"name\":\"getUsers\""));
    EXPECT_TRUE(has(o, "\"range\":{\"start\":34,\"stop\":42,\"line\":2,\"col\":14}"));   /* line/col derived */
    EXPECT_TRUE(has(o, "\"group_range\":{\"start\":"));
    EXPECT_TRUE(has(o, "\"is_method\":false"));
    EXPECT_TRUE(has(o, "\"decl_id\":1"));
    /* annotation text renamed to value, range normalized (field order name, value, raw, range) */
    EXPECT_TRUE(has(o, "\"name\":\"query\",\"value\":\"users\",\"raw\":\"@query users\",\"range\":{\"start\":5,\"stop\":17,\"line\":1,\"col\":5}"));
    free(o);
    hl_js_session_destroy(s);
}

/* Full-AST parity: an annotated declaration nested inside a function, block, loop, catch, and a
 * class method body is collected; a method definition itself yields no fact; the export wrapper is
 * not double-collected. */
UTEST(js_frontend, nested_declarations)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fe_analyze(s,
        "function outer() {\n"
        "  /** @a */\n  const inNested = 1;\n"
        "  {\n  function inBlock() {} }\n"
        "  for (;;) {\n  class InLoop {} }\n"
        "  try {} catch (e) {\n  const inCatch = 2; }\n"
        "}\n"
        "class Host {\n  m() {\n  /** @e */\n  const inMethod = 3; } }");
    /* every nested declaration is collected (function/block/loop/catch/method-body) */
    EXPECT_TRUE(has(o, "\"name\":\"inNested\""));
    EXPECT_TRUE(has(o, "\"name\":\"inBlock\""));
    EXPECT_TRUE(has(o, "\"name\":\"InLoop\""));
    EXPECT_TRUE(has(o, "\"name\":\"inCatch\""));
    EXPECT_TRUE(has(o, "\"name\":\"inMethod\""));
    /* nested annotations attach (own-line JSDoc above the decl): @a in a function body,
     * @e in a class method body */
    EXPECT_TRUE(has(o, "\"name\":\"a\""));
    EXPECT_TRUE(has(o, "\"name\":\"e\""));
    /* the method definition `m` itself is NOT a declaration fact */
    EXPECT_FALSE(has(o, "\"name\":\"m\""));
    /* outer + Host are collected once each (not doubled) */
    EXPECT_EQ(count(o, "\"name\":\"outer\""), 1);
    EXPECT_EQ(count(o, "\"name\":\"Host\""), 1);
    free(o);
    hl_js_session_destroy(s);
}

/* One fact per NAME sharing the declaration node's group_range; the group's annotation is shared. */
UTEST(js_frontend, one_per_name_group)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* a multi-declarator with a shared @g annotation */
    char *o = fe_analyze(s, "/** @g */\nconst a = 1, b = 2;");
    EXPECT_TRUE(has(o, "\"name\":\"a\""));
    EXPECT_TRUE(has(o, "\"name\":\"b\""));
    /* both share ONE group_range (the VariableDeclaration at byte 11) and BOTH carry @g */
    EXPECT_EQ(count(o, "\"group_range\":{\"start\":11,"), 2);
    EXPECT_EQ(count(o, "\"name\":\"g\""), 2);              /* the annotation shared across the group */
    free(o); o = NULL;
    /* destructuring: two names per pattern, shared group */
    o = fe_analyze(s, "const [ok, err] = f(); const {p, q: r} = o;");
    EXPECT_TRUE(has(o, "\"name\":\"ok\""));
    EXPECT_TRUE(has(o, "\"name\":\"err\""));
    EXPECT_TRUE(has(o, "\"name\":\"p\""));
    EXPECT_TRUE(has(o, "\"name\":\"r\""));
    free(o);
    hl_js_session_destroy(s);
}

/* Kinds + exports: const/let/var/function/class; exported + named-default produce facts; an
 * anonymous default produces none. */
UTEST(js_frontend, kinds_and_exports)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fe_analyze(s, "const c = 1; let l = 2; var v = 3; function f(){} class K {}");
    EXPECT_TRUE(has(o, "\"kind\":\"const\",\"name\":\"c\""));
    EXPECT_TRUE(has(o, "\"kind\":\"let\",\"name\":\"l\""));
    EXPECT_TRUE(has(o, "\"kind\":\"var\",\"name\":\"v\""));
    EXPECT_TRUE(has(o, "\"kind\":\"function\",\"name\":\"f\""));
    EXPECT_TRUE(has(o, "\"kind\":\"class\",\"name\":\"K\""));
    free(o); o = NULL;
    /* exported + named default produce facts */
    o = fe_analyze(s, "export function ef(){} export default class DC {}");
    EXPECT_TRUE(has(o, "\"name\":\"ef\""));
    EXPECT_TRUE(has(o, "\"name\":\"DC\""));
    free(o); o = NULL;
    /* anonymous default -> no declaration fact */
    o = fe_analyze(s, "export default function () {}");
    EXPECT_TRUE(has(o, "\"declarations\":[]"));
    free(o);
    hl_js_session_destroy(s);
}

/* declaration_semantics value + multi-declarator identity: q <= bar(); a bare let has a null
 * initializer (not an error). */
UTEST(js_frontend, semantics_value)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* const a = foo(), q = bar(), c = baz(); -> a=1, q=2, c=3 */
    char *o = fe_analyze(s, "const a = foo(), q = bar(), c = baz();");
    free(o);
    char *sem = fe_sem(s, 2);                              /* q */
    EXPECT_TRUE(has(sem, "\"form\":\"value\",\"kind\":\"const\",\"declarator_index\":1,\"binding_path\":[]"));
    EXPECT_TRUE(has(sem, "\"initializer\":{\"type\":\"CallExpression\",\"start\":22,\"stop\":27"));
    EXPECT_TRUE(has(sem, "\"name\":\"bar\""));
    free(sem);
    hl_js_session_destroy(s);

    /* bare let x; -> initializer null, NOT an error */
    HlJsSession *s2 = hl_js_session_create(NULL);
    char *o2 = fe_analyze(s2, "let x;");
    free(o2);
    char *sem2 = fe_sem(s2, 1);
    EXPECT_TRUE(has(sem2, "\"form\":\"value\""));
    EXPECT_TRUE(has(sem2, "\"initializer\":null"));
    EXPECT_FALSE(has(sem2, "\"code\":\"js.internal\""));
    free(sem2);
    hl_js_session_destroy(s2);
}

/* declaration_semantics structural binding paths: array/property index, rest, assignment,
 * object-rest indexing the property array. */
UTEST(js_frontend, semantics_binding_paths)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* [a, b] -> a=1 []; b=2 [{array_index:1}] */
    char *o = fe_analyze(s, "const [a, b] = xs;");
    free(o);
    char *sem = fe_sem(s, 2);
    EXPECT_TRUE(has(sem, "\"binding_path\":[{\"array_index\":1}]"));
    free(sem);
    hl_js_session_destroy(s);

    /* {q: r} -> r=1 [{property_index:0}] */
    HlJsSession *s2 = hl_js_session_create(NULL);
    fe_analyze(s2, "const {q: r} = o;");
    char *sem2 = fe_sem(s2, 1);
    EXPECT_TRUE(has(sem2, "\"binding_path\":[{\"property_index\":0}]"));
    free(sem2);
    hl_js_session_destroy(s2);

    /* {a, ...rest} -> a=1 [{property_index:0}]; rest=2 [{property_index:1},{rest:true}] */
    HlJsSession *s3 = hl_js_session_create(NULL);
    fe_analyze(s3, "const {a, ...rest} = o;");
    char *sa = fe_sem(s3, 1);
    EXPECT_TRUE(has(sa, "\"binding_path\":[{\"property_index\":0}]"));
    free(sa);
    char *sr = fe_sem(s3, 2);
    EXPECT_TRUE(has(sr, "\"binding_path\":[{\"property_index\":1},{\"rest\":true}]"));
    free(sr);
    hl_js_session_destroy(s3);

    /* {x = 1} default -> [{property_index:0},{assignment:true}]; [...r] -> [{array_index:0},{rest:true}] */
    HlJsSession *s4 = hl_js_session_create(NULL);
    fe_analyze(s4, "const {x = 1} = o;");
    char *sd = fe_sem(s4, 1);
    EXPECT_TRUE(has(sd, "\"binding_path\":[{\"property_index\":0},{\"assignment\":true}]"));
    free(sd);
    hl_js_session_destroy(s4);

    HlJsSession *s5 = hl_js_session_create(NULL);
    fe_analyze(s5, "const [...r] = xs;");
    char *se = fe_sem(s5, 1);
    EXPECT_TRUE(has(se, "\"binding_path\":[{\"array_index\":0},{\"rest\":true}]"));
    free(se);
    hl_js_session_destroy(s5);
}

/* declaration_semantics function + class shapes. */
UTEST(js_frontend, semantics_function_class)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    fe_analyze(s, "async function g(p, q) { return p; }");
    char *sf = fe_sem(s, 1);
    EXPECT_TRUE(has(sf, "\"form\":\"function\",\"is_async\":true,\"is_generator\":false"));
    EXPECT_TRUE(has(sf, "\"params\":["));
    EXPECT_TRUE(has(sf, "\"body\":{\"type\":\"BlockStatement\""));
    free(sf);
    hl_js_session_destroy(s);

    HlJsSession *s2 = hl_js_session_create(NULL);
    fe_analyze(s2, "class C extends B { m() {} }");
    char *sc = fe_sem(s2, 1);
    EXPECT_TRUE(has(sc, "\"form\":\"class\""));
    EXPECT_TRUE(has(sc, "\"super_class\":{\"type\":\"Identifier\""));
    EXPECT_TRUE(has(sc, "\"body\":["));
    free(sc);
    hl_js_session_destroy(s2);
}

/* analyze() is TRANSACTIONAL: a forced mid-collection failure returns unit_id:-1 + js.internal,
 * leaves NO partial unit_id/decl_id resolvable, and keeps counters monotonic (ids not reused). */
UTEST(js_frontend, analyze_transactional)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *f = fe_analyze_fail(s, "const a = 1; const b = 2;");
    EXPECT_TRUE(has(f, "\"status\":\"error\""));
    EXPECT_TRUE(has(f, "\"unit_id\":-1"));
    EXPECT_TRUE(has(f, "\"code\":\"js.internal\""));
    free(f);
    /* the rolled-back unit/decl (would have been unit_id 1 / decl_id 1) is NOT resolvable */
    char *sd = fe_sem(s, 1);   EXPECT_TRUE(has(sd, "\"error\""));    free(sd);
    char *su = fe_scope(s, 1); EXPECT_TRUE(has(su, "\"ok\":false")); free(su);
    /* counters stayed monotonic: the next successful analyze gets a HIGHER unit_id (not the reused 1) */
    char *o = fe_analyze(s, "const z = 1;");
    EXPECT_TRUE(has(o, "\"unit_id\":2"));
    EXPECT_FALSE(has(o, "\"unit_id\":1"));
    free(o);
    hl_js_session_destroy(s);
}

/* Corrupt-state: an unknown id, and every ISOLATED retained-identity mutation, returns exactly one
 * js.internal error and NO plausible semantic record. */
UTEST(js_frontend, semantics_corrupt_state)
{
    HlJsSession *s0 = hl_js_session_create(NULL);
    ASSERT_TRUE(s0 != NULL);
    char *o = fe_sem(s0, 9999);                            /* never issued */
    EXPECT_TRUE(has(o, "\"code\":\"js.internal\""));
    free(o);
    hl_js_session_destroy(s0);

    /* each: fresh session (decl_id 1), analyze, mutate the retained state, then semantics(1) */
    struct { const char *label; const char *src; const char *spec; } cases[] = {
        { "kind mismatch",       "const a = 1;",   "{\"declId\":1,\"declField\":\"kind\",\"declValue\":\"let\"}" },
        { "name mismatch",       "const a = 1;",   "{\"declId\":1,\"declField\":\"name\",\"declValue\":\"wrong\"}" },
        { "bad declarator idx",  "const a = 1;",   "{\"declId\":1,\"declField\":\"declarator_index\",\"declValue\":999}" },
        { "bad binding path",    "const [a] = x;", "{\"declId\":1,\"declField\":\"binding_path\",\"declValue\":[{\"array_index\":99}]}" },
        { "malformed fn body",   "function f(){}", "{\"declId\":1,\"nodeField\":\"body\",\"nodeValue\":null}" },
        { "malformed class body","class C {}",     "{\"declId\":1,\"nodeField\":\"body\",\"nodeValue\":null}" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        HlJsSession *s = hl_js_session_create(NULL);
        ASSERT_TRUE_MSG(s != NULL, cases[i].label);
        char *a = fe_analyze(s, cases[i].src); free(a);
        char *m = fe_mutate(s, cases[i].spec); free(m);
        char *sem = fe_sem(s, 1);
        EXPECT_EQ_MSG(count(sem, "\"code\":\"js.internal\""), 1, cases[i].label);   /* exactly one error */
        EXPECT_FALSE_MSG(has(sem, "\"form\":"), cases[i].label);                     /* no live record */
        free(sem);
        hl_js_session_destroy(s);
    }

    /* node from ANOTHER retained unit: analyze two units, point decl_id 1's unit_id at unit 2 (its
     * node belongs to unit 1) -> the ownership check rejects it. */
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *u1 = fe_analyze(s, "const a = 1;"); free(u1);   /* unit 1, decl 1 */
    char *u2 = fe_analyze(s, "const b = 2;"); free(u2);   /* unit 2 */
    char *m = fe_mutate(s, "{\"declId\":1,\"declField\":\"unit_id\",\"declValue\":2}"); free(m);
    char *sem = fe_sem(s, 1);
    EXPECT_EQ(count(sem, "\"code\":\"js.internal\""), 1);
    EXPECT_FALSE(has(sem, "\"form\":"));
    free(sem);
    hl_js_session_destroy(s);
}

/* The scope capability reached through the adapter: a valid unit_id returns the Slice-4 model;
 * an unknown unit_id returns an error. */
UTEST(js_frontend, scope_capability)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fe_analyze(s, "const y = 1; function k() { return y; }");
    free(o);
    char *sc = fe_scope(s, 1);
    EXPECT_TRUE(has(sc, "\"ok\":true"));
    EXPECT_TRUE(has(sc, "\"bindings\":["));
    EXPECT_TRUE(has(sc, "\"kind\":\"closure\""));           /* y from inside k */
    free(sc);
    char *bad = fe_scope(s, 9999);
    EXPECT_TRUE(has(bad, "\"ok\":false"));
    EXPECT_TRUE(has(bad, "\"code\":\"js.internal\""));
    free(bad);
    hl_js_session_destroy(s);
}

/* Bridge-private ids: unit_id/decl_id are integers; the facts carry no AST/JSValue; a fresh
 * analyze issues fresh ids. NOTE: a bare decl_id is SESSION-RELATIVE (a fresh session re-issues 1),
 * so this only proves per-session state isolation, not stale-generation (ABA) safety - the full
 * stale guarantee is the C-owned { session_token, unit_id, decl_id } tuple, tested in Slice 6. */
UTEST(js_frontend, handles_and_session_isolation)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fe_analyze(s, "const a = 1;");
    EXPECT_TRUE(has(o, "\"unit_id\":1"));
    EXPECT_TRUE(has(o, "\"decl_id\":1"));
    EXPECT_FALSE(has(o, "\"BlockStatement\""));            /* no AST leaks into the facts */
    EXPECT_FALSE(has(o, "\"declarations\":[{\"type\":"));   /* facts are metadata, not AST nodes */
    free(o);
    hl_js_session_destroy(s);

    /* per-session state isolation: a decl_id never issued in a fresh session is not resolvable
     * (each session has its own module state). Full ABA stale-generation safety is Slice 6. */
    HlJsSession *b = hl_js_session_create(NULL);
    char *fresh = fe_sem(b, 1);                            /* nothing analyzed in b yet */
    EXPECT_TRUE(has(fresh, "\"error\""));
    EXPECT_FALSE(has(fresh, "\"form\":"));
    free(fresh);
    hl_js_session_destroy(b);
}

/* Transport / never-raise: a recovered syntax-error source yields status:"error" + partial
 * declarations and no js.internal. */
UTEST(js_frontend, recovered_source_partial)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *o = fe_analyze(s, "const = ;\nconst y = 2;");
    EXPECT_TRUE(has(o, "\"status\":\"error\""));
    EXPECT_TRUE(has(o, "\"code\":\"js.syntax\""));
    EXPECT_FALSE(has(o, "\"code\":\"js.internal\""));
    EXPECT_TRUE(has(o, "\"name\":\"y\""));                  /* the recovered declaration still collected */
    free(o);
    hl_js_session_destroy(s);
}

/* Corpus regression: `analyze` over every committed JS file yields status in {analyzed, error}
 * with no js.internal, and a valid schema. */
static int has_js_ext(const char *name) {
    const char *d = strrchr(name, '.');
    return d && (!strcmp(d, ".js") || !strcmp(d, ".mjs") || !strcmp(d, ".cjs"));
}
static int fe_walk(HlJsSession *s, const char *dir, int *okc, int *badc)
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
            if (fe_walk(s, path, okc, badc) != 0) { rc = -1; break; }
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
            int arc = hl_js_session_analyze(s, "hull:source:lextest", "frontendAnalyze", (const uint8_t *)buf, n, path, NULL, 0, &out, &out_len);
            int ok = arc == 0 && out && !strstr(out, "\"code\":\"js.internal\"")
                     && (strstr(out, "\"status\":\"analyzed\"") || strstr(out, "\"status\":\"error\""));
            if (ok) (*okc)++; else { (*badc)++; fprintf(stderr, "[frontend corpus] %s :: %.160s\n", path, out ? out : "(null)"); }
            free(out); free(buf);
        }
    }
    if (closedir(d) != 0 && rc == 0) rc = -1;
    return rc;
}
UTEST(js_frontend, corpus_regression)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *roots[] = { "stdlib/js/hull", "examples", "tests/fixtures" };
    int okc = 0, badc = 0;
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++)
        ASSERT_EQ_MSG(fe_walk(s, roots[r], &okc, &badc), 0, "corpus enumeration must not fail-open");
    fprintf(stderr, "\nfrontend corpus: ok=%d bad=%d\n", okc, badc);
    EXPECT_TRUE(okc >= 150);
    EXPECT_EQ(badc, 0);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
