/*
 * test_buildctx.c - runs the BuildContext suites (stdlib/cli/lua/hull/buildctx/
 * tests/: the action's inputs, its outputs and results, the plugin host,
 * registration and the pipeline plan) against the real hull.buildctx._fs / _store / _plugin
 * bindings and a real workspace, with the few filesystem helpers a plain Lua
 * state lacks. HOME is a temporary directory, so the tool seal key the store
 * makes is never the user's. The plugin host leg runs real plugin processes:
 * the hull binary at $HULL_BIN, else build/hull.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "../lua_script_test.h"
#include "../test_tmpdir.h"
#include "../../../src/hull/runtime/lua/mod_buildctx.h"
#include "../../../src/hull/utils/hex.h"
#include "hull/cap/crypto.h"
#include "hull/manifest.h"
#include "hull/utils/alloc.h"

#include <fcntl.h>
#if defined(__APPLE__)
# include <malloc/malloc.h>
#elif defined(__GLIBC__)
# include <malloc.h>
#endif
#include <ftw.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int rm_e(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }

/* t.tmpdir(name) -> a fresh absolute directory */
static int t_tmpdir(lua_State *L)
{
    char p[HL_TEST_PATH_MAX];
    hl_test_path(p, sizeof(p), "hull_buildctx_%d_%s", (int)getpid(), luaL_checkstring(L, 1));
    nftw(p, rm_e, 16, FTW_DEPTH | FTW_PHYS);
    mkdir(p, 0755);
    lua_pushstring(L, p);
    return 1;
}
static int t_rmtree(lua_State *L) { nftw(luaL_checkstring(L, 1), rm_e, 16, FTW_DEPTH | FTW_PHYS); return 0; }
static int t_mkdir(lua_State *L) { lua_pushboolean(L, mkdir(luaL_checkstring(L, 1), 0755) == 0); return 1; }
static int t_symlink(lua_State *L)
{
    lua_pushboolean(L, symlink(luaL_checkstring(L, 1), luaL_checkstring(L, 2)) == 0);
    return 1;
}
static int t_set_mtime(lua_State *L)
{
    struct timeval tv[2] = { { (time_t)luaL_checkinteger(L, 2), 0 }, { (time_t)luaL_checkinteger(L, 2), 0 } };
    lua_pushboolean(L, utimes(luaL_checkstring(L, 1), tv) == 0);
    return 1;
}
static int t_chmod(lua_State *L)
{
    const char *p = luaL_checkstring(L, 1);
    struct stat st;
    /* true only when the host kept the bits (a Windows APE may not) */
    lua_pushboolean(L, chmod(p, (mode_t)luaL_checkinteger(L, 2)) == 0 && stat(p, &st) == 0 &&
                       (st.st_mode & 0777) == (mode_t)luaL_checkinteger(L, 2));
    return 1;
}
static int t_open_fds(lua_State *L)
{
    int n = 0;
    for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) != -1) n++;
    lua_pushinteger(L, n);
    return 1;
}

/* T.sha256(s) -> lowercase hex */
static int t_sha256(lua_State *L)
{
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    uint8_t sha[32];
    char hex[65];
    hl_cap_crypto_sha256(s, n, sha);
    hl_hex_encode(sha, 32, hex, sizeof hex);
    lua_pushlstring(L, hex, 64);
    return 1;
}

static char g_home[HL_TEST_PATH_MAX];

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define BUILDCTX_TEST_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define BUILDCTX_TEST_ASAN 1
#endif

static void setup(lua_State *L)
{
#ifdef BUILDCTX_TEST_ASAN
    /* The plugin processes are this build's hull, under the app sandbox: the
     * leak checker's exit-time thread is a syscall the sandbox refuses, which
     * killed a plugin that STOP had ended cleanly. Their leaks are not what
     * this suite checks. */
    {
        const char *o = getenv("ASAN_OPTIONS");
        char buf[512];
        snprintf(buf, sizeof buf, "%s%sdetect_leaks=0", o ? o : "", o && o[0] ? ":" : "");
        setenv("ASAN_OPTIONS", buf, 1);
    }
#endif
    if (!g_home[0]) {
        hl_test_path(g_home, sizeof(g_home), "hull_buildctx_home_%d", (int)getpid());
        mkdir(g_home, 0700);
        setenv("HOME", g_home, 1);
    }
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_pushcfunction(L, luaopen_hull_buildctx_fs);
    lua_setfield(L, -2, "hull.buildctx._fs");
    lua_pushcfunction(L, luaopen_hull_buildctx_store);
    lua_setfield(L, -2, "hull.buildctx._store");
    lua_pushcfunction(L, luaopen_hull_buildctx_plugin);
    lua_setfield(L, -2, "hull.buildctx._plugin");
    lua_pop(L, 1);
    static const luaL_Reg T[] = {
        { "tmpdir", t_tmpdir }, { "rmtree", t_rmtree }, { "mkdir", t_mkdir },
        { "symlink", t_symlink }, { "set_mtime", t_set_mtime }, { "chmod", t_chmod },
        { "open_fds", t_open_fds }, { "sha256", t_sha256 }, { NULL, NULL },
    };
    luaL_newlib(L, T);
    char cwd[HL_TEST_PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) {
        lua_pushstring(L, cwd);
        lua_setfield(L, -2, "cwd");
        const char *bin = getenv("HULL_BIN");
        if (bin && bin[0] == '/') lua_pushstring(L, bin);
        else lua_pushfstring(L, "%s/%s", cwd, bin && bin[0] ? bin : "build/hull");
        lua_setfield(L, -2, "hull");
    }
    lua_setglobal(L, "T");
}

/* P.preset_check swapped the registry's __hull_manifest for the plugin's
 * table and extracted it unprotected: a raise inside the extraction (a
 * metamethod, an allocation failure) left the registry naming the plugin's
 * table and leaked the strings already copied (audit 50). */
UTEST(buildctx_audit50, preset_check_restores_manifest)
{
    lua_State *L = luaL_newstate();
    ASSERT_TRUE(L != NULL);
    luaL_openlibs(L);
    lua_newtable(L);
    lua_pushliteral(L, "saved");
    lua_setfield(L, -2, "marker");
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    luaL_requiref(L, "P", luaopen_hull_buildctx_plugin, 1);
    lua_pop(L, 1);
    const char *chunk =
        "local m = setmetatable({ fs = { read = { 'a', 'b' } } },"
        "  { __index = function(_, k) if k == 'env' then error('boom50', 0) end end })\n"
        "local ok, err = pcall(P.preset_check, m)\n"
        "return ok, err";
    ASSERT_EQ(luaL_dostring(L, chunk), LUA_OK);
    EXPECT_FALSE(lua_toboolean(L, -2));
    EXPECT_STREQ(lua_tostring(L, -1) ? lua_tostring(L, -1) : "(null)", "boom50");
    lua_pop(L, 2);
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "marker");
    EXPECT_STREQ(lua_tostring(L, -1) ? lua_tostring(L, -1) : "(null)", "saved");
    lua_pop(L, 2);
    /* A plain table still extracts (the preset refuses an empty manifest). */
    ASSERT_EQ(luaL_dostring(L, "return P.preset_check({})"), LUA_OK);
    EXPECT_TRUE(lua_isnil(L, -2));
    lua_close(L);
}

/* Bytes the process's allocator holds, or 0 where this host gives no count
 * (the leak check below is then skipped). Under ASan freed blocks sit in its
 * quarantine and still count; LeakSanitizer covers the leak there. */
#if defined(__has_feature)
# if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#  define A50_SANITIZED 1
# endif
#endif
#if defined(__SANITIZE_ADDRESS__)
# define A50_SANITIZED 1
#endif
static size_t a50_heap_in_use(void)
{
#if defined(A50_SANITIZED)
    return 0;
#elif defined(__APPLE__)
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    return st.size_in_use;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    return mallinfo2().uordblks;
#else
    return 0;
#endif
}

/* A raise inside the extraction left what it had copied before the raise
 * (fs.read here) allocated: pl_preset_check frees the partial manifest before
 * it re-raises (audit 50). 32 paths of 2 KiB a call, 100 calls: a leak would
 * be 6.4 MB. */
UTEST(buildctx_audit50, preset_check_raise_frees_partial_manifest)
{
    if (a50_heap_in_use() == 0) {
        UTEST_SKIP("no allocator statistics on this host");
    }
    lua_State *L = luaL_newstate();
    ASSERT_TRUE(L != NULL);
    luaL_openlibs(L);
    luaL_requiref(L, "P", luaopen_hull_buildctx_plugin, 1);
    lua_pop(L, 1);
    ASSERT_EQ(luaL_dostring(L,
        "local read = {}\n"
        "for i = 1, 32 do read[i] = string.rep(string.char(97 + i % 26), 2048) .. i end\n"
        "M = setmetatable({ fs = { read = read } },\n"
        "  { __index = function(_, k) if k == 'env' then error('boom50', 0) end end })\n"
        "function RUN(n) for i = 1, n do assert(not pcall(P.preset_check, M)) end end\n"
        "RUN(5)"), LUA_OK);
    lua_gc(L, LUA_GCCOLLECT, 0);
    size_t before = a50_heap_in_use();
    ASSERT_EQ(luaL_dostring(L, "RUN(100)"), LUA_OK);
    lua_gc(L, LUA_GCCOLLECT, 0);
    size_t after = a50_heap_in_use();
    size_t grew = after > before ? after - before : 0;
    EXPECT_LT_MSG(grew, (size_t)1024 * 1024, "partial manifests leaked");
    lua_close(L);
}

/* After the extraction raised: the error object is the extractor's own
 * (re-raised unchanged), the caller's stack holds only it, the registry names
 * the tool VM's real manifest again - so the next extraction (the build's own,
 * hl_manifest_extract_lua) reads that manifest, not the plugin's - and the next
 * preset_check calls judge their own manifests (audit 50). */
UTEST(buildctx_audit50, preset_check_raise_leaves_no_state)
{
    lua_State *L = luaL_newstate();
    ASSERT_TRUE(L != NULL);
    luaL_openlibs(L);
    ASSERT_EQ(luaL_dostring(L, "REAL = { hosts = { 'real.example' } }"), LUA_OK);
    lua_getglobal(L, "REAL");
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    luaL_requiref(L, "P", luaopen_hull_buildctx_plugin, 1);
    lua_setglobal(L, "P");
    ASSERT_EQ(luaL_dostring(L,
        "ERR = {}\n"
        "BAD = setmetatable({ hosts = { 'plugin.example' } },\n"
        "  { __index = function(_, k) if k == 'csp' then error(ERR) end end })"), LUA_OK);

    int top = lua_gettop(L);
    lua_getglobal(L, "P");
    lua_getfield(L, -1, "preset_check");
    lua_remove(L, -2);
    lua_getglobal(L, "BAD");
    EXPECT_NE(lua_pcall(L, 1, 1, 0), LUA_OK);
    EXPECT_EQ(lua_gettop(L), top + 1);
    lua_getglobal(L, "ERR");
    EXPECT_TRUE(lua_rawequal(L, -1, -2));
    lua_settop(L, top);

    /* Asserted: the extraction below would raise the plugin's error
     * unprotected if the registry still named BAD. */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    lua_getglobal(L, "REAL");
    ASSERT_TRUE(lua_rawequal(L, -1, -2));
    lua_settop(L, top);

    HlAllocator a;
    hl_alloc_init(&a, 0);
    HlManifest m;
    ASSERT_EQ(hl_manifest_extract_lua(L, &m, &a), 0);
    EXPECT_EQ(lua_gettop(L), top);
    EXPECT_EQ(m.hosts_count, 1);
    if (m.hosts_count == 1) EXPECT_STREQ(m.hosts[0], "real.example");
    hl_manifest_free(&m);
    EXPECT_EQ(hl_alloc_used(&a), (size_t)0);

    ASSERT_EQ(luaL_dostring(L,
        "local ok, err = P.preset_check({ modules = { 'hull/build-plugin@1' } })\n"
        "assert(ok == true, tostring(err))\n"
        "local ok2, err2 = P.preset_check({ hosts = { 'x.example' } })\n"
        "assert(ok2 == nil and err2:find('hosts', 1, true), tostring(err2))"), LUA_OK);
    EXPECT_EQ(lua_gettop(L), top);
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    lua_getglobal(L, "REAL");
    EXPECT_TRUE(lua_rawequal(L, -1, -2));
    lua_settop(L, top);
    lua_close(L);
}

UTEST(buildctx, action_core)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test_with("stdlib/cli/lua/hull/buildctx/tests/test_action.lua", setup, &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(buildctx, outputs_and_results)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test_with("stdlib/cli/lua/hull/buildctx/tests/test_outputs.lua", setup, &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(buildctx, plugin_host)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test_with("stdlib/cli/lua/hull/buildctx/tests/test_plugin.lua", setup, &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(buildctx, registration)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test_with("stdlib/cli/lua/hull/buildctx/tests/test_registry.lua", setup, &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(buildctx, pipeline_generation)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test_with("stdlib/cli/lua/hull/buildctx/tests/test_generation.lua", setup, &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(buildctx, pipeline_plan)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test_with("stdlib/cli/lua/hull/buildctx/tests/test_plan.lua", setup, &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* Audit 49: the channel's edges, one case of test_plugin_audit49.lua each. */
static const char *g_case;

static void setup_case(lua_State *L)
{
    setup(L);
    lua_getglobal(L, "T");
    lua_pushstring(L, g_case);
    lua_setfield(L, -2, "case");
    lua_pop(L, 1);
}

static void run_audit49(int *utest_result, const char *name)
{
    long long pass = 0, fail = -1;
    g_case = name;
    int rc = run_lua_test_with("stdlib/cli/lua/hull/buildctx/tests/test_plugin_audit49.lua",
                               setup_case, &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(buildctx_audit49, big_error_reported) { run_audit49(utest_result, "big_error_reported"); }
UTEST(buildctx_audit49, nil_error_fails) { run_audit49(utest_result, "nil_error_fails"); }
UTEST(buildctx_audit49, deep_args_refused_cleanly) { run_audit49(utest_result, "deep_args_refused_cleanly"); }
UTEST(buildctx_audit49, shared_value_bounded) { run_audit49(utest_result, "shared_value_bounded"); }
UTEST(buildctx_audit49, recv_raise_contained) { run_audit49(utest_result, "recv_raise_contained"); }

int main(int argc, const char *const argv[])
{
    int rc = utest_main(argc, argv);
    if (g_home[0]) nftw(g_home, rm_e, 16, FTW_DEPTH | FTW_PHYS);
    return rc;
}

UTEST_STATE();
