/*
 * commands/test.c — hull test subcommand
 *
 * In-process test runner: discovers test files, loads app,
 * wires routes, executes tests with assertions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/test.h"
#include "hull/app_context.h"
#include "hull/cap/db.h"
#include "hull/cap/tool.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "hull/cap/test.h"
#include "hull/runtime/test.h"
#include "lua.h"
#include "lauxlib.h"
#endif

#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#include "hull/cap/test.h"
#include "hull/runtime/test.h"
#include "quickjs.h"
#endif

#include <keel/router.h>
#include <keel/allocator.h>

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Usage ─────────────────────────────────────────────────────────── */

static void test_usage(void)
{
    fprintf(stderr, "Usage: hull test [app_dir]\n"
            "\n"
            "Discovers and runs test_*.[lua|js] files.\n");
}

#ifdef HL_ENABLE_LUA

/* ── Lua test runner ───────────────────────────────────────────────── */

static int run_lua_tests(const char *app_dir, const char *entry)
{
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = { .app_dir = app_dir, .entry_point = entry };
    if (hl_app_context_init(&ctx, &opts) != 0) {
        fprintf(stderr, "hull test: failed to initialize app context\n");
        return 1;
    }

    HlLua *lua = hl_app_context_lua(ctx);

    /* Wire routes into a standalone KlRouter */
    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_lua_wire_routes(lua, &router) != 0) {
        fprintf(stderr, "hull test: no routes registered\n");
        kl_router_free(&router);
        hl_app_context_free(ctx);
        return 1;
    }

    /* Register test module */
    hl_lua_test_register(lua->L, &router, lua);

    /* Discover test files */
    char **test_files = hl_tool_find_files(app_dir, "test_*.lua", NULL);
    if (!test_files || !test_files[0]) {
        fprintf(stderr, "hull test: no test files found in %s\n", app_dir);
        if (test_files) free(test_files);
        kl_router_free(&router);
        hl_app_context_free(ctx);
        return 1;
    }

    /* Run each test file */
    int total = 0, passed = 0, failed = 0;

    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        printf("\n--- %s ---\n", basename);

        /* Clear test cases from previous file */
        hl_lua_test_clear(lua->L);

        /* Load and execute the test file -> registers test cases */
        if (luaL_dofile(lua->L, file) != LUA_OK) {
            const char *err = lua_tostring(lua->L, -1);
            fprintf(stderr, "  ERROR: %s\n", err ? err : "unknown");
            lua_pop(lua->L, 1);
            failed++;
            total++;
            free(*fp);
            continue;
        }

        /* Execute registered test cases */
        int file_total = 0, file_passed = 0, file_failed = 0;
        hl_lua_test_run(lua->L, &file_total, &file_passed, &file_failed,
                            stdout, NULL, 0);

        total += file_total;
        passed += file_passed;
        failed += file_failed;

        free(*fp);
    }
    free(test_files);

    /* Report */
    printf("\n%d/%d tests passed", passed, total);
    if (failed > 0)
        printf(", %d failed", failed);
    printf("\n");

    /* Cleanup */
    kl_router_free(&router);
    hl_app_context_free(ctx);

    return failed > 0 ? 1 : 0;
}

#endif /* HL_ENABLE_LUA */

#ifdef HL_ENABLE_JS

/* ── JS test runner ────────────────────────────────────────────────── */

static int run_js_tests(const char *app_dir, const char *entry)
{
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = { .app_dir = app_dir, .entry_point = entry };
    if (hl_app_context_init(&ctx, &opts) != 0) {
        fprintf(stderr, "hull test: failed to initialize app context\n");
        return 1;
    }

    HlJS *js = hl_app_context_js(ctx);

    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_js_wire_routes(js, &router) != 0) {
        fprintf(stderr, "hull test: no routes registered\n");
        kl_router_free(&router);
        hl_app_context_free(ctx);
        return 1;
    }

    hl_js_test_register(js->ctx, &router, js);

    char **test_files = hl_tool_find_files(app_dir, "test_*.js", NULL);
    if (!test_files || !test_files[0]) {
        fprintf(stderr, "hull test: no test files found in %s\n", app_dir);
        if (test_files) free(test_files);
        kl_router_free(&router);
        hl_app_context_free(ctx);
        return 1;
    }

    int total = 0, passed = 0, failed = 0;

    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        printf("\n--- %s ---\n", basename);

        hl_js_test_clear(js->ctx);

        /* Read and evaluate the test file */
        FILE *f = fopen(file, "r");
        if (!f) {
            fprintf(stderr, "  ERROR: cannot open %s\n", file);
            failed++;
            total++;
            free(*fp);
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); free(*fp); continue; }
        long flen = ftell(f);
        if (flen < 0) { fclose(f); free(*fp); continue; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); free(*fp); continue; }
        char *src = malloc((size_t)flen + 1);
        if (!src) { fclose(f); free(*fp); continue; }
        if (fread(src, 1, (size_t)flen, f) != (size_t)flen) {
            free(src); fclose(f); free(*fp); continue;
        }
        src[flen] = '\0';
        fclose(f);

        JSValue result = JS_Eval(js->ctx, src, (size_t)flen, file,
                                 JS_EVAL_TYPE_MODULE);
        free(src);

        if (JS_IsException(result)) {
            hl_js_dump_error(js);
            JS_FreeValue(js->ctx, result);
            failed++;
            total++;
            free(*fp);
            continue;
        }
        JS_FreeValue(js->ctx, result);

        int file_total = 0, file_passed = 0, file_failed = 0;
        hl_js_test_run(js->ctx, &file_total, &file_passed, &file_failed,
                           stdout, NULL, 0);

        total += file_total;
        passed += file_passed;
        failed += file_failed;

        free(*fp);
    }
    free(test_files);

    printf("\n%d/%d tests passed", passed, total);
    if (failed > 0)
        printf(", %d failed", failed);
    printf("\n");

    kl_router_free(&router);
    hl_app_context_free(ctx);

    return failed > 0 ? 1 : 0;
}

#endif /* HL_ENABLE_JS */

/* ── Detect entry points ──────────────────────────────────────────── */

static const char *detect_lua_entry(const char *app_dir)
{
    static char lua_buf[4096];
    snprintf(lua_buf, sizeof(lua_buf), "%s/app.lua", app_dir);
    if (access(lua_buf, F_OK) == 0) return lua_buf;
    return NULL;
}

static const char *detect_js_entry(const char *app_dir)
{
    static char js_buf[4096];
    snprintf(js_buf, sizeof(js_buf), "%s/app.js", app_dir);
    if (access(js_buf, F_OK) == 0) return js_buf;
    return NULL;
}

/* ── Command entry point ───────────────────────────────────────────── */

int hl_cmd_test(int argc, char **argv, const HlCommandEnv *env)
{
    const char *app_dir = env->app_dir;
    if (argc >= 2 && argv[1][0] != '-')
        app_dir = argv[1];

    const char *lua_entry = detect_lua_entry(app_dir);
    const char *js_entry = detect_js_entry(app_dir);

    if (!lua_entry && !js_entry) {
        fprintf(stderr, "hull test: no entry point found (app.js or app.lua) in %s\n",
                app_dir);
        test_usage();
        return 1;
    }

    int result = 0;
    int ran_any = 0;

    /* Try Lua tests if app.lua exists and test_*.lua files are present */
#ifdef HL_ENABLE_LUA
    if (lua_entry) {
        char **lua_tests = hl_tool_find_files(app_dir, "test_*.lua", NULL);
        if (lua_tests && lua_tests[0]) {
            ran_any = 1;
            result |= run_lua_tests(app_dir, lua_entry);
        }
        if (lua_tests) {
            for (char **fp = lua_tests; *fp; fp++) free(*fp);
            free(lua_tests);
        }
    }
#endif

    /* Try JS tests if app.js exists and test_*.js files are present */
#ifdef HL_ENABLE_JS
    if (js_entry) {
        char **js_tests = hl_tool_find_files(app_dir, "test_*.js", NULL);
        if (js_tests && js_tests[0]) {
            ran_any = 1;
            result |= run_js_tests(app_dir, js_entry);
        }
        if (js_tests) {
            for (char **fp = js_tests; *fp; fp++) free(*fp);
            free(js_tests);
        }
    }
#endif

    // cppcheck-suppress knownConditionTrueFalse
    if (!ran_any) {
        fprintf(stderr, "hull test: no test files found in %s\n", app_dir);
        return 1;
    }

    return result;
}
