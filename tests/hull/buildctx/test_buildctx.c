/*
 * test_buildctx.c - runs the BuildContext suites (stdlib/cli/lua/hull/buildctx/
 * tests/: the action's inputs, its outputs and results) against the real
 * hull.buildctx._fs / _store bindings and a real workspace, with the few
 * filesystem helpers a plain Lua state lacks. HOME is a temporary directory, so
 * the tool seal key the store makes is never the user's.
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

#include <fcntl.h>
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

static char g_home[HL_TEST_PATH_MAX];

static void setup(lua_State *L)
{
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
    lua_pop(L, 1);
    static const luaL_Reg T[] = {
        { "tmpdir", t_tmpdir }, { "rmtree", t_rmtree }, { "mkdir", t_mkdir },
        { "symlink", t_symlink }, { "set_mtime", t_set_mtime }, { "chmod", t_chmod },
        { "open_fds", t_open_fds }, { NULL, NULL },
    };
    luaL_newlib(L, T);
    lua_setglobal(L, "T");
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

int main(int argc, const char *const argv[])
{
    int rc = utest_main(argc, argv);
    if (g_home[0]) nftw(g_home, rm_e, 16, FTW_DEPTH | FTW_PHYS);
    return rc;
}

UTEST_STATE();
