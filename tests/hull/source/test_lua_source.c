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
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

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

    /* Resolve require("hull.source.X") from the source tree (repo-root relative). */
    if (luaL_dostring(L,
            "package.path = 'stdlib/cli/lua/?.lua;stdlib/cli/lua/?/init.lua;' .. package.path")
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

UTEST_MAIN()
