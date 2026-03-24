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
#include "hull/worker_db.h"
#include "hull/cap/audit.h"
#include "hull/cap/db.h"
#include "hull/cap/env.h"
#include "hull/cap/fs.h"
#include "hull/cap/http.h"
#include "hull/cap/smtp.h"

#include <keel/client_pool.h>
#include <keel/compress_miniz.h>
#include <keel/decompress_miniz.h>
#include "hull/migrate.h"
#include "hull/vfs.h"

#include <keel/tls_mbedtls.h>
#include "hull/commands/dispatch.h"
#include "hull/agent_api.h"
#include "hull/limits.h"
#include "hull/manifest.h"
#include "hull/parse_size.h"
#include "hull/sandbox.h"
#include "hull/signature.h"
#include "hull/static.h"
#include "hull/tool.h"
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"
#endif
#ifdef HL_ENABLE_GPU
#include "hull/cap/gpu.h"
#endif

#include <keel/cors.h>
#include <keel/keel.h>
#include <keel/thread_pool.h>

#include <sqlite3.h>

#include "log.h"

#include <sh_arena.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Default Content-Security-Policy ───────────────────────────────── */

#define HL_DEFAULT_CSP \
    "default-src 'none'; style-src 'self'; " \
    "img-src 'self'; form-action 'self'; frame-ancestors 'none'"

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
    /* Check embedded app entries first (hull build binaries) */
    extern const HlEntry hl_app_entries[];
    for (int i = 0; hl_app_entries[i].name; i++) {
#ifdef HL_ENABLE_JS
        if (strcmp(hl_app_entries[i].name, "./app.js") == 0)
            return "app.js";
#endif
#ifdef HL_ENABLE_LUA
        if (strcmp(hl_app_entries[i].name, "./app") == 0)
            return "app.lua";
#endif
    }

    /* Filesystem fallback (development mode) */
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
            "  --tls-cert PATH      TLS certificate file (PEM)\n"
            "  --tls-key PATH       TLS private key file (PEM)\n"
            "  --verify-sig PUBKEY  Verify app signature before startup\n"
            "  --drain-timeout MS   Graceful shutdown drain timeout (default: 5000)\n"
            "  --no-migrate         Skip auto-run migrations on startup\n"
            "  --skip-ca-bundle     Skip TLS certificate verification (dev mode)\n"
            "  --max-instructions N Set runtime instruction limit per request (default: 100m)\n"
            "  --audit              Enable capability audit logging (JSON to stderr)\n"
            "  --max-connections N  Max concurrent connections (default: 256)\n"
            "  --body-max-size SIZE Max request body size (default: 1m)\n"
            "  --read-timeout MS    Read timeout in milliseconds (default: 30000)\n"
            "  --workers N          Thread pool worker count (default: 4)\n"
            "  --queue-capacity N   Thread pool queue capacity (default: 64)\n"
            "  --no-compress        Disable response compression\n"
            "  --no-sandbox         Disable kernel sandbox (dev/debug only)\n"
            "\n"
            "WASM compute options:\n"
            "  --wasm-heap SIZE     WASM instance heap ceiling (default: 2m, max: ~4g)\n"
            "  --wasm-stack SIZE    WASM stack size ceiling (default: 64k, max: 8m)\n"
            "  --wasm-gas N         WASM instruction gas ceiling (default: 10m, max: 100b)\n"
            "  --wasm-max-input SIZE  Max compute input size (default: 1m, max: 256m)\n"
            "  --wasm-max-output SIZE Max compute output size (default: 1m, max: 256m)\n"
            "\n"
            "GPU compute options:\n"
            "  --gpu-device N       Default GPU device index (default: 0)\n"
            "\n"
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
            "  migrate [subcommand] Run/status/create SQL migrations\n"
            "  eject [dir] [-o out] Export standalone Makefile project\n"
            "\n"
            "SIZE accepts optional suffix: k (KB), m (MB), g (GB).\n",
            prog);
}

/* ── CA bundle auto-detection ───────────────────────────────────────── */

static const char *find_ca_bundle(void)
{
    static const char *paths[] = {
        "/etc/ssl/cert.pem",                    /* macOS, Alpine */
        "/etc/ssl/certs/ca-certificates.crt",   /* Debian/Ubuntu */
        "/etc/pki/tls/certs/ca-bundle.crt",     /* RHEL/CentOS */
        NULL,
    };
    for (const char **p = paths; *p; p++) {
        FILE *f = fopen(*p, "r");
        if (f) { fclose(f); return *p; }
    }
    return NULL;
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
    long instruction_limit = 0; /* 0 = use default */
    long wasm_heap = 0, wasm_stack = 0, wasm_max_input = 0, wasm_max_output = 0;
    long long wasm_gas = 0;
#ifdef HL_ENABLE_GPU
    int gpu_device = -1;  /* -1 = auto (default device 0) */
#endif
    int log_level = LOG_INFO;
    int no_migrate = 0;
    int no_sandbox = 0;
    int no_compress = 0;
    int skip_ca_bundle = 0;
    int agent_mode = 0;
    int agent_api_mode = 0;
    int drain_timeout = HL_DEFAULT_DRAIN_TIMEOUT_MS;
    int max_connections = 0;  /* 0 = use default */
    long body_max_size = 0;   /* 0 = use default */
    int read_timeout = 0;     /* 0 = use default */
    int num_workers = 0;      /* 0 = use default */
    int queue_capacity = 0;   /* 0 = use default */
    const char *tls_cert_path = NULL;
    const char *tls_key_path = NULL;

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
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            tls_cert_path = argv[++i];
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            tls_key_path = argv[++i];
        } else if (strcmp(argv[i], "--verify-sig") == 0 && i + 1 < argc) {
            verify_sig_path = argv[++i];
        } else if (strcmp(argv[i], "--no-migrate") == 0) {
            no_migrate = 1;
        } else if (strcmp(argv[i], "--no-sandbox") == 0) {
            no_sandbox = 1;
        } else if (strcmp(argv[i], "--no-compress") == 0) {
            no_compress = 1;
        } else if (strcmp(argv[i], "--skip-ca-bundle") == 0) {
            skip_ca_bundle = 1;
        } else if (strcmp(argv[i], "--agent") == 0) {
            agent_mode = 1;
        } else if (strcmp(argv[i], "--agent-api") == 0) {
            agent_api_mode = 1;
        } else if (strcmp(argv[i], "--audit") == 0) {
            hl_audit_enabled = 1;
        } else if (strcmp(argv[i], "--max-instructions") == 0 && i + 1 < argc) {
            char *end;
            instruction_limit = strtol(argv[++i], &end, 10);
            if (*end != '\0' || instruction_limit < 0) {
                fprintf(stderr, "hull: invalid instruction limit: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--max-connections") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 100000) {
                fprintf(stderr, "hull: invalid max-connections: %s\n", argv[i]);
                return 1;
            }
            max_connections = (int)v;
        } else if (strcmp(argv[i], "--body-max-size") == 0 && i + 1 < argc) {
            body_max_size = hl_parse_size(argv[++i]);
            if (body_max_size <= 0) {
                fprintf(stderr, "hull: invalid body-max-size: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--read-timeout") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > 600000) {
                fprintf(stderr, "hull: invalid read-timeout: %s\n", argv[i]);
                return 1;
            }
            read_timeout = (int)v;
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 128) {
                fprintf(stderr, "hull: invalid workers: %s\n", argv[i]);
                return 1;
            }
            num_workers = (int)v;
        } else if (strcmp(argv[i], "--queue-capacity") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 10000) {
                fprintf(stderr, "hull: invalid queue-capacity: %s\n", argv[i]);
                return 1;
            }
            queue_capacity = (int)v;
        } else if (strcmp(argv[i], "--drain-timeout") == 0 && i + 1 < argc) {
            char *end;
            long dt = strtol(argv[++i], &end, 10);
            if (*end != '\0' || dt < 0 || dt > 300000) {
                fprintf(stderr, "hull: invalid drain timeout: %s\n", argv[i]);
                return 1;
            }
            drain_timeout = (int)dt;
        } else if (strcmp(argv[i], "--wasm-heap") == 0 && i + 1 < argc) {
            wasm_heap = hl_parse_size(argv[++i]);
        } else if (strcmp(argv[i], "--wasm-stack") == 0 && i + 1 < argc) {
            wasm_stack = hl_parse_size(argv[++i]);
        } else if (strcmp(argv[i], "--wasm-gas") == 0 && i + 1 < argc) {
            char *end;
            wasm_gas = strtoll(argv[++i], &end, 10);
        } else if (strcmp(argv[i], "--wasm-max-input") == 0 && i + 1 < argc) {
            wasm_max_input = hl_parse_size(argv[++i]);
        } else if (strcmp(argv[i], "--wasm-max-output") == 0 && i + 1 < argc) {
            wasm_max_output = hl_parse_size(argv[++i]);
#ifdef HL_ENABLE_GPU
        } else if (strcmp(argv[i], "--gpu-device") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > 15) {
                fprintf(stderr, "hull: invalid gpu-device: %s\n", argv[i]);
                return 1;
            }
            gpu_device = (int)v;
#endif
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            entry_point = argv[i];
        }
    }

    /* Check HULL_AUDIT env var */
    {
        const char *audit_env = getenv("HULL_AUDIT");
        if (audit_env && strcmp(audit_env, "1") == 0)
            hl_audit_enabled = 1;
    }

    /* Check HULL_MAX_INSTRUCTIONS env var */
    if (instruction_limit == 0) {
        const char *il_env = getenv("HULL_MAX_INSTRUCTIONS");
        if (il_env) {
            char *end;
            long val = strtol(il_env, &end, 10);
            if (*end == '\0' && val >= 0)
                instruction_limit = val;
        }
    }

    if (!entry_point)
        entry_point = auto_detect_entry();

    if (!entry_point) {
        fprintf(stderr, "hull: no entry point found (app.js or app.lua)\n");
        usage(argv[0]);
        return 1;
    }

    /* Resolve entry point to absolute path.  This ensures app_dir (derived
     * below) is also absolute, so realpath() inside the sandbox doesn't need
     * to stat the CWD — which may be outside the sandbox's allowed paths. */
    char entry_abs[4096];
    if (realpath(entry_point, entry_abs) != NULL)
        entry_point = entry_abs;

    /* Derive app directory from entry point (needed for migrations + static files + sandbox) */
    char app_dir[4096];
    {
        const char *slash = strrchr(entry_point, '/');
        if (slash) {
            size_t len = (size_t)(slash - entry_point);
            if (len >= sizeof(app_dir)) {
                fprintf(stderr, "hull: entry point path too long (max %zu chars)\n",
                        sizeof(app_dir) - 1);
                return 1;
            }
            memcpy(app_dir, entry_point, len);
            app_dir[len] = '\0';
        } else {
            app_dir[0] = '.';
            app_dir[1] = '\0';
        }
    }

    /* Initialize VFS instances for sorted entry lookup */
    extern const HlEntry hl_app_entries[];
    extern const HlEntry hl_stdlib_entries[];
    HlVfs app_vfs, platform_vfs;
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);

    /* Validate TLS cert/key pair */
    if ((tls_cert_path != NULL) != (tls_key_path != NULL)) {
        fprintf(stderr, "hull: --tls-cert and --tls-key must be provided together\n");
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

    HlStmtCache stmt_cache;
    memset(&stmt_cache, 0, sizeof(stmt_cache));

    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        log_error("[hull:c] cannot open database %s: %s",
                  db_path, sqlite3_errmsg(db));
        goto cleanup_db;
    }

    /* Apply performance PRAGMAs (WAL, synchronous=NORMAL, mmap, etc.) */
    if (hl_cap_db_init(db) != 0) {
        log_error("[hull:c] database PRAGMA initialization failed");
        goto cleanup_db;
    }

    /* Auto-run SQL migrations */
    if (!no_migrate) {
        int migrated = hl_migrate_run(db, &app_vfs);
        if (migrated == HL_MIGRATE_ERR) {
            log_error("[hull:c] migration failed — refusing to start");
            goto cleanup_db;
        }
        if (migrated > 0)
            log_info("[hull:c] applied %d migration(s)", migrated);
    }

    /* Initialize prepared statement cache */
    hl_stmt_cache_init(&stmt_cache, db, &alloc);

    /* Initialize Keel server */
    KlConfig config = {
        .port = port,
        .bind_addr = bind_addr,
        .max_connections = max_connections > 0 ? max_connections : HL_DEFAULT_MAX_CONN,
        .read_timeout_ms = read_timeout > 0 ? read_timeout : HL_DEFAULT_READ_TIMEOUT_MS,
        .max_body_size = body_max_size > 0 ? (size_t)body_max_size : HL_BODY_MAX_SIZE,
        .install_signal_handlers = 1,
        .drain_timeout_ms = drain_timeout,
        .alloc = &kl_alloc,
        .log_fn = hl_keel_log_bridge,
        .log_user_data = NULL,
    };

    /* Set up server TLS if cert/key provided */
    KlTlsConfig server_tls_config = {0};
    KlTlsCtx *server_tls_ctx = NULL;

    if (tls_cert_path && tls_key_path) {
        server_tls_ctx = kl_tls_mbedtls_ctx_create(
            tls_cert_path, tls_key_path, NULL, KL_MTLS_NONE, &kl_alloc);
        if (!server_tls_ctx) {
            log_error("[hull:c] failed to create server TLS context "
                      "(cert=%s, key=%s)", tls_cert_path, tls_key_path);
            goto cleanup_db;
        }
        server_tls_config.ctx         = server_tls_ctx;
        server_tls_config.factory     = (KlTlsFactory)kl_tls_mbedtls_create;
        server_tls_config.ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy;
        config.tls = &server_tls_config;
    }

    KlServer server;
    if (kl_server_init(&server, &config) != 0) {
        log_error("[hull:c] server init failed: %s",
                  kl_strerror(server.last_error));
        if (server_tls_ctx)
            kl_tls_mbedtls_ctx_destroy(server_tls_ctx);
        goto cleanup_db;
    }

    /* Create thread pool for async work (db queries, file I/O) */
    KlThreadPoolConfig tp_cfg = {
        .num_workers    = num_workers > 0 ? num_workers : HL_THREAD_POOL_WORKERS,
        .queue_capacity = queue_capacity > 0 ? queue_capacity : HL_THREAD_POOL_CAPACITY,
        .alloc          = &kl_alloc,
    };
    KlThreadPool *thread_pool = kl_thread_pool_create(&server.ev, &tp_cfg);
    if (!thread_pool)
        log_warn("[hull:c] thread pool creation failed: %s — async work unavailable",
                 kl_strerror(server.ev.last_error));
    /* thread_pool may be NULL if creation fails — non-fatal, async work
     * will simply be unavailable */

    /* Create client connection pool for HTTP keep-alive reuse */
    KlClientPool client_pool;
    int cpool_ok = kl_cpool_init(&client_pool, NULL, &kl_alloc, &server.ev);
    if (cpool_ok != 0)
        log_warn("[hull:c] client pool creation failed — connection reuse disabled");

    /* Create compression context for server responses and client decompression */
    KlCompressCtx *comp_ctx = NULL;
    KlCompressConfig compress_cfg = {0};
    KlDecompressConfig decompress_cfg = {0};

    if (!no_compress) {
        comp_ctx = kl_compress_miniz_ctx_create(6, &kl_alloc);
        if (comp_ctx) {
            compress_cfg.ctx = comp_ctx;
            compress_cfg.factory = (KlCompressFactory)kl_compress_miniz_create;
            compress_cfg.ctx_destroy =
                (void (*)(KlCompressCtx *))kl_compress_miniz_ctx_destroy;
            config.compress = &compress_cfg;

            decompress_cfg.ctx = comp_ctx;
            decompress_cfg.factory =
                (KlDecompressFactory)kl_decompress_miniz_create;
        } else {
            log_warn("[hull:c] compression init failed — responses uncompressed");
        }
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
        if (heap_limit > 0)        js_cfg.max_heap_bytes   = (size_t)heap_limit;
        if (stack_limit > 0)       js_cfg.max_stack_bytes   = (size_t)stack_limit;
        if (instruction_limit > 0) js_cfg.max_instructions  = instruction_limit;
        rt = &rt_storage.js.base;
        rt->vt = &hl_js_vtable;
        rt_cfg = &js_cfg;
#endif
    } else {
#ifdef HL_ENABLE_LUA
        lua_cfg = (HlLuaConfig)HL_LUA_CONFIG_DEFAULT;
        if (heap_limit > 0)        lua_cfg.max_heap_bytes   = (size_t)heap_limit;
        if (instruction_limit > 0) lua_cfg.max_instructions  = instruction_limit;
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
    rt->stmt_cache = &stmt_cache;
    rt->alloc = &alloc;
    rt->thread_pool = thread_pool;
    rt->app_vfs = &app_vfs;
    rt->platform_vfs = &platform_vfs;
    rt->db_path = db_path;
    if (comp_ctx)
        rt->compress = &compress_cfg;

#ifdef HL_ENABLE_WASM
    /* Initialize WAMR compute runtime and wire to rt immediately so
     * module registration in init() can see it.  If the manifest later
     * declares compute: false, we NULL it out before route wiring. */
    static HlWasmCache wasm_cache;
    static int wasm_cache_ok = 0;
    if (hl_cap_wasm_init(&wasm_cache) == 0) {
        wasm_cache_ok = 1;
        rt->wasm_cache = &wasm_cache;
    } else {
        log_warn("[hull:c] WAMR init failed — compute.call() unavailable");
    }
#endif

#ifdef HL_ENABLE_GPU
    /* Initialize GPU compute runtime and wire to rt immediately so
     * module registration in init() can see it.  If the manifest later
     * declares gpu: false, we NULL it out before route wiring. */
    static HlGpuCtx gpu_ctx;
    static int gpu_ctx_ok = 0;
    if (hl_cap_gpu_init(&gpu_ctx, &hl_gpu_backend_wgpu) == HL_GPU_OK
        && hl_cap_gpu_available(&gpu_ctx)) {
        if (gpu_device >= 0 && gpu_device < gpu_ctx.device_count)
            gpu_ctx.default_device = gpu_device;
        gpu_ctx_ok = 1;
        rt->gpu_ctx = &gpu_ctx;
    } else {
        log_info("[hull:c] GPU compute unavailable — gpu.* disabled");
    }
#endif

    /* Initialize worker DB capability (per-worker SQLite connections) */
    if (db_path)
        hl_worker_db_init(db_path);

    if (rt->vt->init(rt, rt_cfg) != 0) {
        log_error("[hull:c] %s init failed", rt->vt->name);
        goto cleanup_server;
    }

    /* RT-01: Verify app signature BEFORE loading — malicious code never
     * executes if verification fails. */
    if (verify_sig_path) {
        if (hl_verify_startup(verify_sig_path, entry_point, &app_vfs) != 0) {
            log_error("[hull:c] signature verification failed — refusing to start");
            rt->vt->destroy(rt);
            goto cleanup_server;
        }
        log_info("[hull:c] signature verified OK");
    }

    /* Phase 1 sandbox: block exec/proc/fork before loading user code */
    if (!no_sandbox) {
        if (hl_sandbox_apply_pledge() != 0) {
            log_error("[hull:c] failed to apply phase 1 sandbox");
            rt->vt->destroy(rt);
            goto cleanup_server;
        }
    }

    /* Load and evaluate the app (runs under phase 1 pledge) */
    if (rt->vt->load_app(rt, entry_point) != 0) {
        log_error("[hull:c] failed to load %s", entry_point);
        if (agent_mode) {
            char err_dir[4096], err_path[4096];
            snprintf(err_dir, sizeof(err_dir), "%s/.hull", app_dir);
            mkdir(err_dir, 0755);
            snprintf(err_path, sizeof(err_path), "%s/.hull/last_error.json", app_dir);
            FILE *ef = fopen(err_path, "w");
            if (ef) {
                fprintf(ef, "{\"error\":\"failed to load %s\",\"timestamp\":%ld}\n",
                        entry_point, (long)time(NULL));
                fclose(ef);
            }
        }
        rt->vt->destroy(rt);
        goto cleanup_server;
    }

    /* Clear previous error file on successful load */
    if (agent_mode) {
        char err_path[4096];
        snprintf(err_path, sizeof(err_path), "%s/.hull/last_error.json", app_dir);
        unlink(err_path);
    }

    /* Extract manifest and configure capabilities */
    HlManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    if (rt->vt->extract_manifest(rt, &manifest) == 0) {
        log_info("[hull:c] manifest: fs_read=%d fs_write=%d env=%d hosts=%d",
                 manifest.fs_read_count, manifest.fs_write_count,
                 manifest.env_count, manifest.hosts_count);
    }

    /* Wire CSP policy to runtime.
     * Default CSP is always active — even without app.manifest().
     * Explicit csp="custom" overrides; csp=false disables. */
    if (manifest.csp_set)
        rt->csp_policy = manifest.csp;    /* custom or NULL (disabled) */
    else
        rt->csp_policy = HL_DEFAULT_CSP;  /* default */

#ifdef HL_ENABLE_WASM
    /* Revoke WASM if manifest is present but doesn't declare compute: true.
     * Already wired above; only revoke when manifest explicitly omits it. */
    if (wasm_cache_ok && manifest.present && !manifest.compute) {
        rt->wasm_cache = NULL;
        log_info("[hull:c] compute not declared in manifest — compute.* disabled");
    }

    /* Resolve three-tier WASM config: CLI > manifest > compile-time defaults.
     * Zero = not set (fall through to compile-time default at call time). */
    {
        uint32_t wh = manifest.wasm_heap;
        uint32_t ws = manifest.wasm_stack;
        int64_t  wg = manifest.wasm_gas;
        uint32_t wi = manifest.wasm_max_input;
        uint32_t wo = manifest.wasm_max_output;

        /* CLI overrides manifest (operator > developer) */
        if (wasm_heap > 0)       wh = (uint32_t)wasm_heap;
        if (wasm_stack > 0)      ws = (uint32_t)wasm_stack;
        if (wasm_gas > 0)        wg = (int64_t)wasm_gas;
        if (wasm_max_input > 0)  wi = (uint32_t)wasm_max_input;
        if (wasm_max_output > 0) wo = (uint32_t)wasm_max_output;

        /* Clamp to compile-time maximums */
        if (wh > (uint32_t)HL_WASM_MAX_HEAP)  wh = (uint32_t)HL_WASM_MAX_HEAP;
        if (ws > (uint32_t)HL_WASM_MAX_STACK) ws = (uint32_t)HL_WASM_MAX_STACK;
        if (wg > HL_WASM_MAX_GAS)             wg = HL_WASM_MAX_GAS;
        if (wi > (uint32_t)HL_WASM_MAX_IO_SIZE) wi = (uint32_t)HL_WASM_MAX_IO_SIZE;
        if (wo > (uint32_t)HL_WASM_MAX_IO_SIZE) wo = (uint32_t)HL_WASM_MAX_IO_SIZE;

        rt->wasm_config.heap_size  = wh;
        rt->wasm_config.stack_size = ws;
        rt->wasm_config.gas        = wg;
        rt->wasm_config.max_input  = wi;
        rt->wasm_config.max_output = wo;
    }
#endif

#ifdef HL_ENABLE_GPU
    /* Revoke GPU if manifest is present but doesn't declare gpu: true.
     * Already wired above; only revoke when manifest explicitly omits it. */
    if (gpu_ctx_ok && manifest.present && !manifest.gpu) {
        rt->gpu_ctx = NULL;
        log_info("[hull:c] gpu not declared in manifest — gpu.* disabled");
    }
#endif

    /* Wire fs_cfg from manifest (if app declares fs.read paths) */
    HlFsConfig fs_cfg_storage = {0};
    if (manifest.fs_read_count > 0) {
        fs_cfg_storage.base_dir = app_dir;
        fs_cfg_storage.base_len = strlen(app_dir);
        rt->fs_cfg = &fs_cfg_storage;
    }

    /* Wire env_cfg from manifest (if app declares env vars) */
    HlEnvConfig env_cfg_storage = {0};
    if (manifest.env_count > 0) {
        env_cfg_storage.allowed = manifest.env;
        env_cfg_storage.count   = manifest.env_count;
        rt->env_cfg = &env_cfg_storage;
    }

    /* Wire http_cfg from manifest (if app declares hosts) */
    HlHttpConfig http_cfg_storage = {0};
    KlTlsConfig client_tls_config = {0};
    KlTlsCtx *client_tls_ctx = NULL;
    const char *ca_bundle_path = NULL;

    if (manifest.hosts_count > 0) {
        http_cfg_storage.allowed_hosts     = manifest.hosts;
        http_cfg_storage.count             = manifest.hosts_count;
        http_cfg_storage.timeout_ms        = KL_CLIENT_DEFAULT_TIMEOUT_MS;
        http_cfg_storage.max_response_size = KL_CLIENT_DEFAULT_MAX_RESP;

        /* Set up TLS client for HTTPS support */
        if (skip_ca_bundle) {
            log_warn("[hull:c] TLS certificate verification disabled (--skip-ca-bundle)");
            client_tls_ctx = kl_tls_mbedtls_client_ctx_create(NULL, &kl_alloc);
        } else {
            ca_bundle_path = find_ca_bundle();
            if (ca_bundle_path) {
                log_info("[hull:c] using CA bundle: %s", ca_bundle_path);
                client_tls_ctx = kl_tls_mbedtls_client_ctx_create(ca_bundle_path, &kl_alloc);
            } else {
                log_warn("[hull:c] no CA bundle found; HTTPS disabled "
                         "(use --skip-ca-bundle for dev mode)");
            }
        }

        if (client_tls_ctx) {
            client_tls_config.ctx         = client_tls_ctx;
            client_tls_config.factory     = (KlTlsFactory)kl_tls_mbedtls_create;
            client_tls_config.ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy;
            http_cfg_storage.tls          = &client_tls_config;
        }

        /* Enable connection pooling and redirect following */
        if (cpool_ok == 0)
            http_cfg_storage.pool = &client_pool;
        http_cfg_storage.follow_redirects = 1;

        /* Enable client-side response decompression */
        if (comp_ctx)
            http_cfg_storage.decompress = &decompress_cfg;

        rt->http_cfg = &http_cfg_storage;
    }

    /* Wire smtp_cfg — shares same host allowlist and TLS context as HTTP */
    HlSmtpConfig smtp_cfg_storage = {0};
    if (manifest.hosts_count > 0) {
        smtp_cfg_storage.allowed_hosts = manifest.hosts;
        smtp_cfg_storage.host_count    = manifest.hosts_count;
        smtp_cfg_storage.timeout_ms    = HL_SMTP_DEFAULT_TIMEOUT_MS;
        smtp_cfg_storage.tls           = client_tls_ctx ? &client_tls_config : NULL;
        rt->smtp_cfg = &smtp_cfg_storage;
    }

    /* RT-04: Apply kernel sandbox BEFORE wiring routes — all route
     * handlers execute inside sandbox constraints. */
    if (!no_sandbox) {
        if (hl_sandbox_apply(&manifest, app_dir, db_path, ca_bundle_path,
                              tls_cert_path, tls_key_path) != 0) {
            log_error("[hull:c] sandbox enforcement failed");
            rt->vt->free_manifest_strings(rt, &manifest);
            rt->vt->destroy(rt);
            if (client_tls_ctx)
                kl_tls_mbedtls_ctx_destroy(client_tls_ctx);
            goto cleanup_server;
        }
    } else {
        log_warn("[hull:c] kernel sandbox disabled (--no-sandbox)");
    }

    /* Register CORS middleware from manifest (before routes) */
    KlCorsConfig cors_cfg;
    if (manifest.cors_set) {
        kl_cors_init(&cors_cfg);
        for (int i = 0; i < manifest.cors_origin_count; i++)
            kl_cors_add_origin(&cors_cfg, manifest.cors_origins[i]);
        if (manifest.cors_methods)
            cors_cfg.allowed_methods = manifest.cors_methods;
        if (manifest.cors_headers)
            cors_cfg.allowed_headers = manifest.cors_headers;
        cors_cfg.allow_credentials = manifest.cors_credentials;
        if (manifest.cors_max_age > 0)
            cors_cfg.max_age_seconds = manifest.cors_max_age;
        kl_server_use(&server, "*", "/*", kl_cors_middleware, &cors_cfg);
        log_info("[hull:c] CORS enabled (%d origin(s))",
                 manifest.cors_origin_count);
    }

    /* Wire routes into Keel (after sandbox is applied) */
    if (rt->vt->wire_routes_server(rt, &server, track_route_alloc) != 0) {
        rt->vt->free_manifest_strings(rt, &manifest);
        rt->vt->destroy(rt);
        if (client_tls_ctx)
            kl_tls_mbedtls_ctx_destroy(client_tls_ctx);
        goto cleanup_server;
    }

    /* Auto-register static file serving (after sandbox is applied) */
    {
        int has_static = hl_vfs_has_prefix(&app_vfs, "static/");
        if (!has_static) {
            char static_dir[4096];
            snprintf(static_dir, sizeof(static_dir), "%s/static", app_dir);
            struct stat sdir;
            if (stat(static_dir, &sdir) == 0 && S_ISDIR(sdir.st_mode))
                has_static = 1;
        }
        if (has_static) {
            HlStaticCtx *sctx = track_route_alloc(sizeof(HlStaticCtx));
            sctx->vfs = &app_vfs;
            kl_server_use(&server, "GET", "/static/*",
                          hl_static_middleware, sctx);
        }
    }

    /* Register agent diagnostic endpoints (opt-in via --agent-api) */
    HlAgentApiCtx agent_api_ctx = { .app_dir = app_dir, .db_path = db_path };
    if (agent_api_mode)
        hl_agent_api_register(&server, &agent_api_ctx);

    log_info("[hull:c] listening on %s://%s:%d (%s runtime)",
             server_tls_ctx ? "https" : "http",
             bind_addr, port, rt->vt->name);

    /* Enter event loop */
    if (kl_server_run(&server) < 0)
        log_error("[hull:c] server exited with error: %s",
                  kl_strerror(server.last_error));

    log_info("[hull:c] server stopped");

    /* Free thread pool BEFORE server — join workers, drain queues while
     * server infrastructure (connections, event loop) is still valid */
    if (thread_pool)
        kl_thread_pool_free(thread_pool);

    /* Free client connection pool (closes idle connections) */
    if (cpool_ok == 0)
        kl_cpool_free(&client_pool);

    /* Free compression context (shared by compress + decompress) */
    if (comp_ctx)
        kl_compress_miniz_ctx_destroy(comp_ctx);

    /* Cleanup — free manifest strings AFTER server stops
     * (env_cfg and http_cfg reference them during runtime) */
    rt->vt->free_manifest_strings(rt, &manifest);
    rt->vt->destroy(rt);

    /* Destroy WASM cache AFTER runtime destroy — GC finalizers
     * (WasmBuffer WASM-kind and WasmInstance) need WAMR alive */
#ifdef HL_ENABLE_WASM
    if (wasm_cache_ok)
        hl_cap_wasm_destroy(&wasm_cache);
#endif
#ifdef HL_ENABLE_GPU
    if (gpu_ctx_ok)
        hl_cap_gpu_destroy(&gpu_ctx);
#endif
    if (client_tls_ctx)
        kl_tls_mbedtls_ctx_destroy(client_tls_ctx);
    if (server_tls_ctx)
        kl_tls_mbedtls_ctx_destroy(server_tls_ctx);
    ret = 0;

cleanup_server:
    kl_server_free(&server);
cleanup_db:
    hl_stmt_cache_destroy(&stmt_cache);
    hl_cap_db_shutdown(db);
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
