/*
 * main.c — Hull application entry point
 *
 * Detects runtime from entry point file extension (.lua → Lua, .js → QuickJS).
 * Initializes the selected runtime, opens SQLite database, registers routes
 * with Keel, and enters the event loop.
 *
 * Compile-time runtime selection:
 *   -DHL_ENABLE_JS   — include QuickJS runtime
 *   -DHL_ENABLE_LUA  — include Lua runtime
 *   Both may be defined simultaneously (default).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#include "quickjs.h"
#endif

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "lua.h"
#include "lauxlib.h"
#endif

#include "hull/alloc.h"
#include "hull/cap/body.h"
#include "hull/limits.h"
#include "hull/parse_size.h"

#include <keel/keel.h>

#include <sqlite3.h>

#include "log.h"

#include <sh_arena.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── Logging ───────────────────────────────────────────────────────── */

/* Custom log callback: suppresses file:line in release builds */
static void hl_log_callback(log_Event *ev) {
    char ts[16];
    ts[strftime(ts, sizeof(ts), "%H:%M:%S", ev->time)] = '\0';

    char msg[1024];
    vsnprintf(msg, sizeof(msg), ev->fmt, ev->ap);

#ifdef DEBUG
    fprintf((FILE *)ev->udata, "%s %-5s %s:%d: %s\n",
            ts, log_level_string(ev->level), ev->file, ev->line, msg);
#else
    fprintf((FILE *)ev->udata, "%s %-5s %s\n",
            ts, log_level_string(ev->level), msg);
#endif
}

static int hl_parse_log_level(const char *s) {
    if (strcmp(s, "trace") == 0) return LOG_TRACE;
    if (strcmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcmp(s, "info")  == 0) return LOG_INFO;
    if (strcmp(s, "warn")  == 0) return LOG_WARN;
    if (strcmp(s, "error") == 0) return LOG_ERROR;
    if (strcmp(s, "fatal") == 0) return LOG_FATAL;
    return -1;
}

/* Bridge: routes Keel KlLogFn through rxi/log.c with [keel] prefix */
static void hl_keel_log_bridge(int level, const char *fmt, va_list ap,
                                void *user_data) {
    (void)user_data;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    log_log(level, "keel", 0, "[keel] %s", buf);
}

/* ── Route allocation arena (freed on shutdown) ────────────────────── */

static SHArena *route_arena;

static void *track_route_alloc(size_t size)
{
    return sh_arena_calloc(route_arena, 1, size);
}

static void free_route_allocs(HlAllocator *a)
{
    hl_arena_free(a, route_arena);
    route_arena = NULL;
}

/* ── Runtime selection ──────────────────────────────────────────────── */

typedef enum {
    HL_RUNTIME_LUA = 0,
    HL_RUNTIME_JS  = 1,
} HlRuntimeType;

static HlRuntimeType detect_runtime(const char *entry_point)
{
    const char *ext = strrchr(entry_point, '.');
    if (ext && strcmp(ext, ".js") == 0)
        return HL_RUNTIME_JS;
    return HL_RUNTIME_LUA; /* default */
}

/* ── JS handler bridge ──────────────────────────────────────────────── */

#ifdef HL_ENABLE_JS

typedef struct {
    HlJS *js;
    int     handler_id;
} HlJSRoute;

static void hl_js_keel_handler(KlRequest *req, KlResponse *res,
                                  void *user_data)
{
    HlJSRoute *route = (HlJSRoute *)user_data;
    if (hl_js_dispatch(route->js, route->handler_id, req, res) != 0) {
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body(res, "Internal Server Error", 21);
    }
}

/* ── Wire JS routes into Keel ───────────────────────────────────────── */

static int wire_js_routes(HlJS *js, KlServer *server)
{
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");

    if (JS_IsUndefined(defs) || !JS_IsArray(ctx, defs)) {
        JS_FreeValue(ctx, defs);
        JS_FreeValue(ctx, global);
        log_error("[hull:c] no routes registered");
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
            HlJSRoute *route = track_route_alloc(sizeof(HlJSRoute));
            if (route) {
                route->js = js;
                route->handler_id = handler_id;
                kl_server_route(server, method_str, pattern,
                                hl_js_keel_handler, route,
                                hl_cap_body_factory);
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

#endif /* HL_ENABLE_JS */

/* ── Lua handler bridge ─────────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

typedef struct {
    HlLua *lua;
    int      handler_id;
} HlLuaRoute;

static void hl_lua_keel_handler(KlRequest *req, KlResponse *res,
                                   void *user_data)
{
    HlLuaRoute *route = (HlLuaRoute *)user_data;
    if (hl_lua_dispatch(route->lua, route->handler_id, req, res) != 0) {
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body(res, "Internal Server Error", 21);
    }
}

/* ── Wire Lua routes into Keel ─────────────────────────────────────── */

static int wire_lua_routes(HlLua *lua, KlServer *server)
{
    lua_State *L = lua->L;

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    int count = (int)luaL_len(L, -1);
    if (count <= 0) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
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
            HlLuaRoute *route = track_route_alloc(sizeof(HlLuaRoute));
            if (route) {
                route->lua = lua;
                route->handler_id = handler_id;
                kl_server_route(server, method_str, pattern,
                                hl_lua_keel_handler, route,
                                hl_cap_body_factory);
            }
        }

        lua_pop(L, 3); /* method_str, pattern, handler_id */
        lua_pop(L, 1); /* route def table */
    }

    lua_pop(L, 1); /* __hull_route_defs table */
    return 0;
}

#endif /* HL_ENABLE_LUA */

/* ── Auto-detect entry point ───────────────────────────────────────── */

static const char *auto_detect_entry(void)
{
#ifdef HL_ENABLE_JS
    FILE *f = fopen("app.js", "r");
    if (f) { fclose(f); return "app.js"; }
#endif
#ifdef HL_ENABLE_LUA
    FILE *f2 = fopen("app.lua", "r");
    if (f2) { fclose(f2); return "app.lua"; }
#endif
    return NULL;
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
            "  -m SIZE     Runtime heap limit (default: 64m)\n"
            "  -M SIZE     Process memory limit (default: unlimited)\n"
            "  -s SIZE     JS stack size limit (default: 1m)\n"
            "  -l LEVEL    Log level: trace|debug|info|warn|error|fatal (default: info)\n"
            "  -h          Show this help\n"
            "\n"
            "SIZE accepts optional suffix: k (KB), m (MB), g (GB).\n",
            prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int port = HL_DEFAULT_PORT;
    const char *bind_addr = "127.0.0.1";
    const char *db_path = "data.db";
    const char *entry_point = NULL;
    long heap_limit = 0;    /* 0 = use default */
    long stack_limit = 0;   /* 0 = use default */
    long mem_limit = 0;     /* 0 = unlimited */
    int log_level = LOG_INFO;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char *end;
            long p = strtol(argv[++i], &end, 10);
            if (*end != '\0' || p < 1 || p > 65535) {
                fprintf(stderr, "hull: invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)p;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            heap_limit = hl_parse_size(argv[++i]);
            if (heap_limit <= 0) {
                fprintf(stderr, "hull: invalid heap size: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
            mem_limit = hl_parse_size(argv[++i]);
            if (mem_limit <= 0) {
                fprintf(stderr, "hull: invalid memory limit: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            stack_limit = hl_parse_size(argv[++i]);
            if (stack_limit <= 0) {
                fprintf(stderr, "hull: invalid stack size: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            log_level = hl_parse_log_level(argv[++i]);
            if (log_level < 0) {
                fprintf(stderr, "hull: invalid log level: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            entry_point = argv[i];
        }
    }

    if (!entry_point)
        entry_point = auto_detect_entry();

    if (!entry_point) {
        fprintf(stderr, "hull: no entry point found (app.js or app.lua)\n");
        usage(argv[0]);
        return 1;
    }

    HlRuntimeType runtime = detect_runtime(entry_point);

    /* Validate that the requested runtime is compiled in */
#ifndef HL_ENABLE_JS
    if (runtime == HL_RUNTIME_JS) {
        fprintf(stderr, "hull: QuickJS runtime not enabled in this build\n");
        return 1;
    }
#endif
#ifndef HL_ENABLE_LUA
    if (runtime == HL_RUNTIME_LUA) {
        fprintf(stderr, "hull: Lua runtime not enabled in this build\n");
        return 1;
    }
#endif

    /* Initialize logging */
    log_set_level(log_level);
    log_set_quiet(true);  /* suppress default stderr callback */
    log_add_callback(hl_log_callback, stderr, log_level);

    /* Initialize tracking allocator */
    HlAllocator alloc;
    hl_alloc_init(&alloc, (size_t)mem_limit);
    KlAllocator kl_alloc = hl_alloc_kl(&alloc);

    int ret = 1;

    /* Create route allocation arena (256 routes x 64 bytes = 16KB) */
    route_arena = hl_arena_create(&alloc, HL_MAX_ROUTES * 64);
    if (!route_arena) {
        log_error("[hull:c] route arena allocation failed");
        return 1;
    }

    /* Open SQLite database */
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        log_error("[hull:c] cannot open database %s: %s",
                  db_path, sqlite3_errmsg(db));
        goto cleanup_db;
    }

    /* Enable WAL mode for concurrent access */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

    /* Initialize Keel server */
    KlConfig config = {
        .port = port,
        .bind_addr = bind_addr,
        .max_connections = HL_DEFAULT_MAX_CONN,
        .read_timeout_ms = HL_DEFAULT_READ_TIMEOUT_MS,
        .alloc = &kl_alloc,
        .log_fn = hl_keel_log_bridge,
        .log_user_data = NULL,
    };

    KlServer server;
    if (kl_server_init(&server, &config) != 0) {
        log_error("[hull:c] server init failed");
        goto cleanup_db;
    }

#ifdef HL_ENABLE_JS
    if (runtime == HL_RUNTIME_JS) {
        /* ── QuickJS runtime ──────────────────────────────────── */
        HlJSConfig js_cfg = HL_JS_CONFIG_DEFAULT;
        if (heap_limit > 0)  js_cfg.max_heap_bytes  = (size_t)heap_limit;
        if (stack_limit > 0) js_cfg.max_stack_bytes  = (size_t)stack_limit;
        HlJS js;

        js.db = db;
        js.alloc = &alloc;

        if (hl_js_init(&js, &js_cfg) != 0) {
            log_error("[hull:c] QuickJS init failed");
            goto cleanup_server;
        }

        /* Load and evaluate the app */
        if (hl_js_load_app(&js, entry_point) != 0) {
            log_error("[hull:c] failed to load %s", entry_point);
            hl_js_free(&js);
            goto cleanup_server;
        }

        /* Wire JS routes into Keel */
        if (wire_js_routes(&js, &server) != 0) {
            hl_js_free(&js);
            goto cleanup_server;
        }

        log_info("[hull:c] listening on %s:%d (QuickJS runtime)",
                 bind_addr, port);

        /* Enter event loop */
        kl_server_run(&server);

        /* Cleanup */
        hl_js_free(&js);
        ret = 0;
    }
#endif

#ifdef HL_ENABLE_LUA
    if (runtime == HL_RUNTIME_LUA) {
        /* ── Lua runtime ─────────────────────────────────────── */
        HlLuaConfig lua_cfg = HL_LUA_CONFIG_DEFAULT;
        if (heap_limit > 0) lua_cfg.max_heap_bytes = (size_t)heap_limit;
        HlLua lua;

        lua.db = db;
        lua.alloc = &alloc;

        if (hl_lua_init(&lua, &lua_cfg) != 0) {
            log_error("[hull:c] Lua init failed");
            goto cleanup_server;
        }

        /* Load and execute the app */
        if (hl_lua_load_app(&lua, entry_point) != 0) {
            log_error("[hull:c] failed to load %s", entry_point);
            hl_lua_free(&lua);
            goto cleanup_server;
        }

        /* Wire Lua routes into Keel */
        if (wire_lua_routes(&lua, &server) != 0) {
            hl_lua_free(&lua);
            goto cleanup_server;
        }

        log_info("[hull:c] listening on %s:%d (Lua runtime)",
                 bind_addr, port);

        /* Enter event loop */
        kl_server_run(&server);

        /* Cleanup */
        hl_lua_free(&lua);
        ret = 0;
    }
#endif

cleanup_server:
    kl_server_free(&server);
cleanup_db:
    sqlite3_close(db);
    free_route_allocs(&alloc);

    log_debug("[hull:c] peak memory: %zu bytes", hl_alloc_peak(&alloc));

    return ret;
}
