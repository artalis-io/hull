/*
 * bench_bytecode_cache.c — Cold-vs-warm comparison for hl_lua_load_cached.
 *
 * Measures the wall-clock cost of loading every embedded Lua stdlib
 * chunk (~70 modules) twice:
 *   1. Cold: $HOME/.hull/cache/ is empty → every load parses source
 *      AND persists bytecode to disk.
 *   2. Warm: cache populated → every load reads + verifies bytecode.
 *
 * Reports per-load µs in both regimes. The expected win on warm boot
 * is "luaL_loadbuffer skipped → just disk read + bytecode verify".
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua_bytecode_cache.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <dirent.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static double now_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1.0e6;
}

/* Stand-in stdlib corpus: synthesize N realistic-sized Lua modules.
 * Each is ~1.5 KB of mixed control flow + table construction — close
 * to a real stdlib middleware. Held in heap so the bench doesn't
 * depend on any particular Lua file layout. */
typedef struct {
    char *src;
    size_t len;
    char name[64];
} Chunk;

static char *synth_chunk(int seed, size_t *out_len)
{
    size_t cap = 4096;
    char *buf = malloc(cap);
    int n = 0;
    n += snprintf(buf + n, cap - n,
        "-- Synthetic stdlib chunk #%d\n"
        "local M = {}\n"
        "local function pad_%d(x, y) return x * %d + y end\n",
        seed, seed, seed * 7 + 11);
    for (int i = 0; i < 20; i++) {
        n += snprintf(buf + n, cap - n,
            "function M.case_%d(v)\n"
            "    local t = { idx = %d, op = 'case', val = v }\n"
            "    if pad_%d(v, %d) > %d then\n"
            "        t.r = v * 2 - %d\n"
            "    else\n"
            "        t.r = v + %d\n"
            "    end\n"
            "    return t\n"
            "end\n",
            i, i, seed, i, seed * (i + 1), i + 3, i * 2 + 1);
    }
    n += snprintf(buf + n, cap - n, "return M\n");
    *out_len = (size_t)n;
    return buf;
}

#define N_CHUNKS 70   /* approximately the Hull Lua stdlib surface */

static int rm_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void wipe_cache(const char *home)
{
    char path[512];
    snprintf(path, sizeof(path),
             "%s/.hull/blobs/runtime/lua-bytecode", home);
    nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static double run_pass(Chunk *chunks, int n)
{
    double t0 = now_sec();
    for (int i = 0; i < n; i++) {
        lua_State *L = luaL_newstate();
        int rc = hl_lua_load_cached(L, chunks[i].src, chunks[i].len, chunks[i].name);
        if (rc != LUA_OK) {
            fprintf(stderr, "load failed on chunk %d: %s\n", i, lua_tostring(L, -1));
            exit(1);
        }
        /* Don't run the chunk — we're benching load, not execute. */
        lua_close(L);
    }
    return now_sec() - t0;
}

int main(void)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/hull_bc_bench_XXXXXX");
    mkdtemp(tmp);
    setenv("HOME", tmp, 1);
    unsetenv("HULL_NO_CACHE");
    unsetenv("HULL_NO_BYTECODE_CACHE");

    Chunk chunks[N_CHUNKS];
    size_t total = 0;
    for (int i = 0; i < N_CHUNKS; i++) {
        chunks[i].src = synth_chunk(i, &chunks[i].len);
        snprintf(chunks[i].name, sizeof(chunks[i].name), "=synth_%d.lua", i);
        total += chunks[i].len;
    }

    printf("─────────────────────────────────────────────────────\n");
    printf("Lua bytecode cache microbench\n");
    printf("  Chunks:        %d\n", N_CHUNKS);
    printf("  Avg src size:  %zu bytes\n", total / N_CHUNKS);
    printf("  Total src:     %zu KB\n", total / 1024);
    printf("  Cache root:    %s/.hull/blobs/runtime/lua-bytecode/\n", tmp);
    printf("─────────────────────────────────────────────────────\n\n");

    /* Cold pass: empty cache. Every load = parse + dump + rename. */
    wipe_cache(tmp);
    double cold = run_pass(chunks, N_CHUNKS);
    printf("COLD (empty cache):  %.3f ms total   %.1f µs/load\n",
            cold * 1000.0, cold * 1.0e6 / N_CHUNKS);

    /* Warm pass: cache populated. Every load = open + read + verify. */
    double warm = run_pass(chunks, N_CHUNKS);
    printf("WARM (cache hit):    %.3f ms total   %.1f µs/load\n",
            warm * 1000.0, warm * 1.0e6 / N_CHUNKS);

    /* Bypass pass: cache disabled — same as pre-cache baseline. */
    setenv("HULL_NO_BYTECODE_CACHE", "1", 1);
    double bypass = run_pass(chunks, N_CHUNKS);
    unsetenv("HULL_NO_BYTECODE_CACHE");
    printf("BYPASS (no cache):   %.3f ms total   %.1f µs/load\n",
            bypass * 1000.0, bypass * 1.0e6 / N_CHUNKS);

    printf("\nWarm-cache speedup vs bypass: %.2fx\n", bypass / warm);
    printf("Cold-cache overhead vs bypass: %.2fx\n", cold / bypass);

    /* Cleanup. */
    for (int i = 0; i < N_CHUNKS; i++) free(chunks[i].src);
    nftw(tmp, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    return 0;
}
