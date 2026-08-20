/*
 * test_lua_source.c — harness for the pure-Lua hull.source.lua layer.
 *
 * The source-analysis layer is pure Lua with NO Hull C dependencies, so it is
 * tested in a VANILLA lua_State (vendored Lua 5.4, no Hull sandbox / module
 * resolver): we point package.path at stdlib/cli/lua and run the co-located Lua
 * test scripts, which self-assert and return { pass, fail }. This keeps the
 * source layer's own tests in Lua (where the data lives) while surfacing pass/
 * fail to the C test runner + CI. Tests run from the repo root (see mk/tests.mk).
 *
 * The Lua 5.4 load() differential conformance corpus (later slice) belongs HERE,
 * in the harness, not in the source-analysis module — the module never calls
 * dynamic compilation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   /* lstat / S_ISLNK / dirent under -std=c11 -Wpedantic */
#endif

#include "utest.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "sh_json.h"
#include "sh_arena.h"

/* Run a co-located Lua test script in a vanilla state. Returns 0 when the script
 * ran and returned a { pass, fail } table (values via out-params); -1 on any
 * harness-level failure (state alloc, package.path, dofile raise, bad return).
 * Uses NO utest macros -- those are only valid inside a UTEST body. */
static int run_lua_test(const char *script_path, long long *pass_out, long long *fail_out)
{
    *pass_out = 0;
    *fail_out = -1;

    lua_State *L = luaL_newstate();
    if (!L) return -1;
    luaL_openlibs(L);

    /* Resolve require("hull.source.X") from the source tree (repo-root relative).
     * stdlib/lua is also on the path so hull.source.analyze can pull in hull.json
     * (the tool VM resolves it via the embedded VFS; the harness has no VFS). */
    if (luaL_dostring(L,
            "package.path = 'stdlib/cli/lua/?.lua;stdlib/cli/lua/?/init.lua;"
            "stdlib/lua/?.lua;stdlib/lua/?/init.lua;' .. package.path")
        != LUA_OK) {
        fprintf(stderr, "package.path setup failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return -1;
    }

    if (luaL_dofile(L, script_path) != LUA_OK) {
        fprintf(stderr, "\n%s: %s\n", script_path, lua_tostring(L, -1));
        lua_close(L);
        return -1;   /* the Lua script raised (a bug in the layer or the test) */
    }

    if (!lua_istable(L, -1)) { lua_close(L); return -1; }
    lua_getfield(L, -1, "fail");
    *fail_out = (long long)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "pass");
    *pass_out = (long long)lua_tointeger(L, -1);
    lua_pop(L, 1);

    fprintf(stderr, "  %s: %lld passed, %lld failed\n", script_path, *pass_out, *fail_out);
    lua_close(L);
    return 0;
}

/* ── conformance corpus: enumerate real .lua files in C (hermetic + deterministic) ──
 * The conformance suite (test_conformance.lua) needs the repo's actual Lua sources.
 * We enumerate + READ them in C and inject { path, source } records as a Lua global
 * HULL_LUA_CORPUS, so the Lua side needs no io, makes no second filesystem pass, and
 * the oracle (load()) and the Hull parser see the identical bytes. Deterministic:
 * regular .lua files only, symlinks skipped (no traversal loops), paths sorted. */

/* Fail-closed: any allocation failure latches `oom`; the runner then aborts the
 * whole conformance leg rather than testing a silently-truncated corpus. */
typedef struct { char **items; size_t count, cap; int oom; } StrList;

static void sl_push(StrList *l, const char *s)
{
    if (l->oom) return;
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        char **ni = realloc(l->items, nc * sizeof(char *));
        if (!ni) { l->oom = 1; return; }
        l->items = ni; l->cap = nc;
    }
    char *dup = strdup(s);
    if (!dup) { l->oom = 1; return; }    /* never insert NULL (would crash qsort) */
    l->items[l->count++] = dup;
}

static void sl_free(StrList *l)
{
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL; l->count = l->cap = 0;
}

static int cmp_cstr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Recursive walk. lstat (never stat) so a symlink -- file OR dir -- is skipped
 * outright, which fixes symlink handling AND makes traversal loops impossible; a
 * depth cap is a defensive backstop. Regular *.lua files only. */
static void walk_lua(const char *root, StrList *out, int depth)
{
    if (depth > 32) return;
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char path[4096];
        int w = snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        if (w < 0 || (size_t)w >= sizeof path) continue;
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;                 /* never follow symlinks */
        if (S_ISDIR(st.st_mode)) {
            /* lua54-tests is the pinned upstream corpus (its own official_lua54 leg), NOT Hull's
             * own source -- keep it out of the repo conformance walk. Its dedicated leg passes the
             * cases dir directly, so this basename skip never affects that leg. */
            if (strcmp(e->d_name, "lua54-tests") == 0) continue;
            walk_lua(path, out, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(e->d_name);
            if (nl > 4 && strcmp(e->d_name + nl - 4, ".lua") == 0) sl_push(out, path);
        }
    }
    closedir(d);
}

/* Read the whole file, or NULL on ANY failure (open, seek, alloc, OR a short read).
 * A short read of a regular file is an error here, not a truncated-but-ok corpus. */
static char *read_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }    /* short read -> fail closed */
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/* Run the conformance script with HULL_LUA_CORPUS injected. Same contract as
 * run_lua_test (returns 0 + { pass, fail } via out-params, -1 on harness failure). */
static int run_lua_conformance(const char *script_path, long long *pass_out, long long *fail_out)
{
    *pass_out = 0;
    *fail_out = -1;

    lua_State *L = luaL_newstate();
    if (!L) return -1;
    luaL_openlibs(L);
    if (luaL_dostring(L,
            "package.path = 'stdlib/cli/lua/?.lua;stdlib/cli/lua/?/init.lua;' .. package.path")
        != LUA_OK) {
        fprintf(stderr, "package.path setup failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return -1;
    }

    static const char *const roots[] = {
        "stdlib/lua", "stdlib/cli/lua", "examples", "tests/fixtures",
    };
    StrList files = { NULL, 0, 0, 0 };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++)
        walk_lua(roots[i], &files, 0);
    if (files.oom) {                       /* fail closed: never test a truncated corpus */
        fprintf(stderr, "conformance: corpus enumeration ran out of memory\n");
        sl_free(&files); lua_close(L); return -1;
    }
    qsort(files.items, files.count, sizeof(char *), cmp_cstr);

    lua_createtable(L, (int)files.count, 0);
    size_t read_ok = 0;
    for (size_t i = 0; i < files.count; i++) {
        size_t len = 0;
        char *src = read_all(files.items[i], &len);
        if (!src) {                        /* an enumerated file MUST read fully */
            fprintf(stderr, "conformance: failed to read corpus file '%s'\n", files.items[i]);
            sl_free(&files); lua_close(L); return -1;
        }
        lua_createtable(L, 0, 2);
        lua_pushstring(L, files.items[i]); lua_setfield(L, -2, "path");
        lua_pushlstring(L, src, len);      lua_setfield(L, -2, "source");
        free(src);
        lua_rawseti(L, -2, (int)(++read_ok));
    }
    lua_setglobal(L, "HULL_LUA_CORPUS");
    fprintf(stderr, "  conformance: enumerated %zu, read %zu .lua files\n",
            files.count, read_ok);
    sl_free(&files);

    if (luaL_dofile(L, script_path) != LUA_OK) {
        fprintf(stderr, "\n%s: %s\n", script_path, lua_tostring(L, -1));
        lua_close(L);
        return -1;
    }
    if (!lua_istable(L, -1)) { lua_close(L); return -1; }
    lua_getfield(L, -1, "fail");
    *fail_out = (long long)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "pass");
    *pass_out = (long long)lua_tointeger(L, -1);
    lua_pop(L, 1);

    fprintf(stderr, "  %s: %lld passed, %lld failed\n", script_path, *pass_out, *fail_out);
    lua_close(L);
    return 0;
}

UTEST(lua_source, lexer_slice1)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_lexer.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);              /* harness ran the script to a { pass, fail } return */
    EXPECT_EQ(fail, 0LL);          /* every Lua assertion passed */
    EXPECT_GT(pass, 0LL);          /* and the script actually ran cases */
}

UTEST(lua_source, parser_slice2)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_parser.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_source, statements_slice3)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_statements.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_source, annotations_slice4)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_annotations.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* Differential conformance vs real Lua 5.4 (load()) over the repo's own .lua corpus,
 * plus range round-trip, curated negatives, pinned semantic divergences, seeded
 * mutation fuzz, and the classifier's own tests. Its own leg for isolated failures. */
UTEST(lua_source, conformance)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_conformance("stdlib/cli/lua/hull/source/tests/test_conformance.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_source, lint_slice1)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_lint.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_source, scope_slice2)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_scope.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* analyze_source's three-state contract, incl. an injected resolver failure. */
UTEST(lua_source, analyze_core)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/source/tests/test_analyze.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* Project source-discovery layer: Lua frontend adapter + model + registry + orchestrator. */
UTEST(lua_source, project_discovery)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/cli/lua/hull/project/tests/test_project.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* ── the pinned, parser-scoped official Lua 5.4.7 test corpus (docs/lua_official_tests_design.md) ──
 * C verifies the vendored corpus fail-closed (manifest schema / pinned Lua version / archive SHA /
 * per-case source_hash / cases<->manifest bijection), injects the VERIFIED bytes as
 * HULL_LUA54_CORPUS, and runs the classifier (load(...,"t") oracle vs hull.source.lua). CI is
 * OFFLINE: only the committed subset is read; scripts/fetch_lua_tests.sh never runs here. */

#define L54_DIR      "tests/fixtures/lua54-tests"
#define L54_CASES    L54_DIR "/cases"
#define L54_MANIFEST L54_DIR "/manifest.json"
#define L54_MANHASH  L54_DIR "/MANIFEST.sha256"
#define L54_LUA_VERSION "5.4.7"
#define L54_ARCHIVE_SHA "8a4898ffe4c7613c8009327a0722db7a41ef861d526c77c5b46114e59ebf811e"
#define L54_SCHEMA 1
#define L54_SELECTION_RULES 1

/* Minimal self-contained SHA-256 (corpus integrity only; no crypto-cap link). */
typedef struct { uint32_t s[8]; uint8_t b[64]; size_t bl; } L54Sha;
static const uint32_t L54_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static uint32_t l54_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void l54_block(L54Sha *c, const uint8_t *p) {
    uint32_t w[64], a, b_, cc, d, e, f, g, h, i, t1, t2;
    for (i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (i = 16; i < 64; i++) { uint32_t s0 = l54_ror(w[i-15],7)^l54_ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = l54_ror(w[i-2],17)^l54_ror(w[i-2],19)^(w[i-2]>>10); w[i] = w[i-16]+s0+w[i-7]+s1; }
    a=c->s[0];b_=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for (i = 0; i < 64; i++) { uint32_t S1=l54_ror(e,6)^l54_ror(e,11)^l54_ror(e,25), ch=(e&f)^(~e&g);
        t1=h+S1+ch+L54_K[i]+w[i]; uint32_t S0=l54_ror(a,2)^l54_ror(a,13)^l54_ror(a,22), mj=(a&b_)^(a&cc)^(b_&cc);
        t2=S0+mj; h=g;g=f;f=e;e=d+t1;d=cc;cc=b_;b_=a;a=t1+t2; }
    c->s[0]+=a;c->s[1]+=b_;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void l54_sha_hex(const void *data, size_t len, char out[65]) {
    L54Sha c; c.s[0]=0x6a09e667;c.s[1]=0xbb67ae85;c.s[2]=0x3c6ef372;c.s[3]=0xa54ff53a;
    c.s[4]=0x510e527f;c.s[5]=0x9b05688c;c.s[6]=0x1f83d9ab;c.s[7]=0x5be0cd19;c.bl=0;
    const uint8_t *p=(const uint8_t*)data; size_t rem=len;
    while (rem) { size_t k=64-c.bl; if (k>rem) k=rem; memcpy(c.b+c.bl,p,k); c.bl+=k; p+=k; rem-=k; if (c.bl==64){l54_block(&c,c.b);c.bl=0;} }
    uint64_t bits=(uint64_t)len*8; c.b[c.bl++]=0x80; if (c.bl>56){ while(c.bl<64)c.b[c.bl++]=0; l54_block(&c,c.b); c.bl=0; }
    while (c.bl<56) c.b[c.bl++]=0; for (int i=7;i>=0;i--) c.b[c.bl++]=(uint8_t)(bits>>(i*8)); l54_block(&c,c.b);
    static const char hx[]="0123456789abcdef";
    for (int i=0;i<8;i++) for (int j=3;j>=0;j--){ uint8_t byte=(uint8_t)(c.s[i]>>(j*8)); *out++=hx[byte>>4]; *out++=hx[byte&15]; }
    *out='\0';
}

/* A committed case path must be relative, canonical, no `.`/`..`/empty component, no backslash,
 * no `//`, no leading/trailing slash. */
static int l54_path_ok(const char *p) {
    if (!p || !*p || p[0]=='/') return 0;
    if (strchr(p,'\\') || strstr(p,"//")) return 0;
    size_t n=strlen(p); if (p[n-1]=='/') return 0;
    const char *seg=p;
    for (size_t i=0;;i++) { if (p[i]=='/'||p[i]=='\0') { size_t sl=(size_t)(&p[i]-seg);
        if (sl==0) return 0; if (sl==1&&seg[0]=='.') return 0; if (sl==2&&seg[0]=='.'&&seg[1]=='.') return 0;
        if (p[i]=='\0') break; seg=&p[i+1]; } }
    return 1;
}

/* Verify the vendored corpus + inject HULL_LUA54_CORPUS, then run the classifier. Returns 0 +
 * {pass,fail} via out-params (harness contract); -1 on any integrity/harness failure so the leg
 * FAILS closed rather than testing a corrupt / partial corpus. */
static int run_lua54_official(const char *script_path, long long *pass_out, long long *fail_out,
                              int *enumerated_out, int *hashed_out)
{
    *pass_out = 0; *fail_out = -1; *enumerated_out = 0; *hashed_out = 0;

    /* manifest bytes + MANIFEST.sha256 integrity */
    size_t man_len = 0; char *man = read_all(L54_MANIFEST, &man_len);
    if (!man) { fprintf(stderr, "lua54: manifest.json unreadable\n"); return -1; }
    size_t mh_len = 0; char *mh = read_all(L54_MANHASH, &mh_len);
    if (!mh) { free(man); fprintf(stderr, "lua54: MANIFEST.sha256 unreadable\n"); return -1; }
    char man_hex[65]; l54_sha_hex(man, man_len, man_hex);
    if (strncmp(mh, man_hex, 64) != 0) { free(man); free(mh); fprintf(stderr, "lua54: manifest hash mismatch\n"); return -1; }
    free(mh);

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    if (!arena) { free(man); return -1; }
    ShJsonValue *root = NULL;
    if (sh_json_parse(man, man_len, arena, &root) != SH_JSON_OK) { free(man); sh_arena_free(arena); fprintf(stderr, "lua54: manifest parse\n"); return -1; }
    if (sh_json_as_int(sh_json_get(root, "schema_version"), -1) != L54_SCHEMA
        || sh_json_as_int(sh_json_get(root, "selection_rules_version"), -1) != L54_SELECTION_RULES
        || strcmp(sh_json_as_string(sh_json_get(root, "lua_version"), ""), L54_LUA_VERSION) != 0
        || strcmp(sh_json_as_string(sh_json_get(root, "archive_sha256"), ""), L54_ARCHIVE_SHA) != 0) {
        free(man); sh_arena_free(arena); fprintf(stderr, "lua54: manifest header mismatch\n"); return -1;
    }
    ShJsonValue *cases = sh_json_get(root, "cases");
    int count = sh_json_as_int(sh_json_get(root, "count"), -1);
    if (!cases || sh_json_type(cases) != SH_JSON_ARRAY || count != (int)sh_json_array_len(cases)) {
        free(man); sh_arena_free(arena); fprintf(stderr, "lua54: bad cases array\n"); return -1;
    }

    /* set up the Lua state + inject the verified corpus */
    lua_State *L = luaL_newstate();
    if (!L) { free(man); sh_arena_free(arena); return -1; }
    luaL_openlibs(L);
    if (luaL_dostring(L, "package.path = 'stdlib/cli/lua/?.lua;stdlib/cli/lua/?/init.lua;' .. package.path") != LUA_OK) {
        fprintf(stderr, "lua54: package.path: %s\n", lua_tostring(L, -1)); lua_close(L); free(man); sh_arena_free(arena); return -1;
    }
    lua_createtable(L, count, 0);
    int read_ok = 0, hashed = 0;
    for (size_t i = 0; i < sh_json_array_len(cases); i++) {
        ShJsonValue *c = sh_json_array_get(cases, i);
        const char *rel = sh_json_as_string(sh_json_get(c, "path"), NULL);
        const char *shash = sh_json_as_string(sh_json_get(c, "source_hash"), NULL);
        if (!rel || !shash || !l54_path_ok(rel)) { fprintf(stderr, "lua54: bad case path\n"); lua_close(L); free(man); sh_arena_free(arena); return -1; }
        if (i > 0) {   /* sorted, unique */
            const char *prev = sh_json_as_string(sh_json_get(sh_json_array_get(cases, i-1), "path"), "");
            if (strcmp(prev, rel) >= 0) { fprintf(stderr, "lua54: unsorted/dup %s\n", rel); lua_close(L); free(man); sh_arena_free(arena); return -1; }
        }
        char full[4096];
        int w = snprintf(full, sizeof full, "%s/%s", L54_CASES, rel);
        if (w < 0 || (size_t)w >= sizeof full) { lua_close(L); free(man); sh_arena_free(arena); return -1; }
        struct stat st;
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) { fprintf(stderr, "lua54: not a regular file: %s\n", rel); lua_close(L); free(man); sh_arena_free(arena); return -1; }
        size_t len = 0; char *src = read_all(full, &len);
        if (!src) { fprintf(stderr, "lua54: read fail %s\n", rel); lua_close(L); free(man); sh_arena_free(arena); return -1; }
        read_ok++;
        char hex[65]; l54_sha_hex(src, len, hex);
        if (strcmp(hex, shash) != 0) { fprintf(stderr, "lua54: source_hash mismatch %s\n", rel); free(src); lua_close(L); free(man); sh_arena_free(arena); return -1; }
        hashed++;
        lua_createtable(L, 0, 2);
        lua_pushstring(L, rel); lua_setfield(L, -2, "path");
        lua_pushlstring(L, src, len); lua_setfield(L, -2, "source");
        free(src);
        lua_rawseti(L, -2, (int)(i + 1));
    }
    lua_setglobal(L, "HULL_LUA54_CORPUS");

    /* bijection: every disk .lua under cases/ appears in the manifest and vice versa */
    StrList disk = { NULL, 0, 0, 0 };
    walk_lua(L54_CASES, &disk, 0);
    if (disk.oom) { sl_free(&disk); lua_close(L); free(man); sh_arena_free(arena); return -1; }
    if ((int)disk.count != count) { fprintf(stderr, "lua54: bijection count disk=%zu manifest=%d\n", disk.count, count); sl_free(&disk); lua_close(L); free(man); sh_arena_free(arena); return -1; }
    qsort(disk.items, disk.count, sizeof(char *), cmp_cstr);
    int bij = 0;
    for (size_t i = 0; i < disk.count; i++) {
        const char *rel = sh_json_as_string(sh_json_get(sh_json_array_get(cases, i), "path"), "");
        char full[4096]; snprintf(full, sizeof full, "%s/%s", L54_CASES, rel);
        if (strcmp(disk.items[i], full) != 0) { bij = -1; fprintf(stderr, "lua54: bijection mismatch %s vs %s\n", disk.items[i], full); break; }
    }
    sl_free(&disk);
    if (bij != 0) { lua_close(L); free(man); sh_arena_free(arena); return -1; }

    *enumerated_out = count; *hashed_out = hashed;
    free(man); sh_arena_free(arena);
    fprintf(stderr, "  lua54: enumerated %d, read %d, hashed %d official .lua files\n", count, read_ok, hashed);

    if (luaL_dofile(L, script_path) != LUA_OK) { fprintf(stderr, "\n%s: %s\n", script_path, lua_tostring(L, -1)); lua_close(L); return -1; }
    if (!lua_istable(L, -1)) { lua_close(L); return -1; }
    lua_getfield(L, -1, "fail"); *fail_out = (long long)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "pass"); *pass_out = (long long)lua_tointeger(L, -1); lua_pop(L, 1);
    fprintf(stderr, "  %s: %lld passed, %lld failed\n", script_path, *pass_out, *fail_out);
    lua_close(L);
    return 0;
}

/* The official Lua 5.4.7 suite as a parser-scoped corpus (load(...,"t") vs hull.source.lua).
 * Verified fail-closed in C; classified in Lua. Its own leg for isolated failures. */
UTEST(lua_source, official_lua54)
{
    long long pass = 0, fail = -1;
    int enumerated = 0, hashed = 0;
    int rc = run_lua54_official("stdlib/cli/lua/hull/source/tests/test_official_lua54.lua",
                                &pass, &fail, &enumerated, &hashed);
    ASSERT_EQ(rc, 0);                 /* corpus integrity + bijection + classifier all ran */
    EXPECT_EQ(enumerated, 33);        /* the pinned selection is 33 .lua files */
    EXPECT_EQ(hashed, enumerated);    /* every case source_hash verified before parsing */
    EXPECT_EQ(fail, 0LL);             /* zero-gates: no false-reject/accept/unsupported/indeterminate/range */
    EXPECT_GT(pass, 0LL);
}

UTEST_MAIN()
