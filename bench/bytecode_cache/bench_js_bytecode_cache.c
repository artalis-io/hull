/*
 * bench_js_bytecode_cache.c - Cold-vs-warm comparison for
 * hl_js_compile_module_cached. Mirrors the Lua bench
 * (bench_bytecode_cache.c) so the two languages report
 * apples-to-apples timings.
 *
 * Synthesizes N realistic-sized ES modules, then runs three
 * passes:
 *   1. Cold: $HOME/.hull/blobs/runtime/js-bytecode/ is empty →
 *      every load parses source AND persists bytecode.
 *   2. Warm: cache populated → every load deserializes via
 *      JS_ReadObject; parser pass skipped entirely.
 *   3. Bypass: HULL_NO_JS_BYTECODE_CACHE=1 → plain JS_Eval
 *      every time, no cache I/O at all.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js_bytecode_cache.h"

#include "quickjs.h"

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

typedef struct {
    char  *src;
    size_t len;
    char   name[64];
} Module;

static char *synth_module(int seed, size_t *out_len)
{
    size_t cap = 4096;
    char *buf = malloc(cap);
    int n = 0;
    n += snprintf(buf + n, cap - n,
        "// Synthetic ES module #%d\n"
        "function pad_%d(x, y) { return x * %d + y; }\n",
        seed, seed, seed * 7 + 11);
    for (int i = 0; i < 20; i++) {
        n += snprintf(buf + n, cap - n,
            "export function case_%d(v) {\n"
            "    const t = { idx: %d, op: 'case', val: v };\n"
            "    if (pad_%d(v, %d) > %d) {\n"
            "        t.r = v * 2 - %d;\n"
            "    } else {\n"
            "        t.r = v + %d;\n"
            "    }\n"
            "    return t;\n"
            "}\n",
            i, i, seed, i, seed * (i + 1), i + 3, i * 2 + 1);
    }
    *out_len = (size_t)n;
    return buf;
}

#define N_MODULES 70

static int rm_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void wipe_cache(const char *home)
{
    char path[512];
    snprintf(path, sizeof(path),
             "%s/.hull/blobs/runtime/js-bytecode", home);
    nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static double run_pass(Module *mods, int n)
{
    double t0 = now_sec();
    for (int i = 0; i < n; i++) {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        JSValue v = hl_js_compile_module_cached(
            ctx, mods[i].src, mods[i].len, mods[i].name);
        if (JS_IsException(v)) {
            fprintf(stderr, "compile failed on module %d\n", i);
            exit(1);
        }
        JS_FreeValue(ctx, v);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    return now_sec() - t0;
}

int main(void)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/hull_jbc_bench_XXXXXX");
    mkdtemp(tmp);
    setenv("HOME", tmp, 1);
    unsetenv("HULL_NO_CACHE");
    unsetenv("HULL_NO_JS_BYTECODE_CACHE");
    /* Make sure the bytecode cache module re-resolves under our
     * new HOME (singleton reset). */
    hl_js_bytecode_cache_reset();

    Module mods[N_MODULES];
    size_t total = 0;
    for (int i = 0; i < N_MODULES; i++) {
        mods[i].src = synth_module(i, &mods[i].len);
        snprintf(mods[i].name, sizeof(mods[i].name), "synth_%d.js", i);
        total += mods[i].len;
    }

    printf("─────────────────────────────────────────────────────\n");
    printf("QuickJS bytecode cache microbench\n");
    printf("  Modules:       %d\n", N_MODULES);
    printf("  Avg src size:  %zu bytes\n", total / N_MODULES);
    printf("  Total src:     %zu KB\n", total / 1024);
    printf("  Cache root:    %s/.hull/blobs/runtime/js-bytecode/\n", tmp);
    printf("─────────────────────────────────────────────────────\n\n");

    wipe_cache(tmp);
    hl_js_bytecode_cache_reset();
    double cold = run_pass(mods, N_MODULES);
    printf("COLD (empty cache):  %.3f ms total   %.1f µs/load\n",
            cold * 1000.0, cold * 1.0e6 / N_MODULES);

    double warm = run_pass(mods, N_MODULES);
    printf("WARM (cache hit):    %.3f ms total   %.1f µs/load\n",
            warm * 1000.0, warm * 1.0e6 / N_MODULES);

    setenv("HULL_NO_JS_BYTECODE_CACHE", "1", 1);
    double bypass = run_pass(mods, N_MODULES);
    unsetenv("HULL_NO_JS_BYTECODE_CACHE");
    printf("BYPASS (no cache):   %.3f ms total   %.1f µs/load\n",
            bypass * 1000.0, bypass * 1.0e6 / N_MODULES);

    printf("\nWarm-cache speedup vs bypass: %.2fx\n", bypass / warm);
    printf("Cold-cache overhead vs bypass: %.2fx\n", cold / bypass);

    for (int i = 0; i < N_MODULES; i++) free(mods[i].src);
    nftw(tmp, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    return 0;
}
