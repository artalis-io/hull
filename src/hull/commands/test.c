/*
 * commands/test.c — hull test subcommand
 *
 * In-process test runner: discovers test files, loads app,
 * wires routes, executes tests with assertions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/test.h"
#include "hull/cap/tool.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "hull/cap/test.h"
#include "lua.h"
#include "lauxlib.h"
#endif

#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#include "hull/cap/test.h"
#include "quickjs.h"
#endif

#include <keel/router.h>
#include <keel/allocator.h>

#include <sqlite3.h>
#include <sh_arena.h>

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

/* ── Detect entry point ────────────────────────────────────────────── */

static const char *detect_entry(const char *app_dir)
{
    static char buf[4096];

    snprintf(buf, sizeof(buf), "%s/app.js", app_dir);
    if (access(buf, F_OK) == 0) return buf;

    snprintf(buf, sizeof(buf), "%s/app.lua", app_dir);
    if (access(buf, F_OK) == 0) return buf;

    return NULL;
}

static int is_js_entry(const char *entry)
{
    const char *ext = strrchr(entry, '.');
    return ext && strcmp(ext, ".js") == 0;
}

#ifdef HL_ENABLE_LUA

/* ── Lua test runner ───────────────────────────────────────────────── */

static int run_lua_tests(const char *app_dir, const char *entry)
{
    /* Init Lua VM (sandboxed — tests run in app context) */
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 1;

    HlLua lua;
    memset(&lua, 0, sizeof(lua));

    /* Open :memory: SQLite for test isolation */
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "hull test: cannot open :memory: database\n");
        return 1;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    lua.base.db = db;

    if (hl_lua_init(&lua, &cfg) != 0) {
        fprintf(stderr, "hull test: Lua init failed\n");
        sqlite3_close(db);
        return 1;
    }

    /* Load app entry point → routes registered */
    if (hl_lua_load_app(&lua, entry) != 0) {
        fprintf(stderr, "hull test: failed to load %s\n", entry);
        hl_lua_free(&lua);
        sqlite3_close(db);
        return 1;
    }

    /* Wire routes into a standalone KlRouter */
    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_lua_wire_routes(&lua, &router) != 0) {
        fprintf(stderr, "hull test: no routes registered\n");
        kl_router_free(&router);
        hl_lua_free(&lua);
        sqlite3_close(db);
        return 1;
    }

    /* Register test module */
    hl_cap_test_register_lua(lua.L, &router, &lua);

    /* Discover test files */
    char **test_files = hl_tool_find_files(app_dir, "test_*.lua", NULL);
    if (!test_files || !test_files[0]) {
        fprintf(stderr, "hull test: no test files found in %s\n", app_dir);
        if (test_files) free(test_files);
        kl_router_free(&router);
        hl_lua_free(&lua);
        sqlite3_close(db);
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
        hl_cap_test_clear_lua(lua.L);

        /* Load and execute the test file → registers test cases */
        if (luaL_dofile(lua.L, file) != LUA_OK) {
            const char *err = lua_tostring(lua.L, -1);
            fprintf(stderr, "  ERROR: %s\n", err ? err : "unknown");
            lua_pop(lua.L, 1);
            failed++;
            total++;
            free(*fp);
            continue;
        }

        /* Execute registered test cases */
        int file_total = 0, file_passed = 0, file_failed = 0;
        hl_cap_test_run_lua(lua.L, &file_total, &file_passed, &file_failed);

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
    hl_lua_free(&lua);
    sqlite3_close(db);

    return failed > 0 ? 1 : 0;
}

#endif /* HL_ENABLE_LUA */

#ifdef HL_ENABLE_JS

/* ── JS test runner ────────────────────────────────────────────────── */

static int run_js_tests(const char *app_dir, const char *entry)
{
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    HlJS js;
    memset(&js, 0, sizeof(js));

    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "hull test: cannot open :memory: database\n");
        return 1;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    js.base.db = db;

    if (hl_js_init(&js, &cfg) != 0) {
        fprintf(stderr, "hull test: QuickJS init failed\n");
        sqlite3_close(db);
        return 1;
    }

    if (hl_js_load_app(&js, entry) != 0) {
        fprintf(stderr, "hull test: failed to load %s\n", entry);
        hl_js_free(&js);
        sqlite3_close(db);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlRouter router;
    kl_router_init(&router, &alloc);

    if (hl_js_wire_routes(&js, &router) != 0) {
        fprintf(stderr, "hull test: no routes registered\n");
        kl_router_free(&router);
        hl_js_free(&js);
        sqlite3_close(db);
        return 1;
    }

    hl_cap_test_register_js(js.ctx, &router, &js);

    char **test_files = hl_tool_find_files(app_dir, "test_*.js", NULL);
    if (!test_files || !test_files[0]) {
        fprintf(stderr, "hull test: no test files found in %s\n", app_dir);
        if (test_files) free(test_files);
        kl_router_free(&router);
        hl_js_free(&js);
        sqlite3_close(db);
        return 1;
    }

    int total = 0, passed = 0, failed = 0;

    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        printf("\n--- %s ---\n", basename);

        hl_cap_test_clear_js(js.ctx);

        /* Read and evaluate the test file */
        FILE *f = fopen(file, "r");
        if (!f) {
            fprintf(stderr, "  ERROR: cannot open %s\n", file);
            failed++;
            total++;
            free(*fp);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        if (flen < 0) { fclose(f); free(*fp); continue; }
        fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)flen + 1);
        if (!src) { fclose(f); free(*fp); continue; }
        if (fread(src, 1, (size_t)flen, f) != (size_t)flen) {
            free(src); fclose(f); free(*fp); continue;
        }
        src[flen] = '\0';
        fclose(f);

        JSValue result = JS_Eval(js.ctx, src, (size_t)flen, file,
                                 JS_EVAL_TYPE_MODULE);
        free(src);

        if (JS_IsException(result)) {
            hl_js_dump_error(&js);
            JS_FreeValue(js.ctx, result);
            failed++;
            total++;
            free(*fp);
            continue;
        }
        JS_FreeValue(js.ctx, result);

        int file_total = 0, file_passed = 0, file_failed = 0;
        hl_cap_test_run_js(js.ctx, &file_total, &file_passed, &file_failed);

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
    hl_js_free(&js);
    sqlite3_close(db);

    return failed > 0 ? 1 : 0;
}

#endif /* HL_ENABLE_JS */

/* ── Command entry point ───────────────────────────────────────────── */

int hl_cmd_test(int argc, char **argv, const char *hull_exe)
{
    (void)hull_exe;

    const char *app_dir = ".";
    if (argc >= 2 && argv[1][0] != '-')
        app_dir = argv[1];

    /* Detect entry point */
    const char *entry = detect_entry(app_dir);
    if (!entry) {
        fprintf(stderr, "hull test: no entry point found (app.js or app.lua) in %s\n",
                app_dir);
        test_usage();
        return 1;
    }

    int is_js = is_js_entry(entry);
    (void)is_js; /* may be unused if runtime not compiled in */

#ifdef HL_ENABLE_JS
    if (is_js)
        return run_js_tests(app_dir, entry);
#endif

#ifdef HL_ENABLE_LUA
    if (!is_js)
        return run_lua_tests(app_dir, entry);
#endif

    fprintf(stderr, "hull test: runtime for %s not enabled in this build\n", entry);
    return 1;
}
