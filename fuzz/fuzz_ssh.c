/*
 * fuzz_ssh.c - libFuzzer harness for the hull.ssh protocol stack.
 *
 * Every byte an SSH server sends is parsed by pure-Lua code under
 * stdlib/lua/hull/ssh/, most of it before authentication. Lua makes a bounds
 * mistake a wrong answer rather than heap corruption, but a wrong answer can
 * still be a crash from the caller's side: a Lua runtime error escaping
 * hull.ssh is not a refusal anyone can match on. This harness feeds arbitrary
 * bytes to each parser and to a whole connection (handshake, auth, exec,
 * sftp) over a scripted stream, and aborts when anything other than an
 * `ssh...:` error or a coded refusal escapes.
 *
 * The targets and the oracle live in fuzz/ssh_driver.lua; this file is the
 * libFuzzer glue. The first input byte selects a target.
 *
 * Bounds, so a finding is a finding and not a resource artefact:
 *   - memory: a per-input ceiling above the post-init baseline; exhaustion is
 *     LUA_ERRMEM, classified as expected (same as fuzz_lua_source.c);
 *   - time: an instruction budget per input. Running out is reported as a bug,
 *     because every loop in hull.ssh is meant to be bounded by its input.
 *
 * Build/run: make fuzz-ssh   (clang -fsanitize=fuzzer,address,undefined)
 * Local smoke (no libFuzzer): cc -DHL_FUZZ_STANDALONE -Ivendor/lua \
 *     fuzz/fuzz_ssh.c vendor/lua/l*.c -lm -o /tmp/fz && /tmp/fz fuzz/corpus_ssh/NN_seed
 * Run from the repo root: the driver and stdlib load from the source tree.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PER_INPUT_BYTES ((size_t)64 * 1024 * 1024)

/* Instructions per input, counted in hook steps of HOOK_STEP. Generous: a
 * whole scripted connection runs a few million. */
#define HOOK_STEP   10000
#define HOOK_BUDGET 20000   /* 200M instructions */

typedef struct { size_t in_use; size_t ceiling; } AllocState;

static void *fuzz_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    AllocState *st = (AllocState *)ud;
    if (nsize == 0) {
        if (ptr) st->in_use -= osize;
        free(ptr);
        return NULL;
    }
    size_t old = ptr ? osize : 0;
    size_t next = st->in_use - old + nsize;
    if (next > st->ceiling) return NULL;
    void *np = realloc(ptr, nsize);
    if (!np) return NULL;
    st->in_use = next;
    return np;
}

static lua_State *L;
static AllocState g_alloc;
static int g_driver_ref = LUA_NOREF;
static int g_base_top;
static int g_steps;

/* Once the budget is spent, raise on EVERY step: a single raise could be
 * swallowed by a pcall inside the code under test, and the loop would go on. */
static void budget_hook(lua_State *Ls, lua_Debug *ar)
{
    (void)ar;
    if (++g_steps > HOOK_BUDGET)
        luaL_error(Ls, "fuzz: instruction budget exhausted (an unbounded loop?)");
}

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "fuzz_ssh: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    fflush(stderr);
    abort();
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    g_alloc.in_use = 0;
    g_alloc.ceiling = (size_t)-1;
    L = lua_newstate(fuzz_alloc, &g_alloc);
    if (!L) die("lua_newstate failed", NULL);
    luaL_openlibs(L);

    if (luaL_dostring(L,
            "package.path = 'stdlib/lua/?.lua;stdlib/lua/?/init.lua;' .. package.path")
        != LUA_OK)
        die("package.path setup failed", lua_tostring(L, -1));

    if (luaL_loadfile(L, "fuzz/ssh_driver.lua") != LUA_OK)
        die("driver load failed (run from the repo root)", lua_tostring(L, -1));
    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        die("driver init failed", lua_tostring(L, -1));
    if (!lua_isfunction(L, -1))
        die("driver did not return a function", NULL);
    g_driver_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_gc(L, LUA_GCCOLLECT, 0);
    g_base_top = lua_gettop(L);
    g_alloc.ceiling = g_alloc.in_use + PER_INPUT_BYTES;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!L) LLVMFuzzerInitialize(NULL, NULL);

    g_steps = 0;
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, HOOK_STEP);

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_driver_ref);
    lua_pushlstring(L, (const char *)data, size);

    int rc = lua_pcall(L, 1, 2, 0);
    lua_sethook(L, NULL, 0, 0);
    if (rc == LUA_OK) {
        if (!lua_toboolean(L, -2)) {
            const char *msg = lua_tostring(L, -1);
            die("invariant violated", msg ? msg : "(no message)");
        }
    } else if (rc == LUA_ERRMEM) {
        /* Expected: the per-input allowance was exceeded. */
    } else {
        die("Lua error escaped the driver", lua_tostring(L, -1));
    }

    lua_settop(L, g_base_top);
    lua_gc(L, LUA_GCCOLLECT, 0);
    return 0;
}

#ifdef HL_FUZZ_STANDALONE
static void run_file(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    if (path) fclose(f);
    LLVMFuzzerTestOneInput((const uint8_t *)buf, n);
}

int main(int argc, char **argv)
{
    LLVMFuzzerInitialize(&argc, &argv);
    if (argc <= 1) run_file(NULL);
    else for (int i = 1; i < argc; i++) run_file(argv[i]);
    fprintf(stderr, "fuzz_ssh: %d input(s) OK\n", argc > 1 ? argc - 1 : 1);
    return 0;
}
#endif
