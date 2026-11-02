/*
 * complexity.c - engine complexity tests (audit convergence §6.5)
 *
 * Runs each listed QuickJS / Lua builtin on a worst-case-SHAPED ordinary
 * input at sizes n and 2n and asserts t(2n) / t(n) < 2.5: a linear builtin
 * doubles, an n log n one (sort) a little more, a quadratic one quadruples.
 * Each time is the THREAD CPU time of the work (CLOCK_THREAD_CPUTIME_ID), so
 * a loaded or preempted runner does not count time the thread spent off the
 * CPU, and it is the minimum of several runs (noise only ever adds time). A
 * case that fails is measured again, up to ATTEMPTS times in all, before it
 * counts, and every ratio is printed. The ratio does not depend on how fast
 * the machine is, so a sanitizer build (HL_METER_SLOWDOWN) needs no
 * adjustment.
 *
 * The engines run raw - the vendored QuickJS and Lua with Hull's patches,
 * no Hull runtime around them - so the numbers are the engine's own. Inputs
 * are ordinary shapes: long runs of combining marks, large character
 * classes, many alternatives, deep and wide JSON, many keys. Hash structures
 * are not attacked with crafted collisions: they run on the per-runtime
 * keyed hash (QuickJS patch 0009), and the one collision case uses the
 * test-only hook QJS_HULL_TEST_HOOKS provides (every atom on one chain) to
 * check that the cost of a long chain is CHARGED (patch 0005 / 0009) in
 * step with its time, which is what bounds it in Hull.
 *
 * A QuickJS or Lua upgrade must pass this suite (docs/quickjs_patches.md,
 * docs/lua_patches.md). Run: make complexity  (about 30-60 s).
 *
 * Usage: build/test_complexity [--filter SUBSTR] [--verbose]
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "quickjs.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#define RATIO_MAX   2.5
#define RUNS        5        /* minimum of */
#define ATTEMPTS    3        /* a failing case is measured again */
#define MIN_MS      4.0      /* a case whose t(n) is below this is too small */

static const char *g_filter;
static int g_verbose;
static int g_fail, g_pass;

static double clock_ms(clockid_t id)
{
    struct timespec t;
    clock_gettime(id, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Wall time, for the suite's total only. */
static double now_ms(void) { return clock_ms(CLOCK_MONOTONIC); }

/* The calling thread's CPU time: what each case is measured in. */
static double cpu_ms(void) { return clock_ms(CLOCK_THREAD_CPUTIME_ID); }

static double minimum(const double *v, int n)
{
    double m = v[0];
    for (int i = 1; i < n; i++) if (v[i] < m) m = v[i];
    return m;
}

/* ── QuickJS ───────────────────────────────────────────────────────────── */

typedef struct JsCase {
    const char *name;
    long n;
    /* defines setup(n) -> state and work(state); only work is timed */
    const char *src;
    int force_atom_hash;   /* the collision-hook case */
} JsCase;

static uint64_t g_js_units;
static int js_count_work(JSRuntime *rt, void *opaque, uint64_t units)
{
    (void)rt; (void)opaque;
    g_js_units += units;
    return 0;
}

static void js_dump_exc(JSContext *ctx, const char *what)
{
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    fprintf(stderr, "    %s: %s\n", what, s ? s : "?");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
}

/* least CPU time (ms) of work(setup(n)); *units = charged work of the last run */
static double js_time(const JsCase *c, long n, uint64_t *units)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    double out = -1;
    JS_SetMemoryLimit(rt, (size_t)2 << 30);
    JS_SetMaxStackSize(rt, 0);
    JS_SetWorkHandler(rt, js_count_work, NULL);
#ifdef QJS_HULL_TEST_HOOKS
    if (c->force_atom_hash) JS_TestForceAtomHash(rt, 1);
#endif
    JSValue r = JS_Eval(ctx, c->src, strlen(c->src), c->name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { js_dump_exc(ctx, "eval"); goto done; }
    JS_FreeValue(ctx, r);
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue setup = JS_GetPropertyStr(ctx, g, "setup");
    JSValue work = JS_GetPropertyStr(ctx, g, "work");
    double t[RUNS];
    int ok = 1;
    for (int k = 0; k < RUNS && ok; k++) {
        JSValue arg = JS_NewFloat64(ctx, (double)n);
        JSValue st = JS_Call(ctx, setup, JS_UNDEFINED, 1, &arg);
        if (JS_IsException(st)) { js_dump_exc(ctx, "setup"); ok = 0; break; }
        JS_RunGC(rt);
        g_js_units = 0;
        double t0 = cpu_ms();
        JSValue v = JS_Call(ctx, work, JS_UNDEFINED, 1, &st);
        t[k] = cpu_ms() - t0;
        if (units) *units = g_js_units;
        if (JS_IsException(v)) { js_dump_exc(ctx, "work"); ok = 0; }
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, st);
    }
    if (ok) out = minimum(t, RUNS);
    JS_FreeValue(ctx, setup);
    JS_FreeValue(ctx, work);
    JS_FreeValue(ctx, g);
done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return out;
}

/* ── Lua ───────────────────────────────────────────────────────────────── */

typedef struct LuaCase {
    const char *name;
    long n;
    const char *src;   /* defines setup(n) -> state and work(state) */
} LuaCase;

static double lua_time(const LuaCase *c, long n)
{
    lua_State *L = luaL_newstate();
    double out = -1;
    if (!L) return -1;
    luaL_openlibs(L);
    if (luaL_dostring(L, c->src) != LUA_OK) {
        fprintf(stderr, "    load: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return -1;
    }
    double t[RUNS];
    int k;
    for (k = 0; k < RUNS; k++) {
        lua_getglobal(L, "setup");
        lua_pushinteger(L, (lua_Integer)n);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            fprintf(stderr, "    setup: %s\n", lua_tostring(L, -1));
            break;
        }
        lua_gc(L, LUA_GCCOLLECT);
        lua_getglobal(L, "work");
        lua_insert(L, -2);
        double t0 = cpu_ms();
        int rc = lua_pcall(L, 1, 0, 0);
        t[k] = cpu_ms() - t0;
        if (rc != LUA_OK) {
            fprintf(stderr, "    work: %s\n", lua_tostring(L, -1));
            break;
        }
    }
    if (k == RUNS) out = minimum(t, RUNS);
    lua_close(L);
    return out;
}

/* ── the cases ─────────────────────────────────────────────────────────── */

/* Helpers every JS case may use. */
#define JS_PRE \
    "const marks = (n) => { const M = ['\\u0345', '\\u0316', '\\u0300', '\\u0334'];" \
    " let s = 'a'; for (let k = 0; k < n; k++) s += M[k & 3]; return s; };\n"

static const JsCase js_cases[] = {
    /* String */
    { "js.string.indexOf_miss", 40000, JS_PRE
      "function setup(n) { return ['ab'.repeat(n), 'ab'.repeat(64) + 'c']; }\n"
      "function work(s) { for (let i = 0; i < 20; i++) s[0].indexOf(s[1]); }\n", 0 },
    { "js.string.includes_miss", 40000, JS_PRE
      "function setup(n) { return ['ab'.repeat(n), 'ab'.repeat(64) + 'c']; }\n"
      "function work(s) { for (let i = 0; i < 20; i++) s[0].includes(s[1]); }\n", 0 },
    { "js.string.split", 300000, JS_PRE
      "function setup(n) { return 'abc,'.repeat(n); }\n"
      "function work(s) { s.split(','); }\n", 0 },
    { "js.string.replace_all", 300000, JS_PRE
      "function setup(n) { return 'abc,'.repeat(n); }\n"
      "function work(s) { s.replaceAll(',', ';;'); s.replace(/,/g, ';'); }\n", 0 },
    { "js.string.normalize_nfc", 200000, JS_PRE
      "function setup(n) { return marks(n); }\n"
      "function work(s) { s.normalize('NFC'); }\n", 0 },
    { "js.string.normalize_nfd", 200000, JS_PRE
      "function setup(n) { return marks(n); }\n"
      "function work(s) { s.normalize('NFD'); }\n", 0 },
    { "js.string.localeCompare", 200000, JS_PRE
      "function setup(n) { return [marks(n), marks(n) + 'b']; }\n"
      "function work(s) { s[0].localeCompare(s[1]); }\n", 0 },
    { "js.string.padStart", 4000000, JS_PRE
      "function setup(n) { return n; }\n"
      "function work(n) { 'x'.padStart(n, 'abc'); 'x'.padEnd(n, 'abc'); }\n", 0 },
    { "js.string.repeat", 4000000, JS_PRE
      "function setup(n) { return n; }\n"
      "function work(n) { 'abc'.repeat(n).length; }\n", 0 },
    /* RegExp compile */
    { "js.regexp.compile_large_class", 30000, JS_PRE
      "function setup(n) { let m = ''; for (let k = 0; k < n; k++) m += String.fromCodePoint(0x10000 + 2 * k);"
      " return '[' + m + ']'; }\n"
      "function work(p) { for (let i = 0; i < 4; i++) { new RegExp(p, 'u'); new RegExp(p, 'v'); } }\n", 0 },
    { "js.regexp.compile_alternatives", 250000, JS_PRE
      "function setup(n) { return 'ab|'.repeat(n) + 'ab'; }\n"
      "function work(p) { new RegExp(p); }\n", 0 },
    { "js.regexp.compile_lookbehind", 150000, JS_PRE
      "function setup(n) { return '(?<=' + 'ab'.repeat(n) + ')'; }\n"
      "function work(p) { new RegExp(p); }\n", 0 },
    { "js.regexp.exec_linear", 400000, JS_PRE
      "function setup(n) { return 'abc '.repeat(n); }\n"
      "function work(s) { s.match(/[a-c]+/g); /x/.test(s); }\n", 0 },
    /* JSON */
    { "js.json.parse_wide", 100000, JS_PRE
      "function setup(n) { return '[' + Array.from({ length: n }, (_, i) => '{\"id\":' + i + ',\"name\":\"n' + i + '\"}').join(',') + ']'; }\n"
      "function work(s) { JSON.parse(s); }\n", 0 },
    { "js.json.parse_deep", 2000, JS_PRE
      "function setup(n) { const one = '['.repeat(100) + ']'.repeat(100);"
      " return '[' + Array.from({ length: n }, () => one).join(',') + ']'; }\n"
      "function work(s) { JSON.parse(s); }\n", 0 },
    { "js.json.parse_many_keys", 30000, JS_PRE
      "function setup(n) { return '{' + Array.from({ length: n }, (_, i) => '\"key' + i + '\":' + i).join(',') + '}'; }\n"
      "function work(s) { JSON.parse(s); }\n", 0 },
    { "js.json.stringify_wide", 100000, JS_PRE
      "function setup(n) { return Array.from({ length: n }, (_, i) => ({ id: i, name: 'n' + i })); }\n"
      "function work(a) { JSON.stringify(a); }\n", 0 },
    { "js.json.stringify_deep", 2000, JS_PRE
      "function setup(n) { const one = JSON.parse('['.repeat(100) + ']'.repeat(100));"
      " return Array.from({ length: n }, () => one); }\n"
      "function work(a) { JSON.stringify(a); }\n", 0 },
    /* Array */
    { "js.array.sort", 100000, JS_PRE
      "function setup(n) { let x = 12345; return Array.from({ length: n }, () => (x = (x * 1103515245 + 12345) % 2147483648)); }\n"
      "function work(a) { a.slice().sort((p, q) => p - q); }\n", 0 },
    { "js.array.indexOf_miss", 1000000, JS_PRE
      "function setup(n) { return Array.from({ length: n }, (_, i) => i); }\n"
      "function work(a) { for (let i = 0; i < 10; i++) a.indexOf(-1); }\n", 0 },
    { "js.array.splice_end", 200000, JS_PRE
      "function setup(n) { return n; }\n"
      "function work(n) { const a = []; for (let i = 0; i < n; i++) a.push(i);"
      " while (a.length) a.splice(a.length - 1, 1); }\n", 0 },
    { "js.array.join", 500000, JS_PRE
      "function setup(n) { return Array.from({ length: n }, (_, i) => 'v' + i); }\n"
      "function work(a) { a.join(','); }\n", 0 },
    /* TypedArray */
    { "js.typedarray.sort", 400000, JS_PRE
      "function setup(n) { const a = new Float64Array(n); let x = 7;"
      " for (let i = 0; i < n; i++) a[i] = (x = (x * 1103515245 + 12345) % 2147483648); return a; }\n"
      "function work(a) { a.slice().sort(); }\n", 0 },
    { "js.typedarray.set_fill", 16000000, JS_PRE
      "function setup(n) { return [new Uint8Array(n), new Uint8Array(n)]; }\n"
      "function work(s) { for (let i = 0; i < 10; i++) { s[0].fill(i); s[1].set(s[0]); } }\n", 0 },
    /* Map / Set (keyed hash, patch 0009) */
    { "js.map.insert_lookup", 50000, JS_PRE
      "function setup(n) { return Array.from({ length: n }, (_, i) => 'key' + i); }\n"
      "function work(k) { const m = new Map(); for (const x of k) m.set(x, 1);"
      " let c = 0; for (const x of k) c += m.get(x); }\n", 0 },
    { "js.set.insert_lookup", 50000, JS_PRE
      "function setup(n) { return Array.from({ length: n }, (_, i) => i * 7.5); }\n"
      "function work(k) { const s = new Set(); for (const x of k) s.add(x);"
      " let c = 0; for (const x of k) if (s.has(x)) c++; }\n", 0 },
    /* Object property insertion (keyed atom hash, patch 0009) */
    { "js.object.insert", 40000, JS_PRE
      "function setup(n) { return Array.from({ length: n }, (_, i) => 'p' + i); }\n"
      "function work(k) { for (let r = 0; r < 4; r++) { const o = {}; for (const x of k) o[x] = 1; } }\n", 0 },
};

/* With every atom on one chain the insertion is quadratic by construction;
 * what Hull relies on is that the chain walks are charged, so the budget
 * stops it. The check: the charged units grow in step with the time. */
static const JsCase js_collision_case = {
    "js.object.insert_forced_collisions", 3000, JS_PRE
    "function setup(n) { return Array.from({ length: n }, (_, i) => 'q' + i); }\n"
    "function work(k) { const o = {}; for (const x of k) o[x] = 1; }\n", 1
};

static const LuaCase lua_cases[] = {
    { "lua.table.insert_remove", 60000,
      "function setup(n) return n end\n"
      "function work(n) local t = {} for i = 1, n do table.insert(t, i) end"
      " for i = 1, n do table.remove(t) end end\n" },
    { "lua.table.sort", 100000,
      "function setup(n) local t, x = {}, 7 for i = 1, n do x = (x * 1103515245 + 12345) % 2147483648; t[i] = x end return t end\n"
      "function work(t) local c = table.move(t, 1, #t, 1, {}) table.sort(c) end\n" },
    { "lua.table.concat", 400000,
      "function setup(n) local t = {} for i = 1, n do t[i] = 'v' .. i end return t end\n"
      "function work(t) table.concat(t, ',') end\n" },
    { "lua.string.rep", 4000000,
      "function setup(n) return n end\n"
      "function work(n) local s = string.rep('abc', n) end\n" },
    { "lua.string.find_plain_miss", 200000,
      "function setup(n) return { string.rep('ab', n), string.rep('ab', 64) .. 'c' } end\n"
      "function work(s) for i = 1, 20 do string.find(s[1], s[2], 1, true) end end\n" },
    { "lua.string.gsub", 300000,
      "function setup(n) return string.rep('abc,', n) end\n"
      "function work(s) string.gsub(s, ',', ';;') end\n" },
    { "lua.string.format", 100000,
      "function setup(n) local t = {} for i = 1, n do t[i] = i end return t end\n"
      "function work(t) for i = 1, #t do string.format('%d:%s:%5.2f', t[i], 'x', t[i] / 3) end end\n" },
    { "lua.string.byte_char", 1000000,
      "function setup(n) return string.rep('abcdefgh', n // 8) end\n"
      "function work(s) local b = { string.byte(s, 1, 4000) } for i = 1, #s, 4000 do"
      " local c = string.char(string.byte(s, i, i + 3999)) end end\n" },
    { "lua.gc.full_collect", 500000,
      "function setup(n) local t = {} for i = 1, n do t[i] = { i, tostring(i) } end return t end\n"
      "function work(t) collectgarbage('collect') end\n" },
    { "lua.coroutine.create_resume", 100000,
      "function setup(n) return n end\n"
      "function work(n) for i = 1, n do local co = coroutine.create(function(a) return coroutine.yield(a) end)"
      " coroutine.resume(co, i) coroutine.resume(co, i) end end\n" },
};

/* ── driver ────────────────────────────────────────────────────────────── */

static int wanted(const char *name)
{
    return !g_filter || strstr(name, g_filter) != NULL;
}

static void report(const char *name, double t1, double t2, double ratio, int ok,
                   const char *note)
{
    printf("  %-38s t(n)=%8.2f ms  t(2n)=%8.2f ms  ratio=%.2f  %s%s%s\n",
           name, t1, t2, ratio, ok ? "ok" : "FAIL",
           note ? "  " : "", note ? note : "");
    fflush(stdout);
    if (ok) g_pass++; else g_fail++;
}

static void run_js(const JsCase *c)
{
    if (!wanted(c->name)) return;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        double t1 = js_time(c, c->n, NULL), t2 = js_time(c, 2 * c->n, NULL);
        if (t1 < 0 || t2 < 0) { report(c->name, t1, t2, 0, 0, "error"); return; }
        double ratio = t2 / (t1 > 0.001 ? t1 : 0.001);
        int ok = ratio < RATIO_MAX;
        if (ok || attempt == ATTEMPTS - 1) {
            report(c->name, t1, t2, ratio, ok,
                   t1 < MIN_MS ? "(t(n) small: noisy)" : (attempt ? "(retried)" : NULL));
            return;
        }
        if (g_verbose) printf("  %s ratio %.2f, retrying\n", c->name, ratio);
    }
}

static void run_js_collision(void)
{
#ifdef QJS_HULL_TEST_HOOKS
    const JsCase *c = &js_collision_case;
    if (!wanted(c->name)) return;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        uint64_t u1 = 0, u2 = 0;
        double t1 = js_time(c, c->n, &u1), t2 = js_time(c, 2 * c->n, &u2);
        if (t1 < 0 || t2 < 0 || u1 == 0) { report(c->name, t1, t2, 0, 0, "error"); return; }
        double tr = t2 / (t1 > 0.001 ? t1 : 0.001);
        double ur = (double)u2 / (double)u1;
        /* time and charge grow alike (both ~4x here): the charge tracks it */
        double k = tr / ur;
        int ok = k > 1.0 / RATIO_MAX && k < RATIO_MAX && ur > 3.0;
        char note[96];
        snprintf(note, sizeof note, "charged units x%.2f (time/charge %.2f)", ur, k);
        if (ok || attempt == ATTEMPTS - 1) { report(c->name, t1, t2, tr, ok, note); return; }
    }
#endif
}

static void run_lua(const LuaCase *c)
{
    if (!wanted(c->name)) return;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        double t1 = lua_time(c, c->n), t2 = lua_time(c, 2 * c->n);
        if (t1 < 0 || t2 < 0) { report(c->name, t1, t2, 0, 0, "error"); return; }
        double ratio = t2 / (t1 > 0.001 ? t1 : 0.001);
        int ok = ratio < RATIO_MAX;
        if (ok || attempt == ATTEMPTS - 1) {
            report(c->name, t1, t2, ratio, ok,
                   t1 < MIN_MS ? "(t(n) small: noisy)" : (attempt ? "(retried)" : NULL));
            return;
        }
        if (g_verbose) printf("  %s ratio %.2f, retrying\n", c->name, ratio);
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) g_filter = argv[++i];
        else if (strcmp(argv[i], "--verbose") == 0) g_verbose = 1;
    }
    double t0 = now_ms();
    printf("engine complexity: t(2n)/t(n) < %.1f, thread CPU time, least of %d\n",
           RATIO_MAX, RUNS);
    for (size_t i = 0; i < sizeof js_cases / sizeof js_cases[0]; i++)
        run_js(&js_cases[i]);
    run_js_collision();
    for (size_t i = 0; i < sizeof lua_cases / sizeof lua_cases[0]; i++)
        run_lua(&lua_cases[i]);
    printf("engine complexity: %d passed, %d failed (%.1f s)\n",
           g_pass, g_fail, (now_ms() - t0) / 1e3);
    return g_fail ? 1 : 0;
}
