/*
 * main.c — Hull application entry point
 *
 * Detects runtime from entry point file extension (.lua → Lua, .js → QuickJS).
 * Initializes the selected runtime, opens SQLite database, registers routes
 * with Keel, and enters the event loop.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/js_runtime.h"
#include "hull/lua_runtime.h"
#include "hull/hull_cap.h"
#include "quickjs.h"

#include "lua.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Runtime selection ──────────────────────────────────────────────── */

typedef enum {
    HULL_RUNTIME_LUA = 0,
    HULL_RUNTIME_JS  = 1,
} HullRuntimeType;

static HullRuntimeType detect_runtime(const char *entry_point)
{
    const char *ext = strrchr(entry_point, '.');
    if (ext && strcmp(ext, ".js") == 0)
        return HULL_RUNTIME_JS;
    return HULL_RUNTIME_LUA; /* default */
}

/* ── JS handler bridge ──────────────────────────────────────────────── */

typedef struct {
    HullJS *js;
    int     handler_id;
} HullJSRoute;

static void hull_js_keel_handler(KlRequest *req, KlResponse *res,
                                  void *user_data)
{
    HullJSRoute *route = (HullJSRoute *)user_data;
    if (hull_js_dispatch(route->js, route->handler_id, req, res) != 0) {
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body(res, "Internal Server Error", 21);
    }
}

/* ── Wire JS routes into Keel ───────────────────────────────────────── */

static int wire_js_routes(HullJS *js, KlServer *server)
{
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");

    if (JS_IsUndefined(defs) || !JS_IsArray(ctx, defs)) {
        JS_FreeValue(ctx, defs);
        JS_FreeValue(ctx, global);
        fprintf(stderr, "hull: no routes registered\n");
        return -1;
    }

    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < count; i++) {
        JSValue def = JS_GetPropertyUint32(ctx, defs, (uint32_t)i);
        if (JS_IsUndefined(def))
            continue;

        JSValue method_val = JS_GetPropertyStr(ctx, def, "method");
        JSValue pattern_val = JS_GetPropertyStr(ctx, def, "pattern");
        JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");

        const char *method_str = JS_ToCString(ctx, method_val);
        const char *pattern = JS_ToCString(ctx, pattern_val);
        int32_t handler_id = 0;
        JS_ToInt32(ctx, &handler_id, id_val);

        if (method_str && pattern) {
            /* Allocate route context (lives for duration of server) */
            HullJSRoute *route = malloc(sizeof(HullJSRoute));
            if (route) {
                route->js = js;
                route->handler_id = handler_id;
                kl_server_route(server, method_str, pattern,
                                hull_js_keel_handler, route, NULL);
            }
        }

        if (pattern) JS_FreeCString(ctx, pattern);
        if (method_str) JS_FreeCString(ctx, method_str);
        JS_FreeValue(ctx, id_val);
        JS_FreeValue(ctx, pattern_val);
        JS_FreeValue(ctx, method_val);
        JS_FreeValue(ctx, def);
    }

    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, global);

    return 0;
}

/* ── Lua handler bridge ─────────────────────────────────────────────── */

typedef struct {
    HullLua *lua;
    int      handler_id;
} HullLuaRoute;

static void hull_lua_keel_handler(KlRequest *req, KlResponse *res,
                                   void *user_data)
{
    HullLuaRoute *route = (HullLuaRoute *)user_data;
    if (hull_lua_dispatch(route->lua, route->handler_id, req, res) != 0) {
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body(res, "Internal Server Error", 21);
    }
}

/* ── Wire Lua routes into Keel ─────────────────────────────────────── */

static int wire_lua_routes(HullLua *lua, KlServer *server)
{
    lua_State *L = lua->L;

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        fprintf(stderr, "hull: no routes registered\n");
        return -1;
    }

    int count = (int)luaL_len(L, -1);
    if (count <= 0) {
        lua_pop(L, 1);
        fprintf(stderr, "hull: no routes registered\n");
        return -1;
    }

    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "method");
        lua_getfield(L, -2, "pattern");
        lua_getfield(L, -3, "handler_id");

        const char *method_str = lua_tostring(L, -3);
        const char *pattern = lua_tostring(L, -2);
        int handler_id = (int)lua_tointeger(L, -1);

        if (method_str && pattern) {
            HullLuaRoute *route = malloc(sizeof(HullLuaRoute));
            if (route) {
                route->lua = lua;
                route->handler_id = handler_id;
                kl_server_route(server, method_str, pattern,
                                hull_lua_keel_handler, route, NULL);
            }
        }

        lua_pop(L, 3); /* method_str, pattern, handler_id */
        lua_pop(L, 1); /* route def table */
    }

    lua_pop(L, 1); /* __hull_route_defs table */
    return 0;
}

/* ── Usage ──────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options] <app.js|app.lua>\n"
            "\n"
            "Options:\n"
            "  -p PORT     Listen port (default: 3000)\n"
            "  -b ADDR     Bind address (default: 127.0.0.1)\n"
            "  -d FILE     SQLite database file (default: data.db)\n"
            "  -h          Show this help\n",
            prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int port = 3000;
    const char *bind_addr = "127.0.0.1";
    const char *db_path = "data.db";
    const char *entry_point = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            entry_point = argv[i];
        }
    }

    if (!entry_point) {
        /* Auto-detect: look for app.js, then app.lua */
        FILE *f = fopen("app.js", "r");
        if (f) {
            fclose(f);
            entry_point = "app.js";
        } else {
            f = fopen("app.lua", "r");
            if (f) {
                fclose(f);
                entry_point = "app.lua";
            }
        }
    }

    if (!entry_point) {
        fprintf(stderr, "hull: no entry point found (app.js or app.lua)\n");
        usage(argv[0]);
        return 1;
    }

    HullRuntimeType runtime = detect_runtime(entry_point);

    /* Open SQLite database */
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "hull: cannot open database %s: %s\n",
                db_path, sqlite3_errmsg(db));
        return 1;
    }

    /* Enable WAL mode for concurrent access */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

    /* Initialize Keel server */
    KlConfig config = {
        .port = port,
        .bind_addr = bind_addr,
        .max_connections = 64,
        .read_timeout_ms = 30000,
    };

    KlServer server;
    if (kl_server_init(&server, &config) != 0) {
        fprintf(stderr, "hull: server init failed\n");
        sqlite3_close(db);
        return 1;
    }

    if (runtime == HULL_RUNTIME_JS) {
        /* ── QuickJS runtime ──────────────────────────────────── */
        HullJSConfig js_cfg = HULL_JS_CONFIG_DEFAULT;
        HullJS js;

        js.db = db;

        if (hull_js_init(&js, &js_cfg) != 0) {
            fprintf(stderr, "hull: QuickJS init failed\n");
            sqlite3_close(db);
            return 1;
        }

        /* Load and evaluate the app */
        if (hull_js_load_app(&js, entry_point) != 0) {
            fprintf(stderr, "hull: failed to load %s\n", entry_point);
            hull_js_free(&js);
            sqlite3_close(db);
            return 1;
        }

        /* Wire JS routes into Keel */
        if (wire_js_routes(&js, &server) != 0) {
            hull_js_free(&js);
            sqlite3_close(db);
            return 1;
        }

        fprintf(stderr, "hull: listening on %s:%d (QuickJS runtime)\n",
                bind_addr, port);

        /* Enter event loop */
        kl_server_run(&server);

        /* Cleanup */
        hull_js_free(&js);
    } else {
        /* ── Lua runtime ─────────────────────────────────────── */
        HullLuaConfig lua_cfg = HULL_LUA_CONFIG_DEFAULT;
        HullLua lua;

        lua.db = db;

        if (hull_lua_init(&lua, &lua_cfg) != 0) {
            fprintf(stderr, "hull: Lua init failed\n");
            sqlite3_close(db);
            return 1;
        }

        /* Load and execute the app */
        if (hull_lua_load_app(&lua, entry_point) != 0) {
            fprintf(stderr, "hull: failed to load %s\n", entry_point);
            hull_lua_free(&lua);
            sqlite3_close(db);
            return 1;
        }

        /* Wire Lua routes into Keel */
        if (wire_lua_routes(&lua, &server) != 0) {
            hull_lua_free(&lua);
            sqlite3_close(db);
            return 1;
        }

        fprintf(stderr, "hull: listening on %s:%d (Lua runtime)\n",
                bind_addr, port);

        /* Enter event loop */
        kl_server_run(&server);

        /* Cleanup */
        hull_lua_free(&lua);
    }

    kl_server_free(&server);
    sqlite3_close(db);

    return 0;
}
