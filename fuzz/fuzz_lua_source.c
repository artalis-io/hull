/*
 * fuzz_lua_source.c — continuous libFuzzer harness for hull.source.lua.
 *
 * Feeds arbitrary bytes to lua.parse() and asserts the parser's INTRINSIC
 * invariants (design: docs/lua_source_fuzz_design.md):
 *   - never-raise: the C-level pcall of parse() returns LUA_OK; a Lua error
 *     escaping parse() -> abort() (a never-raise breach);
 *   - valid shape: a unit table (with .ast + .diagnostics) OR a proper (nil, err);
 *   - range sanity: every AST node's range is in [1, #src+1], non-inverted, and
 *     unit:text(node) slices exactly;
 *   - bounded: a per-input BOUNDED ALLOCATOR (measured from the post-init VM
 *     baseline) caps memory; exhaustion raises LUA_ERRMEM, which is classified as
 *     EXPECTED resource exhaustion (NOT a crash) -- an allocation failure can even
 *     prevent parse() from constructing its (nil, err), so its own pcall may
 *     re-raise LUA_ERRMEM.
 * Any OTHER escaping Lua error is a crash. Complements the differential conformance
 * gate (correctness vs load()) with robustness vs adversarial input.
 *
 * Build/run: make fuzz-lua-source   (clang -fsanitize=fuzzer,address[,undefined])
 * Local smoke (no libFuzzer): cc -DHL_FUZZ_STANDALONE -Ivendor/lua \
 *     fuzz/fuzz_lua_source.c vendor/lua/l*.c -lm -o /tmp/fz && ./tmp/fz <files...>
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

/* Per-input memory allowance above the post-init baseline. Comfortably above any
 * legitimate parse of a bounded fuzz input; low enough to catch runaway growth. */
#define PER_INPUT_BYTES ((size_t)64 * 1024 * 1024)

/* ── bounded Lua allocator ──────────────────────────────────────────────
 * Standard realloc-backed allocator with a hard ceiling. Lua's contract: for a NEW
 * block (ptr == NULL) `osize` is a type tag, NOT a size, so it is only subtracted
 * when ptr != NULL. Returning NULL makes Lua raise LUA_ERRMEM. */
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
    if (next > st->ceiling) return NULL;   /* deny -> LUA_ERRMEM (expected) */
    void *np = realloc(ptr, nsize);
    if (!np) return NULL;
    st->in_use = next;
    return np;
}

/* The Lua driver: parse + shape + range checks. Returns (ok:boolean, msg:string?).
 * It never needs to raise -- parse() is internally pcall-guarded, and the checks are
 * defensive table reads. A false return with a message is a real robustness bug. */
static const char *DRIVER_SRC =
    "local lua = require('hull.source.lua')\n"
    "local LIMITS = { max_bytes = 1048576, max_tokens = 200000, max_comments = 200000, max_depth = 400 }\n"
    "return function(input)\n"
    "  local u, e = lua.parse(input, LIMITS)\n"
    "  if u == nil then\n"
    "    if e == nil then return false, 'parse returned (nil, nil)' end\n"
    "    return true\n"                     /* a proper (nil, err) is valid */
    "  end\n"
    "  if type(u.diagnostics) ~= 'table' then return false, 'unit has no diagnostics' end\n"
    "  if type(u.ast) ~= 'table' then return false, 'unit has no ast' end\n"
    "  local maxoff = #input + 1\n"
    "  local bad = nil\n"
    "  lua.walk(u.ast, function(n)\n"
    "    if bad then return end\n"
    "    local r = n.range\n"
    "    if type(r) ~= 'table' or type(r.start) ~= 'number' or type(r.stop) ~= 'number'\n"
    "       or r.start < 1 or r.stop < r.start or r.stop > maxoff then\n"
    "      bad = 'bad range kind=' .. tostring(n.kind); return\n"
    "    end\n"
    "    if u:text(n) ~= input:sub(r.start, r.stop - 1) then\n"
    "      bad = 'text mismatch kind=' .. tostring(n.kind)\n"
    "    end\n"
    "  end)\n"
    "  if bad then return false, bad end\n"
    "  return true\n"
    "end\n";

static lua_State *L;            /* persistent across inputs */
static AllocState g_alloc;      /* bounded allocator state */
static int g_driver_ref = LUA_NOREF;
static int g_base_top;          /* stack depth after init */

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "fuzz_lua_source: %s%s%s\n",
            what, detail ? ": " : "", detail ? detail : "");
    fflush(stderr);
    abort();
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    g_alloc.in_use = 0;
    g_alloc.ceiling = (size_t)-1;                    /* unbounded during init */
    L = lua_newstate(fuzz_alloc, &g_alloc);
    if (!L) die("lua_newstate failed", NULL);
    luaL_openlibs(L);

    if (luaL_dostring(L,
            "package.path = 'stdlib/cli/lua/?.lua;stdlib/cli/lua/?/init.lua;' .. package.path")
        != LUA_OK)
        die("package.path setup failed", lua_tostring(L, -1));

    /* Load the driver and cache the returned closure in the registry. */
    if (luaL_loadstring(L, DRIVER_SRC) != LUA_OK)
        die("driver compile failed", lua_tostring(L, -1));
    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        die("driver init failed", lua_tostring(L, -1));
    if (!lua_isfunction(L, -1))
        die("driver did not return a function", NULL);
    g_driver_ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* pops the function */

    lua_gc(L, LUA_GCCOLLECT, 0);
    g_base_top = lua_gettop(L);
    /* Baseline captured; arm the per-input ceiling. */
    g_alloc.ceiling = g_alloc.in_use + PER_INPUT_BYTES;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!L) LLVMFuzzerInitialize(NULL, NULL);

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_driver_ref);  /* the driver closure */
    lua_pushlstring(L, (const char *)data, size);     /* binary-safe input */

    int rc = lua_pcall(L, 1, 2, 0);                   /* driver(input) -> ok, msg? */
    if (rc == LUA_OK) {
        int ok = lua_toboolean(L, -2);
        if (!ok) {
            const char *msg = lua_tostring(L, -1);
            die("invariant violated", msg ? msg : "(no message)");
        }
    } else if (rc == LUA_ERRMEM) {
        /* Expected: the per-input allocator allowance was exceeded. NOT a crash. */
    } else {
        die("Lua error escaped parse()", lua_tostring(L, -1));
    }

    /* Always: restore the stack + force collection before the next input, so
     * per-parse garbage never bleeds across inputs and the allowance stays clean. */
    lua_settop(L, g_base_top);
    lua_gc(L, LUA_GCCOLLECT, 0);
    return 0;
}

#ifdef HL_FUZZ_STANDALONE
/* Local smoke driver (no libFuzzer): run each file arg (or stdin) through the
 * harness. Aborts on any invariant violation, prints a summary otherwise. */
static int run_file(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    if (path) fclose(f);
    LLVMFuzzerTestOneInput((const uint8_t *)buf, n);
    return 0;
}

int main(int argc, char **argv)
{
    LLVMFuzzerInitialize(&argc, &argv);
    if (argc <= 1) { run_file(NULL); }
    else { for (int i = 1; i < argc; i++) run_file(argv[i]); }
    fprintf(stderr, "fuzz_lua_source: %d input(s) OK\n", argc > 1 ? argc - 1 : 1);
    return 0;
}
#endif
