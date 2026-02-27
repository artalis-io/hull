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
#endif

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif

#include "hull/alloc.h"
#include "hull/commands/dispatch.h"
#include "hull/limits.h"
#include "hull/manifest.h"
#include "hull/parse_size.h"
#include "hull/sandbox.h"
#include "hull/signature.h"
#include "hull/tool.h"

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
            "  -p PORT              Listen port (default: 3000)\n"
            "  -b ADDR              Bind address (default: 127.0.0.1)\n"
            "  -d FILE              SQLite database file (default: data.db)\n"
            "  -m SIZE              Runtime heap limit (default: 64m)\n"
            "  -M SIZE              Process memory limit (default: unlimited)\n"
            "  -s SIZE              JS stack size limit (default: 1m)\n"
            "  -l LEVEL             Log level: trace|debug|info|warn|error|fatal (default: info)\n"
            "  --verify-sig PUBKEY  Verify app signature before startup\n"
            "  -h                   Show this help\n"
            "\n"
            "Subcommands:\n"
            "  keygen [prefix]      Generate Ed25519 keypair\n"
            "  build [options] dir  Build standalone binary\n"
            "  verify [dir]         Verify hull.sig signature\n"
            "  inspect [dir]        Display app capabilities\n"
            "  manifest [dir]       Extract app manifest\n"
            "  test [options] dir   Run app tests\n"
            "  new <name>           Scaffold new project\n"
            "  dev [app] [options]  Hot-reload development server\n"
            "  eject [dir] [-o out] Export standalone Makefile project\n"
            "\n"
            "SIZE accepts optional suffix: k (KB), m (MB), g (GB).\n",
            prog);
}

/* ── Server mode (default) ──────────────────────────────────────────── */

static int hull_serve(int argc, char **argv)
{
    int port = HL_DEFAULT_PORT;
    const char *bind_addr = "127.0.0.1";
    const char *db_path = "data.db";
    const char *entry_point = NULL;
    const char *verify_sig_path = NULL;
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
        } else if (strcmp(argv[i], "--verify-sig") == 0 && i + 1 < argc) {
            verify_sig_path = argv[++i];
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

    /* ── Runtime vtable dispatch ─────────────────────────────────── */

    union {
#ifdef HL_ENABLE_JS
        HlJS  js;
#endif
#ifdef HL_ENABLE_LUA
        HlLua lua;
#endif
    } rt_storage;
    memset(&rt_storage, 0, sizeof(rt_storage));

    const void *rt_cfg = NULL;
    HlRuntime *rt = NULL;

#ifdef HL_ENABLE_JS
    HlJSConfig js_cfg;
#endif
#ifdef HL_ENABLE_LUA
    HlLuaConfig lua_cfg;
#endif

    if (runtime == HL_RUNTIME_JS) {
#ifdef HL_ENABLE_JS
        js_cfg = (HlJSConfig)HL_JS_CONFIG_DEFAULT;
        if (heap_limit > 0)  js_cfg.max_heap_bytes  = (size_t)heap_limit;
        if (stack_limit > 0) js_cfg.max_stack_bytes  = (size_t)stack_limit;
        rt = &rt_storage.js.base;
        rt->vt = &hl_js_vtable;
        rt_cfg = &js_cfg;
#endif
    } else {
#ifdef HL_ENABLE_LUA
        lua_cfg = (HlLuaConfig)HL_LUA_CONFIG_DEFAULT;
        if (heap_limit > 0) lua_cfg.max_heap_bytes = (size_t)heap_limit;
        rt = &rt_storage.lua.base;
        rt->vt = &hl_lua_vtable;
        rt_cfg = &lua_cfg;
#endif
    }

    // cppcheck-suppress knownConditionTrueFalse
    if (!rt || !rt->vt) {
        log_error("[hull:c] no runtime available (compile with HL_ENABLE_LUA or HL_ENABLE_JS)");
        goto cleanup_server;
    }

    rt->db = db;
    rt->alloc = &alloc;

    if (rt->vt->init(rt, rt_cfg) != 0) {
        log_error("[hull:c] %s init failed", rt->vt->name);
        goto cleanup_server;
    }

    /* Load and evaluate the app */
    if (rt->vt->load_app(rt, entry_point) != 0) {
        log_error("[hull:c] failed to load %s", entry_point);
        rt->vt->destroy(rt);
        goto cleanup_server;
    }

    /* Verify app signature if requested */
    if (verify_sig_path) {
        if (hl_verify_startup(verify_sig_path, entry_point) != 0) {
            log_error("[hull:c] signature verification failed — refusing to start");
            rt->vt->destroy(rt);
            goto cleanup_server;
        }
        log_info("[hull:c] signature verified OK");
    }

    /* Wire routes into Keel */
    if (rt->vt->wire_routes_server(rt, &server, track_route_alloc) != 0) {
        rt->vt->destroy(rt);
        goto cleanup_server;
    }

    /* Extract manifest and configure capabilities */
    HlManifest manifest;
    if (rt->vt->extract_manifest(rt, &manifest) == 0) {
        log_info("[hull:c] manifest: fs_read=%d fs_write=%d env=%d hosts=%d",
                 manifest.fs_read_count, manifest.fs_write_count,
                 manifest.env_count, manifest.hosts_count);
    }

    /* Apply kernel sandbox (pledge/unveil) from manifest */
    if (hl_sandbox_apply(&manifest, db_path) != 0) {
        log_error("[hull:c] sandbox enforcement failed");
        rt->vt->free_manifest_strings(rt, &manifest);
        rt->vt->destroy(rt);
        goto cleanup_server;
    }
    rt->vt->free_manifest_strings(rt, &manifest);

    log_info("[hull:c] listening on %s:%d (%s runtime)",
             bind_addr, port, rt->vt->name);

    /* Enter event loop */
    kl_server_run(&server);

    /* Cleanup */
    rt->vt->destroy(rt);
    ret = 0;

cleanup_server:
    kl_server_free(&server);
cleanup_db:
    sqlite3_close(db);
    free_route_allocs(&alloc);

    log_debug("[hull:c] peak memory: %zu bytes", hl_alloc_peak(&alloc));

    return ret;
}

/* ── Entry point with subcommand dispatch ──────────────────────────── */

int hull_main(int argc, char **argv)
{
    int rc = hl_command_dispatch(argc, argv);
    if (rc != -1)
        return rc;

    return hull_serve(argc, argv);
}
