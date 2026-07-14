/*
 * serve.c — Hull's full app lifecycle: load → manifest → sandbox →
 *           module resolve → migrations → server-mode dispatch (Keel
 *           event loop) or CLI-mode dispatch (app.main).
 *
 * Split out of main.c so future HL_ENABLE_HTTP=0 builds can swap in a
 * Keel-free counterpart while leaving the subcommand dispatcher in
 * main.c untouched.
 *
 * Detects runtime from entry-point file extension (.lua → Lua,
 * .js → QuickJS). The actual server/CLI branch in
 * hl_serve_wire_and_start consults rt->vt->has_main.
 *
 * Compile-time selection:
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

#include "hull/utils/alloc.h"
#include "hull/app_context.h"
#ifdef HL_ENABLE_DB
#include "hull/cap/db_registry.h"
#endif
#include "hull/shared/async_backend.h"
#include "hull/shared/log_lock.h"
#include "hull/shared/thread_affinity.h"
#include "hull/async/keel.h"
#include "hull/net_backend.h"
#include "hull/net/keel.h"
#include "hull/cacert.h"
#include "hull/utils/csp.h"
#include <sh_seal_arena.h>
#ifdef HL_ENABLE_DB
#include "hull/worker_db.h"
#include "hull/cap/db.h"
#include "hull/migrate.h"
#endif
#include "hull/cap/audit.h"
#include "hull/cap/env.h"
#include "hull/cap/fs.h"
#include "sh_json.h"
#include "hull/cap/http.h"
#include "hull/cap/smtp.h"

#include <keel/client_pool.h>
#include <keel/compress_miniz.h>
#include <keel/decompress_miniz.h>
#include "hull/vfs.h"

#include <keel/tls_mbedtls.h>
#include "hull/commands/dispatch.h"
#include "hull/commands/version.h"
#include "hull/agent_api.h"
#include "hull/utils/limits.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h"
#include "hull/utils/parse_size.h"
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

/* ── JSON writer plumbing ──────────────────────────────────────────── */

/* FILE* writer callback for ShJsonWriter — used by the agent-mode
 * .hull/last_error.json emitter below. Same shape as the helpers in
 * commands/cache.c / sbom.c / commands/doctor.c. */
static int serve_stdio_write(void *ctx, const char *data, size_t len)
{
    FILE *fp = (FILE *)ctx;
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

/* ── Logging ───────────────────────────────────────────────────────── */

/* Custom log callback: suppresses file:line in release builds.
 *
 * The format string is provided by the log.c callback API, not a
 * literal; vsnprintf is doing exactly what it's meant to. Suppress
 * -Wformat-nonliteral locally rather than restructuring the API. */
static void hl_log_callback(log_Event *ev) {
    char ts[16];
    ts[strftime(ts, sizeof(ts), "%H:%M:%S", ev->time)] = '\0';

    char msg[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(msg, sizeof(msg), ev->fmt, ev->ap);
#pragma GCC diagnostic pop

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

/* Bridge: routes Keel KlLogFn through rxi/log.c with [keel] prefix.
 * `fmt` comes from Keel's logging callback API; see the comment on
 * hl_log_callback above. */
static void hl_keel_log_bridge(int level, const char *fmt, va_list ap,
                                void *user_data) {
    (void)user_data;
    char buf[512];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(buf, sizeof(buf), fmt, ap);
#pragma GCC diagnostic pop
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

/* ── WASM/GPU statics (persist across hull_serve calls) ────────────── */

#ifdef HL_ENABLE_WASM
static HlWasmCache wasm_cache;
static int wasm_cache_ok = 0;
#endif
#ifdef HL_ENABLE_GPU
static HlGpuCtx gpu_ctx;
static int gpu_ctx_ok = 0;
#endif

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
            "  --no-verify-platform Skip gethull platform-sig check (v0.1.3+; for\n"
            "                       dev hulls or forks signing with their own key)\n"
            "  --drain-timeout MS   Graceful shutdown drain timeout (default: 5000)\n"
            "  --no-migrate         Skip auto-run migrations on startup\n"
            "  --no-ca-bundle       Skip TLS certificate verification (dev mode)\n"
            "  --ca-bundle PATH     Use custom CA bundle (overrides system + embedded)\n"
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
            "SIZE accepts optional suffix: k (KB), m (MB), g (GB).\n"
            "\n"
            "Help:\n"
            "  hull --help / hull help    Full subcommand reference\n"
            "  hull --version             Print version and exit\n"
            "\n"
            "AI agents: bootstrap with\n"
            "  hull agent context --task=orientation --level=minimal\n",
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

/* ── Server configuration (parsed from CLI + env) ──────────────────── */

typedef struct {
    int port;
    const char *bind_addr;
    const char *db_path;
    const char *entry_point;
    const char *verify_sig_path;
    const char *tls_cert_path;
    const char *tls_key_path;
    long heap_limit;
    long stack_limit;
    long mem_limit;
    long instruction_limit;
    long wasm_heap, wasm_stack, wasm_max_input, wasm_max_output;
    long long wasm_gas;
    int gpu_device;
    int log_level;
    int no_migrate;
    int no_sandbox;
    int no_db;
    int no_compress;
    int no_verify_platform;  /* skip gethull platform-sig check on --verify-sig */
    int skip_ca_bundle;
    const char *ca_bundle_override; /* --ca-bundle=PATH */
    int agent_mode;
    int agent_api_mode;
    int drain_timeout;
    int max_connections;
    long body_max_size;
    int read_timeout;
    int num_workers;
    int queue_capacity;

    /* CLI mode (app.main): everything after `--` on the command line is
     * passed to main as ctx.args. Borrows pointers from argv; lifetime
     * matches the original argv (hl_parse_serve_args copies neither). */
    char **app_args;
    int    app_argc;
} HlServeConfig;

/* No SQLite default path on a build without SQLite: a postgres-only binary with
 * no -d runs DB-less (like a compute-only build) instead of failing to open a
 * SQLite file it can't. An explicit postgres:// -d or manifest.databases still
 * works; an explicit SQLite -d correctly errors at open. */
#ifdef HL_ENABLE_SQLITE
#define HL_DEFAULT_DB_PATH "data.db"
#else
#define HL_DEFAULT_DB_PATH NULL
#endif

#define HL_SERVE_CONFIG_DEFAULT { \
    .port = HL_DEFAULT_PORT, \
    .bind_addr = "127.0.0.1", \
    .db_path = HL_DEFAULT_DB_PATH, \
    .gpu_device = -1, \
    .log_level = LOG_INFO, \
    .drain_timeout = HL_DEFAULT_DRAIN_TIMEOUT_MS, \
}

/* ── Argument parsing ──────────────────────────────────────────────── */

static int hl_parse_serve_args(int argc, char **argv, HlServeConfig *cfg)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char *end;
            long p = strtol(argv[++i], &end, 10);
            if (*end != '\0' || p < 1 || p > 65535) {
                fprintf(stderr, "hull: invalid port: %s\n", argv[i]);
                return -1;
            }
            cfg->port = (int)p;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            cfg->bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            cfg->db_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            cfg->heap_limit = hl_parse_size(argv[++i]);
            if (cfg->heap_limit <= 0) {
                fprintf(stderr, "hull: invalid heap size: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
            cfg->mem_limit = hl_parse_size(argv[++i]);
            if (cfg->mem_limit <= 0) {
                fprintf(stderr, "hull: invalid memory limit: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            cfg->stack_limit = hl_parse_size(argv[++i]);
            if (cfg->stack_limit <= 0) {
                fprintf(stderr, "hull: invalid stack size: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            cfg->log_level = hl_parse_log_level(argv[++i]);
            if (cfg->log_level < 0) {
                fprintf(stderr, "hull: invalid log level: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            cfg->tls_cert_path = argv[++i];
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            cfg->tls_key_path = argv[++i];
        } else if (strcmp(argv[i], "--verify-sig") == 0 && i + 1 < argc) {
            cfg->verify_sig_path = argv[++i];
        } else if (strcmp(argv[i], "--no-migrate") == 0) {
            cfg->no_migrate = 1;
        } else if (strcmp(argv[i], "--no-sandbox") == 0) {
            cfg->no_sandbox = 1;
        } else if (strcmp(argv[i], "--no-verify-platform") == 0) {
            cfg->no_verify_platform = 1;
        } else if (strcmp(argv[i], "--no-db") == 0) {
            cfg->no_db = 1;
        } else if (strcmp(argv[i], "--no-compress") == 0) {
            cfg->no_compress = 1;
        } else if (strcmp(argv[i], "--no-ca-bundle") == 0 ||
                   strcmp(argv[i], "--skip-ca-bundle") == 0) {
            /* --skip-ca-bundle accepted as deprecated alias (one cycle) */
            cfg->skip_ca_bundle = 1;
        } else if (strcmp(argv[i], "--ca-bundle") == 0 && i + 1 < argc) {
            cfg->ca_bundle_override = argv[++i];
        } else if (strncmp(argv[i], "--ca-bundle=", 12) == 0) {
            cfg->ca_bundle_override = argv[i] + 12;
        } else if (strcmp(argv[i], "--agent") == 0) {
            cfg->agent_mode = 1;
        } else if (strcmp(argv[i], "--agent-api") == 0) {
            cfg->agent_api_mode = 1;
        } else if (strcmp(argv[i], "--audit") == 0) {
            hl_audit_enabled = 1;
        } else if (strcmp(argv[i], "--max-instructions") == 0 && i + 1 < argc) {
            char *end;
            cfg->instruction_limit = strtol(argv[++i], &end, 10);
            if (*end != '\0' || cfg->instruction_limit < 0) {
                fprintf(stderr, "hull: invalid instruction limit: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--max-connections") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 100000) {
                fprintf(stderr, "hull: invalid max-connections: %s\n", argv[i]);
                return -1;
            }
            cfg->max_connections = (int)v;
        } else if (strcmp(argv[i], "--body-max-size") == 0 && i + 1 < argc) {
            cfg->body_max_size = hl_parse_size(argv[++i]);
            if (cfg->body_max_size <= 0) {
                fprintf(stderr, "hull: invalid body-max-size: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--read-timeout") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > 600000) {
                fprintf(stderr, "hull: invalid read-timeout: %s\n", argv[i]);
                return -1;
            }
            cfg->read_timeout = (int)v;
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 128) {
                fprintf(stderr, "hull: invalid workers: %s\n", argv[i]);
                return -1;
            }
            cfg->num_workers = (int)v;
        } else if (strcmp(argv[i], "--queue-capacity") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 10000) {
                fprintf(stderr, "hull: invalid queue-capacity: %s\n", argv[i]);
                return -1;
            }
            cfg->queue_capacity = (int)v;
        } else if (strcmp(argv[i], "--drain-timeout") == 0 && i + 1 < argc) {
            char *end;
            long dt = strtol(argv[++i], &end, 10);
            if (*end != '\0' || dt < 0 || dt > 300000) {
                fprintf(stderr, "hull: invalid drain timeout: %s\n", argv[i]);
                return -1;
            }
            cfg->drain_timeout = (int)dt;
        } else if (strcmp(argv[i], "--wasm-heap") == 0 && i + 1 < argc) {
            /* L1: validate hl_parse_size; previously a typo like "128MB"
             * silently set wasm_heap=-1 and was then masked by a
             * downstream `> 0` check that printed nothing. */
            long v = hl_parse_size(argv[++i]);
            if (v <= 0) {
                fprintf(stderr, "hull: invalid --wasm-heap: %s\n", argv[i]);
                return -1;
            }
            cfg->wasm_heap = v;
        } else if (strcmp(argv[i], "--wasm-stack") == 0 && i + 1 < argc) {
            long v = hl_parse_size(argv[++i]);
            if (v <= 0) {
                fprintf(stderr, "hull: invalid --wasm-stack: %s\n", argv[i]);
                return -1;
            }
            cfg->wasm_stack = v;
        } else if (strcmp(argv[i], "--wasm-gas") == 0 && i + 1 < argc) {
            char *end;
            long long v = strtoll(argv[++i], &end, 10);
            if (*end != '\0' || v <= 0) {
                fprintf(stderr, "hull: invalid --wasm-gas: %s\n", argv[i]);
                return -1;
            }
            cfg->wasm_gas = v;
        } else if (strcmp(argv[i], "--wasm-max-input") == 0 && i + 1 < argc) {
            long v = hl_parse_size(argv[++i]);
            if (v <= 0) {
                fprintf(stderr, "hull: invalid --wasm-max-input: %s\n", argv[i]);
                return -1;
            }
            cfg->wasm_max_input = v;
        } else if (strcmp(argv[i], "--wasm-max-output") == 0 && i + 1 < argc) {
            long v = hl_parse_size(argv[++i]);
            if (v <= 0) {
                fprintf(stderr, "hull: invalid --wasm-max-output: %s\n", argv[i]);
                return -1;
            }
            cfg->wasm_max_output = v;
#ifdef HL_ENABLE_GPU
        } else if (strcmp(argv[i], "--gpu-device") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > 15) {
                fprintf(stderr, "hull: invalid gpu-device: %s\n", argv[i]);
                return -1;
            }
            cfg->gpu_device = (int)v;
#endif
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 1; /* signal help shown, exit 0 */
        } else if (strcmp(argv[i], "--") == 0) {
            /* Everything past `--` is app argv (CLI mode). */
            cfg->app_args = &argv[i + 1];
            cfg->app_argc = argc - i - 1;
            break;
        } else if (argv[i][0] != '-') {
            cfg->entry_point = argv[i];
        }
    }

    /* Check HULL_AUDIT env var */
    {
        const char *audit_env = getenv("HULL_AUDIT");
        if (audit_env && strcmp(audit_env, "1") == 0)
            hl_audit_enabled = 1;
    }

    /* Check HULL_MAX_INSTRUCTIONS env var */
    if (cfg->instruction_limit == 0) {
        const char *il_env = getenv("HULL_MAX_INSTRUCTIONS");
        if (il_env) {
            char *end;
            long val = strtol(il_env, &end, 10);
            if (*end == '\0' && val >= 0)
                cfg->instruction_limit = val;
        }
    }

    return 0;
}

/* ── Manifest processing ──────────────────────────────────────────── */

static void hl_resolve_wasm_config(HlRuntime *rt, const HlManifest *manifest,
                                    const HlServeConfig *cfg)
{
#ifdef HL_ENABLE_WASM
    uint32_t wh = manifest->wasm_heap;
    uint32_t ws = manifest->wasm_stack;
    int64_t  wg = manifest->wasm_gas;
    uint32_t wi = manifest->wasm_max_input;
    uint32_t wo = manifest->wasm_max_output;

    /* CLI overrides manifest (operator > developer) */
    if (cfg->wasm_heap > 0)       wh = (uint32_t)cfg->wasm_heap;
    if (cfg->wasm_stack > 0)      ws = (uint32_t)cfg->wasm_stack;
    if (cfg->wasm_gas > 0)        wg = (int64_t)cfg->wasm_gas;
    if (cfg->wasm_max_input > 0)  wi = (uint32_t)cfg->wasm_max_input;
    if (cfg->wasm_max_output > 0) wo = (uint32_t)cfg->wasm_max_output;

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
#else
    (void)rt; (void)manifest; (void)cfg;
#endif
}

/* ── Server state (persists for server lifetime) ──────────────────── */

typedef struct {
    /* Configuration (parsed from CLI + env) */
    HlServeConfig        cfg;

    /* Entry point resolution */
    const char          *entry_point;  /* may alias entry_abs or argv string */
    char                 entry_abs[4096];
    char                 app_dir[4096];

    /* Runtime type (detected from extension) */
    HlRuntimeType        runtime;

    /* Logging + allocator (must outlive everything) */
    HlAllocator          alloc;
    KlAllocator          kl_alloc;

    /* Core app context (VFS + DB + runtime) — replaces individual
     * app_vfs, platform_vfs, db, stmt_cache, rt_storage, rt fields */
    HlAppContext        *app;

    /* Server + TLS */
    KlServer             server;
    KlTlsConfig          server_tls_config;
    KlTlsCtx            *server_tls_ctx;

    /* Backend wrap around server.ev. Created in init_infra (so the
     * thread pool can go through the backend vtable), borrowed by the
     * runtime via rt->async_ctx, unwrapped in cleanup. */
    HlAsyncBackendCtx   *async_ctx;

    /* HlNetBackend wrap around the KlServer. Created in init_server
     * (server must exist first), lent to rt->net_ctx in wire_caps,
     * unwrapped in teardown. Lets vtable consumers route async-op
     * suspend/complete through the backend instead of touching
     * Keel directly. */
    HlNetBackendCtx     *net_ctx;

    /* Infrastructure (thread pool, client pool, compression) */
    HlAsyncBackendPool  *thread_pool;
    KlClientPool         client_pool;
    int                  cpool_ok;
    KlCompressCtx       *comp_ctx;
    KlCompressConfig     compress_cfg;
    KlDecompressConfig   decompress_cfg;

    /* Manifest + wired capability configs */
    HlManifest           manifest;
    /* Page-backed RO storage for the sealed manifest's strings. The
     * manifest itself stays as an embedded value above (no pointer
     * indirection through HlRuntime / cap-config consumers required);
     * after hl_manifest_seal(), every const char * inside `manifest`
     * points into this arena, and the arena is mprotect()'d RO. The
     * attacker we're defending against has a heap memory-write bug
     * and is trying to expand fs.write / hosts / env allowlists
     * post-boot; sealing turns that into SIGSEGV.
     *
     * Sized for the bounded manifest fields (HL_MANIFEST_MAX_PATHS +
     * MODULES + HOSTS times reasonable path lengths) plus headroom;
     * one page (4 KiB) is enough today, we round up to a few for
     * future-proofing without measurable cost (~unused mmap pages
     * never get faulted in). */
    ShSealArena          seal_arena;
    int                  manifest_sealed; /* 1 after successful seal */
    HlResolvedModuleSet  module_set; /* frozen after resolver; consulted by gating */
    HlFsConfig           fs_cfg_storage;
    HlEnvConfig          env_cfg_storage;
    HlHttpConfig         http_cfg_storage;
    HlSmtpConfig         smtp_cfg_storage;
    KlTlsConfig          client_tls_config;
    KlTlsCtx            *client_tls_ctx;
    const char           *ca_bundle_path;
    /* CORS config — allocated INSIDE the seal arena (RO after seal)
     * so the origin allowlist a heap-write primitive could otherwise
     * mutate is mprotect-locked.  NULL when the manifest doesn't
     * declare a cors section. */
    KlCorsConfig        *cors_cfg;

    /* Agent API context */
    HlAgentApiCtx        agent_api_ctx;

    /* Init flags for cleanup ordering */
    int                  server_init;  /* kl_server_init succeeded */
    int                  app_loaded;   /* rt->vt->load_app() succeeded */
    int                  manifest_extracted; /* extract_manifest called */

    /* CLI mode (app.main) state */
    int                  app_main_only;  /* 1 = app registered main + no
                                          *     server handlers; serve loop
                                          *     is skipped and the process
                                          *     exits with cli_exit_code. */
    int                  cli_exit_code; /* main's return value (0..255) */
} HlServerState;

/* ── Phase functions ─────────────────────────────────────────────────
 *
 * Each phase receives HlServerState *s and returns 0 on success, -1 on
 * error (or a positive value for special cases like help).  The phases
 * must run in the order listed — later phases depend on earlier ones.
 *
 * Ordering constraints documented at each phase.
 * ─────────────────────────────────────────────────────────────────── */

/* Phase 1: Parse CLI args into s->cfg.
 * Returns 0 on success, -1 on parse error, 1 if help was shown. */
static int hl_serve_parse_args(HlServerState *s, int argc, char **argv)
{
    s->cfg = (HlServeConfig)HL_SERVE_CONFIG_DEFAULT;
    int rc = hl_parse_serve_args(argc, argv, &s->cfg);
    return rc; /* 0 = ok, -1 = error, 1 = help shown */
}

/* Phase 2: Resolve entry_point to absolute path, derive app_dir.
 * Depends on: parse_args (s->cfg.entry_point). */
static int hl_serve_resolve_entry(HlServerState *s, const char *argv0)
{
    s->entry_point = s->cfg.entry_point;

    if (!s->entry_point)
        s->entry_point = auto_detect_entry();

    if (!s->entry_point) {
        fprintf(stderr, "hull: no entry point found (app.js or app.lua)\n");
        usage(argv0);
        return -1;
    }

    /* Resolve entry point to absolute path.  This ensures app_dir (derived
     * below) is also absolute, so realpath() inside the sandbox doesn't need
     * to stat the CWD — which may be outside the sandbox's allowed paths. */
    if (realpath(s->entry_point, s->entry_abs) != NULL)
        s->entry_point = s->entry_abs;

    /* Derive app directory from entry point (needed for migrations + static files + sandbox) */
    {
        const char *slash = strrchr(s->entry_point, '/');
        if (slash) {
            size_t len = (size_t)(slash - s->entry_point);
            if (len >= sizeof(s->app_dir)) {
                fprintf(stderr, "hull: entry point path too long (max %zu chars)\n",
                        sizeof(s->app_dir) - 1);
                return -1;
            }
            memcpy(s->app_dir, s->entry_point, len);
            s->app_dir[len] = '\0';
        } else {
            /* No slash means either dev mode with cwd entry (`hull
             * app.lua`) or a built standalone binary whose entry
             * is the embedded "./app". In both cases the app_dir
             * "is" cwd. We MUST canonicalize to an absolute path:
             * downstream consumers (hl_cap_blob_init,
             * hl_cap_fs_validate) require base_dir to be absolute,
             * otherwise blob_store_open rejects with -1 and the
             * app's blob.init silently fails. realpath(".")
             * resolves to the actual cwd at startup, which is
             * what dev mode would produce if the file path had
             * had a slash. */
            if (realpath(".", s->app_dir) == NULL) {
                /* Pathological: cwd unreadable or too long. Fall
                 * back to "." so existing code paths still work
                 * for the common dev case where everything's
                 * cwd-relative anyway. */
                s->app_dir[0] = '.';
                s->app_dir[1] = '\0';
            }
        }
    }

    return 0;
}

/* Phase 3 (VFS init) — now handled inside hl_app_context_init. */

/* Phase 4: Validate TLS pair, detect and check runtime type.
 * Depends on: parse_args (TLS paths), resolve_entry (entry_point). */
static int hl_serve_validate_config(HlServerState *s)
{
    /* Validate TLS cert/key pair */
    if ((s->cfg.tls_cert_path != NULL) != (s->cfg.tls_key_path != NULL)) {
        fprintf(stderr, "hull: --tls-cert and --tls-key must be provided together\n");
        return -1;
    }

    s->runtime = detect_runtime(s->entry_point);

    /* Validate that the requested runtime is compiled in */
#ifndef HL_ENABLE_JS
    if (s->runtime == HL_RUNTIME_JS) {
        fprintf(stderr, "hull: QuickJS runtime not enabled in this build\n");
        return -1;
    }
#endif
#ifndef HL_ENABLE_LUA
    if (s->runtime == HL_RUNTIME_LUA) {
        fprintf(stderr, "hull: Lua runtime not enabled in this build\n");
        return -1;
    }
#endif

    return 0;
}

/* Phase 5: Set log level, init tracking allocator, create route arena.
 * Depends on: parse_args (log_level, mem_limit). */
static int hl_serve_init_logging(HlServerState *s)
{
    hl_log_make_threadsafe();  /* workers (db/wasm/gpu) log concurrently */
    log_set_level(s->cfg.log_level);
    log_set_quiet(true);  /* suppress default stderr callback */
    log_add_callback(hl_log_callback, stderr, s->cfg.log_level);

    /* Initialize tracking allocator */
    hl_alloc_init(&s->alloc, (size_t)s->cfg.mem_limit);
    s->kl_alloc = hl_alloc_kl(&s->alloc);

    /* Create route allocation arena (256 routes x 64 bytes = 16KB) */
    route_arena = hl_arena_create(&s->alloc, HL_MAX_ROUTES * 64);
    if (!route_arena) {
        log_error("[hull:c] route arena allocation failed");
        return -1;
    }

    return 0;
}

/* Phase 6 (DB init) — now handled inside hl_app_context_init. */

/* Phase 7: Create KlServer with optional TLS.
 * Depends on: init_logging (kl_alloc), parse_args (port, bind, TLS paths). */
static int hl_serve_init_server(HlServerState *s)
{
    KlConfig config = {
        .port = s->cfg.port,
        .bind_addr = s->cfg.bind_addr,
        .max_connections = s->cfg.max_connections > 0 ? s->cfg.max_connections : HL_DEFAULT_MAX_CONN,
        .read_timeout_ms = s->cfg.read_timeout > 0 ? s->cfg.read_timeout : HL_DEFAULT_READ_TIMEOUT_MS,
        .max_body_size = s->cfg.body_max_size > 0 ? (size_t)s->cfg.body_max_size : HL_BODY_MAX_SIZE,
        .install_signal_handlers = 1,
        .drain_timeout_ms = s->cfg.drain_timeout,
        .alloc = &s->kl_alloc,
        .log_fn = hl_keel_log_bridge,
        .log_user_data = NULL,
    };

    /* Set up server TLS if cert/key provided */
    memset(&s->server_tls_config, 0, sizeof(s->server_tls_config));
    s->server_tls_ctx = NULL;

    if (s->cfg.tls_cert_path && s->cfg.tls_key_path) {
        s->server_tls_ctx = kl_tls_mbedtls_ctx_create(
            s->cfg.tls_cert_path, s->cfg.tls_key_path, NULL, KL_MTLS_NONE, &s->kl_alloc);
        if (!s->server_tls_ctx) {
            log_error("[hull:c] failed to create server TLS context "
                      "(cert=%s, key=%s)", s->cfg.tls_cert_path, s->cfg.tls_key_path);
            return -1;
        }
        s->server_tls_config.ctx         = s->server_tls_ctx;
        s->server_tls_config.factory     = (KlTlsFactory)kl_tls_mbedtls_create;
        s->server_tls_config.ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy;
        config.tls = &s->server_tls_config;
    }

    if (kl_server_init(&s->server, &config) != 0) {
        log_error("[hull:c] server init failed: %s",
                  kl_strerror(s->server.last_error));
        if (s->server_tls_ctx) {
            kl_tls_mbedtls_ctx_destroy(s->server_tls_ctx);
            s->server_tls_ctx = NULL;
        }
        return -1;
    }
    s->server_init = 1;

    return 0;
}

/* Phase 8: Thread pool, client pool, compression (all non-fatal).
 * Depends on: init_server (server.ev for thread pool + client pool). */
static void hl_serve_init_infra(HlServerState *s)
{
    /* Wrap the server's event loop so the thread pool + later
     * consumers go through the async backend vtable. The wrap is
     * borrowed — KlServer still owns the underlying KlEventCtx. */
    if (!s->async_ctx)
        s->async_ctx = hl_async_backend_keel_wrap(&s->server.ev);

    /* Wrap the server itself so net-backend consumers can route
     * connection-bound async-op suspend/complete through the vtable
     * instead of calling kl_async_* directly. Same borrowed-pointer
     * pattern as async_ctx. */
    if (!s->net_ctx)
        s->net_ctx = hl_net_backend_keel_wrap(&s->server);

    /* Create thread pool for async work (db queries, file I/O) via
     * the vtable. With the keel backend this still ends up calling
     * kl_thread_pool_create under the hood, just routed through
     * HlAsyncBackend so future backends slot in. */
    const HlAsyncBackend *be = hl_async_backend();
    int num_workers    = s->cfg.num_workers > 0 ? s->cfg.num_workers : HL_THREAD_POOL_WORKERS;
    int queue_capacity = s->cfg.queue_capacity > 0 ? s->cfg.queue_capacity : HL_THREAD_POOL_CAPACITY;
    if (be->pool_create(&s->thread_pool, s->async_ctx,
                         num_workers, queue_capacity) != 0) {
        s->thread_pool = NULL;
        log_warn("[hull:c] thread pool creation failed — async work unavailable");
    }
    /* thread_pool may be NULL if creation fails — non-fatal, async work
     * will simply be unavailable */

    /* Create client connection pool for HTTP keep-alive reuse */
    s->cpool_ok = kl_cpool_init(&s->client_pool, NULL, &s->kl_alloc, &s->server.ev);
    if (s->cpool_ok != 0)
        log_warn("[hull:c] client pool creation failed — connection reuse disabled");

    /* Create compression context for server responses and client decompression */
    s->comp_ctx = NULL;
    memset(&s->compress_cfg, 0, sizeof(s->compress_cfg));
    memset(&s->decompress_cfg, 0, sizeof(s->decompress_cfg));

    if (!s->cfg.no_compress) {
        s->comp_ctx = kl_compress_miniz_ctx_create(6, &s->kl_alloc);
        if (s->comp_ctx) {
            s->compress_cfg.ctx = s->comp_ctx;
            s->compress_cfg.factory = (KlCompressFactory)kl_compress_miniz_create;
            s->compress_cfg.ctx_destroy =
                (void (*)(KlCompressCtx *))kl_compress_miniz_ctx_destroy;
            /* Note: config is local to init_server, but compress_cfg is used
             * by the runtime for client-side decompression.  The server's
             * KlConfig.compress is only read during kl_server_init, which has
             * already returned.  We wire rt->compress below instead. */

            s->decompress_cfg.ctx = s->comp_ctx;
            s->decompress_cfg.factory =
                (KlDecompressFactory)kl_decompress_miniz_create;
        } else {
            log_warn("[hull:c] compression init failed — responses uncompressed");
        }
    }
}

/* Phase 9: Create HlAppContext (VFS + DB + runtime), with deferred app
 * loading so that Phase 10 can verify signatures and apply phase-1
 * sandbox between init and load.
 * Depends on: init_server, init_infra (thread_pool, comp_ctx). */
static int hl_serve_init_app_context(HlServerState *s)
{
#ifdef HL_ENABLE_WASM
    /* Initialize WAMR compute runtime (static cache persists across calls).
     * Wired to context via opts so module registration in init() can see it.
     * If the manifest later declares compute: false, we NULL it out. */
    if (!wasm_cache_ok) {
        if (hl_cap_wasm_init(&wasm_cache) == 0)
            wasm_cache_ok = 1;
        else
            log_warn("[hull:c] WAMR init failed — compute.call() unavailable");
    }
#endif

#ifdef HL_ENABLE_GPU
    /* Initialize GPU compute runtime (static cache persists across calls). */
    if (!gpu_ctx_ok) {
        if (hl_cap_gpu_init(&gpu_ctx, &hl_gpu_backend_wgpu) == HL_GPU_OK
            && hl_cap_gpu_available(&gpu_ctx)) {
            if (s->cfg.gpu_device >= 0 && s->cfg.gpu_device < gpu_ctx.device_count)
                gpu_ctx.default_device = s->cfg.gpu_device;
            gpu_ctx_ok = 1;
        } else {
            log_info("[hull:c] GPU compute unavailable — gpu.* disabled");
        }
    }
#endif

    HlAppContextOpts app_opts = {
        .app_dir           = s->app_dir,
        .entry_point       = s->entry_point,
        .db_path           = s->cfg.no_db ? NULL : s->cfg.db_path,
        .no_migrate        = s->cfg.no_migrate,
        .no_db             = s->cfg.no_db,
        .sandbox           = 1,
        .alloc             = &s->alloc,
        .thread_pool       = s->thread_pool,
        .worker_db_path    = s->cfg.no_db ? NULL : s->cfg.db_path,
        .compress          = s->comp_ctx ? &s->compress_cfg : NULL,
        .heap_limit        = s->cfg.heap_limit,
        .stack_limit       = s->cfg.stack_limit,
        .instruction_limit = s->cfg.instruction_limit,
        .no_load           = 1,  /* deferred: Phase 10 loads after sig verify */
#ifdef HL_ENABLE_WASM
        .wasm_cache        = wasm_cache_ok ? &wasm_cache : NULL,
#endif
#ifdef HL_ENABLE_GPU
        .gpu_ctx           = gpu_ctx_ok ? &gpu_ctx : NULL,
        .gpu_device        = s->cfg.gpu_device,
#endif
    };

    if (hl_app_context_init(&s->app, &app_opts) != 0) {
        log_error("[hull:c] app context init failed");
        return -1;
    }

    return 0;
}

/* Phase 10: Verify signature, apply phase-1 sandbox, load app code.
 * Depends on: init_app_context (runtime initialized, app not yet loaded). */
static int hl_serve_load_app(HlServerState *s)
{
    const HlVfs *app_vfs = hl_app_context_app_vfs(s->app);

    /* RT-01: Verify app signature BEFORE loading — malicious code never
     * executes if verification fails. */
    if (s->cfg.verify_sig_path) {
        if (hl_verify_startup(s->cfg.verify_sig_path, s->entry_point, app_vfs,
                              s->cfg.no_verify_platform) != 0) {
            log_error("[hull:c] signature verification failed — refusing to start");
            return -1;
        }
        log_info("[hull:c] signature verified OK");
    }

    /* Phase 1 sandbox: block exec/proc/fork before loading user code */
    if (!s->cfg.no_sandbox) {
        if (hl_sandbox_apply_pledge() != 0) {
            log_error("[hull:c] failed to apply phase 1 sandbox");
            return -1;
        }
    }

    /* Load and evaluate the app (runs under phase 1 pledge) */
    if (hl_app_context_load(s->app, s->entry_point) != 0) {
        log_error("[hull:c] failed to load %s", s->entry_point);
        if (s->cfg.agent_mode) {
            char err_dir[4096], err_path[4096];
            int nd = snprintf(err_dir, sizeof(err_dir), "%s/.hull", s->app_dir);
            int np = snprintf(err_path, sizeof(err_path),
                              "%s/.hull/last_error.json", s->app_dir);
            if (nd > 0 && (size_t)nd < sizeof(err_dir) &&
                np > 0 && (size_t)np < sizeof(err_path)) {
                mkdir(err_dir, 0755);
                FILE *ef = fopen(err_path, "w");
                if (ef) {
                    /* entry_point is app-config-derived (controlled
                     * today) but routing through sh_json removes the
                     * latent escape vector if it ever takes external
                     * input. */
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                             "failed to load %s", s->entry_point);
                    ShJsonWriter w;
                    sh_json_writer_init(&w, serve_stdio_write, ef);
                    sh_json_write_object_start(&w);
                    sh_json_write_kv_string(&w, "error", msg);
                    sh_json_write_kv_int   (&w, "timestamp",
                                            (int64_t)time(NULL));
                    sh_json_write_object_end(&w);
                    fputc('\n', ef);
                    fclose(ef);
                }
            }
        }
        return -1;
    }
    s->app_loaded = 1;

    /* Clear previous error file on successful load */
    if (s->cfg.agent_mode) {
        char err_path[4096];
        int np = snprintf(err_path, sizeof(err_path),
                          "%s/.hull/last_error.json", s->app_dir);
        if (np > 0 && (size_t)np < sizeof(err_path))
            unlink(err_path);
    }

    return 0;
}

/* Phase 11: Extract manifest, wire caps, phase-2 sandbox, CORS, routes,
 * static, agent API, run event loop, post-run cleanup.
 * Depends on: load_app (app code loaded). WASM/GPU destroy happens here
 * on the success path only — not on error gotos. */
/* ── hl_serve_wire_and_start — orchestrates phases below ───────────────
 *
 * Phases (each a small static helper):
 *   1. hl_serve_wire_caps              — extract manifest, wire fs/env/http/
 *                                        smtp + TLS client context + CSP
 *   2. hl_serve_apply_sandbox          — build HlSandboxPolicy, apply
 *   3. hl_serve_wire_routes            — CORS, vtable route wiring, static
 *                                        middleware, agent diagnostic API
 *   4. hl_serve_run                    — kl_server_run event loop
 *   5. hl_serve_teardown_after_serve   — success-path resource teardown
 *
 * Split out of a 296-line monolith as roadmap item H.
 */

/* Phase 1: extract manifest + resolve module set + wire per-capability configs.
 *
 * Returns 0 on success, -1 if module resolution fails (unknown name,
 * version mismatch, missing capability, undeclared dep). Module
 * resolution failures are fatal because they indicate the manifest
 * doesn't match what the runtime is about to be configured to expose. */
static int hl_serve_wire_caps(HlServerState *s)
{
    HlRuntime *rt = hl_app_context_runtime(s->app);

    /* The server's event loop was wrapped in init_infra (so the thread
     * pool could go through the backend); just lend the existing wrap
     * to the runtime so its consumers reach the loop via the vtable. */
    rt->async_ctx = s->async_ctx;
    /* Same pattern for the net backend wrap — runtime borrows it so
     * connection-bound async-op suspend/complete go through the vtable
     * instead of touching KlServer directly. */
    rt->net_ctx = s->net_ctx;

    /* Extract manifest and configure capabilities */
    memset(&s->manifest, 0, sizeof(s->manifest));
    if (rt->vt->extract_manifest(rt, &s->manifest) == 0) {
        s->manifest_extracted = 1;
        log_info("[hull:c] manifest: fs_read=%d fs_write=%d env=%d hosts=%d modules=%d",
                 s->manifest.fs_read_count, s->manifest.fs_write_count,
                 s->manifest.env_count, s->manifest.hosts_count,
                 s->manifest.modules_count);

        /* Seal the manifest's strings into a read-only mmap arena BEFORE
         * the resolver / sandbox / capability layer start consuming it.
         * From this point on, every fs_read[i] / fs_write[i] / hosts[i]
         * / env[i] / csp / cors_* / modules[i].name points into a
         * mprotect(PROT_READ) page. An attacker with a heap memory-
         * write bug who tries to expand the allowlists post-boot gets
         * SIGSEGV instead of a silent capability escalation.
         *
         * Sealing failure is FATAL — the alternative is shipping with
         * unsealed policy, which silently weakens the hardening
         * guarantee. See docs/security.md §Sealed runtime tables. */
        if (sh_seal_arena_init(&s->seal_arena, 16 * 1024,
                                "manifest-policy") != 0) {
            log_error("[hull:c] seal arena init failed (mmap)");
            return -1;
        }
        HlManifest sealed;
        if (hl_manifest_seal(&sealed, &s->manifest, &s->seal_arena) != 0) {
            log_error("[hull:c] manifest seal failed (arena OOM?)");
            sh_seal_arena_destroy(&s->seal_arena);
            return -1;
        }
        /* Swap: free the original allocator-backed strings, then
         * bitwise-copy the sealed struct in. sealed.alloc is NULL so
         * subsequent hl_manifest_free(&s->manifest) is a safe no-op
         * for strings (the arena owns them; destroyed at shutdown). */
        hl_manifest_free(&s->manifest);
        s->manifest = sealed;

        /* Point the connection registry at the now-sealed databases map so
         * db.connect("<name>") can resolve declared named connections. Until
         * this runs only the seeded "default" resolves. The sealed manifest
         * lives for the process, satisfying the registry's borrow. */
#ifdef HL_ENABLE_DB
        if (rt->db_registry)
            hl_db_registry_set_manifest(rt->db_registry, &s->manifest);
#endif

        /* CORS config — build it INSIDE the seal arena alongside the
         * manifest strings, BEFORE the mprotect.  Previously the
         * KlCorsConfig was a stack-local in hl_serve_wire_routes;
         * kl_server_use stored the pointer, which dangled the moment
         * wire_routes returned.  Allocating in the arena (a) fixes
         * the dangling-pointer UAF, and (b) means the origin allowlist
         * is mprotect-RO for the rest of process lifetime — a heap-
         * write primitive can no longer punch a new origin into it. */
        if (s->manifest.cors_set) {
            KlCorsConfig *cc = sh_seal_arena_alloc(
                &s->seal_arena, sizeof(*cc), _Alignof(KlCorsConfig));
            if (!cc) {
                log_error("[hull:c] seal arena alloc(cors) failed");
                sh_seal_arena_destroy(&s->seal_arena);
                return -1;
            }
            kl_cors_init(cc);
            for (int i = 0; i < s->manifest.cors_origin_count; i++)
                kl_cors_add_origin(cc, s->manifest.cors_origins[i]);
            /* The cors_methods/cors_headers strings already live in
             * the seal arena (sealed manifest pointers); cc just
             * aliases them. */
            if (s->manifest.cors_methods)
                cc->allowed_methods = s->manifest.cors_methods;
            if (s->manifest.cors_headers)
                cc->allowed_headers = s->manifest.cors_headers;
            cc->allow_credentials = s->manifest.cors_credentials;
            if (s->manifest.cors_max_age > 0)
                cc->max_age_seconds = s->manifest.cors_max_age;
            s->cors_cfg = cc;
        }

        /* Resolve the module set BEFORE sealing the arena.  The
         * resolver writes its 16-byte bitset to s->module_set as a
         * scratch buffer, then we memdup it into the arena so the
         * sealed copy is what the runtime consults on every
         * require/import.  This closes the "manifest declared ->
         * resolved set sealed -> per-import check" loop:
         * a heap-write primitive that flips a bit in the bitset to
         * admit an undeclared module hits the mprotect-RO page and
         * faults. */
        {
            char err[HL_MODULE_RESOLVER_ERR_MAX] = {0};
            if (hl_module_resolver_resolve(&s->manifest, &s->module_set,
                                            err, sizeof(err)) != 0) {
                log_error("[hull:c] %s", err);
                sh_seal_arena_destroy(&s->seal_arena);
                return -1;
            }
            HlResolvedModuleSet *resolved =
                (HlResolvedModuleSet *)sh_seal_arena_memdup(
                    &s->seal_arena, &s->module_set,
                    sizeof(HlResolvedModuleSet));
            if (!resolved) {
                log_error("[hull:c] seal arena alloc(module_set) failed");
                sh_seal_arena_destroy(&s->seal_arena);
                return -1;
            }
            rt->module_set = resolved;
        }

        if (sh_seal_arena_seal(&s->seal_arena) != 0) {
            log_error("[hull:c] seal arena mprotect failed");
            sh_seal_arena_destroy(&s->seal_arena);
            return -1;
        }
        s->manifest_sealed = 1;

        log_info("[hull:c] modules resolved: %d admitted",
                 hl_module_set_count(rt->module_set));
    } else {
        /* No manifest: resolve admits only the intrinsic core.  No
         * arena exists in this branch (nothing else to seal); the
         * resolved bitset stays in the heap-resident s->module_set.
         * That's fine because the attack surface a bitset seal
         * defends — flipping bits to admit an undeclared module —
         * needs a manifest with declarations to escalate FROM.
         * "intrinsics only" is already the floor. */
        char err[HL_MODULE_RESOLVER_ERR_MAX] = {0};
        if (hl_module_resolver_resolve(NULL, &s->module_set,
                                        err, sizeof(err)) != 0) {
            log_error("[hull:c] %s", err);
            return -1;
        }
        rt->module_set = &s->module_set;
        log_info("[hull:c] modules resolved: %d admitted",
                 hl_module_set_count(rt->module_set));
    }

    /* Validate the pre-manifest import tracker against the resolved
     * set.  Top-level Lua require / JS import statements run before
     * the gate is wired, so the gate records the canonical names it
     * let through during that window into rt->import_tracker_*.
     * Walking them here catches the silent-bypass case where an app
     * imports e.g. hull:db but never declares hull/db@1.  Reads
     * only — works against either the sealed (manifest path) or
     * unsealed (no-manifest path) bitset. */
    {
        char itrack_err[256] = {0};
        if (hl_import_tracker_validate(rt, rt->module_set,
                                        itrack_err, sizeof(itrack_err)) != 0) {
            log_error("[hull:c] %s", itrack_err);
            return -1;
        }
    }

    /* Wire CSP policy to runtime.
     * Default CSP is always active — even without app.manifest().
     * Explicit csp="custom" overrides; csp=false disables.
     * csp="<preset>" expands at startup; unknown names pass through
     * as literal CSP strings (see hl_csp_resolve docs above). */
    if (s->manifest.csp_set)
        rt->csp_policy = hl_csp_resolve(s->manifest.csp);
    else
        rt->csp_policy = HL_DEFAULT_CSP;  /* default */

#ifdef HL_ENABLE_WASM
    /* WASM gating. Two paths:
     *   - Modern: app declares `modules.compute = "1"` → admit.
     *   - Legacy: app declares `compute = true` (without modules block) → admit.
     * If the modules block is present, it is authoritative — the legacy
     * `compute = true` field is ignored. Otherwise fall back to legacy. */
    if (wasm_cache_ok) {
        int admit;
        if (s->manifest.modules_declared)
            admit = hl_module_set_contains_name(&s->module_set, "hull/compute");
        else
            admit = !s->manifest.present || s->manifest.compute;
        if (!admit) {
            rt->wasm_cache = NULL;
            log_info("[hull:c] compute not declared — compute.* disabled");
        }
    }

    /* Resolve three-tier WASM config: CLI > manifest > compile-time defaults */
    hl_resolve_wasm_config(rt, &s->manifest, &s->cfg);
#endif

#ifdef HL_ENABLE_GPU
    /* GPU gating mirrors the WASM rule above. */
    if (gpu_ctx_ok) {
        int admit;
        if (s->manifest.modules_declared)
            admit = hl_module_set_contains_name(&s->module_set, "hull/gpu");
        else
            admit = !s->manifest.present || s->manifest.gpu;
        if (!admit) {
            rt->gpu_ctx = NULL;
            log_info("[hull:c] gpu not declared — gpu.* disabled");
        }
    }
    /* Apply per-device allowlist if manifest declares specific devices */
    if (gpu_ctx_ok && s->manifest.present && s->manifest.gpu &&
        s->manifest.gpu_device_count > 0) {
        gpu_ctx.device_restriction = 1;
        memset(gpu_ctx.allowed_devices, 0, sizeof(gpu_ctx.allowed_devices));
        for (int i = 0; i < s->manifest.gpu_device_count; i++) {
            int d = s->manifest.gpu_devices[i];
            if (d >= 0 && d < gpu_ctx.device_count)
                gpu_ctx.allowed_devices[d] = 1;
        }
        log_info("[hull:c] gpu device restriction: %d of %d devices allowed",
                 s->manifest.gpu_device_count, gpu_ctx.device_count);
    }
#endif

    /* Wire fs_cfg from manifest (if app declares fs.read OR fs.write
     * paths — both blob.* and fs.mmap need the cfg). */
    memset(&s->fs_cfg_storage, 0, sizeof(s->fs_cfg_storage));
    if (s->manifest.fs_read_count > 0 || s->manifest.fs_write_count > 0) {
        s->fs_cfg_storage.base_dir = s->app_dir;
        s->fs_cfg_storage.base_len = strlen(s->app_dir);
        rt->fs_cfg = &s->fs_cfg_storage;
    }

    /* Wire env_cfg from manifest (if app declares env vars) */
    memset(&s->env_cfg_storage, 0, sizeof(s->env_cfg_storage));
    if (s->manifest.env_count > 0) {
        s->env_cfg_storage.allowed = s->manifest.env;
        s->env_cfg_storage.count   = s->manifest.env_count;
        rt->env_cfg = &s->env_cfg_storage;
    }

    /* Wire http_cfg from manifest (if app declares hosts) */
    memset(&s->http_cfg_storage, 0, sizeof(s->http_cfg_storage));
    memset(&s->client_tls_config, 0, sizeof(s->client_tls_config));
    s->client_tls_ctx = NULL;
    s->ca_bundle_path = NULL;

    if (s->manifest.hosts_count > 0) {
        s->http_cfg_storage.allowed_hosts     = s->manifest.hosts;
        s->http_cfg_storage.count             = s->manifest.hosts_count;
        s->http_cfg_storage.timeout_ms        = KL_CLIENT_DEFAULT_TIMEOUT_MS;
        s->http_cfg_storage.max_response_size = KL_CLIENT_DEFAULT_MAX_RESP;

        /* Set up TLS client for HTTPS support.
         *
         * Resolution order:
         *   1. --no-ca-bundle (or --skip-ca-bundle alias) → no verification (dev only)
         *   2. --ca-bundle PATH (override) → use that file
         *   3. system CA store at well-known paths
         *   4. embedded Mozilla bundle (when HL_EMBED_CA_BUNDLE=1)
         *   5. fail with a clear hint
         */
        if (s->cfg.skip_ca_bundle) {
            log_warn("[hull:c] TLS certificate verification disabled (--no-ca-bundle)");
            s->client_tls_ctx = kl_tls_mbedtls_client_ctx_create(NULL, &s->kl_alloc);
        } else if (s->cfg.ca_bundle_override) {
            s->ca_bundle_path = s->cfg.ca_bundle_override;
            log_info("[hull:c] using CA bundle (override): %s", s->ca_bundle_path);
            s->client_tls_ctx = kl_tls_mbedtls_client_ctx_create(s->ca_bundle_path, &s->kl_alloc);
            if (!s->client_tls_ctx)
                log_warn("[hull:c] failed to load CA bundle from %s", s->ca_bundle_path);
        } else {
            s->ca_bundle_path = find_ca_bundle();
            if (s->ca_bundle_path) {
                log_info("[hull:c] using CA bundle: %s", s->ca_bundle_path);
                s->client_tls_ctx = kl_tls_mbedtls_client_ctx_create(s->ca_bundle_path, &s->kl_alloc);
            } else {
                /* System bundle not found — try embedded fallback */
                const unsigned char *emb_data = NULL;
                size_t emb_len = 0;
                if (hl_embedded_ca_bundle(&emb_data, &emb_len) == 0) {
                    log_info("[hull:c] using embedded CA bundle (%s)",
                             hl_embedded_ca_bundle_label());
                    s->client_tls_ctx = kl_tls_mbedtls_client_ctx_create_from_buf(
                        emb_data, emb_len, &s->kl_alloc);
                    /* Sentinel so doctor / introspection sees "(embedded)" */
                    s->ca_bundle_path = "(embedded)";
                    if (!s->client_tls_ctx)
                        log_warn("[hull:c] failed to parse embedded CA bundle");
                } else {
                    log_warn("[hull:c] no CA bundle found; HTTPS disabled "
                             "(use --no-ca-bundle, --ca-bundle PATH, or "
                             "build with HL_EMBED_CA_BUNDLE=1)");
                }
            }
        }

        if (s->client_tls_ctx) {
            s->client_tls_config.ctx         = s->client_tls_ctx;
            s->client_tls_config.factory     = (KlTlsFactory)kl_tls_mbedtls_create;
            s->client_tls_config.ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy;
            s->http_cfg_storage.tls          = &s->client_tls_config;
        }

        /* Enable connection pooling and redirect following */
        if (s->cpool_ok == 0)
            s->http_cfg_storage.pool = &s->client_pool;
        s->http_cfg_storage.follow_redirects = 1;

        /* Enable client-side response decompression */
        if (s->comp_ctx)
            s->http_cfg_storage.decompress = &s->decompress_cfg;

        rt->http_cfg = &s->http_cfg_storage;
    }

    /* Wire smtp_cfg — shares same host allowlist and TLS context as HTTP */
    memset(&s->smtp_cfg_storage, 0, sizeof(s->smtp_cfg_storage));
    if (s->manifest.hosts_count > 0) {
        s->smtp_cfg_storage.allowed_hosts = s->manifest.hosts;
        s->smtp_cfg_storage.host_count    = s->manifest.hosts_count;
        s->smtp_cfg_storage.timeout_ms    = HL_SMTP_DEFAULT_TIMEOUT_MS;
        s->smtp_cfg_storage.tls           = s->client_tls_ctx ? &s->client_tls_config : NULL;
        rt->smtp_cfg = &s->smtp_cfg_storage;
    }

    return 0;
}

/* Cleanup if a phase fails after wire_caps has succeeded. */
static void hl_serve_undo_caps(HlServerState *s)
{
    /* hl_manifest_free is a safe no-op on a sealed manifest (the
     * `sealed` flag short-circuits the per-string free walk). */
    hl_manifest_free(&s->manifest);
    s->manifest_extracted = 0;
    if (s->client_tls_ctx) {
        kl_tls_mbedtls_ctx_destroy(s->client_tls_ctx);
        s->client_tls_ctx = NULL;
    }
    /* Sealed manifest arena: LAST. The TLS ctx destroyed above (and
     * any other cap-config consumer added in the future) may alias
     * manifest string pointers; unmapping the arena earlier turns
     * those aliases into faults. */
    if (s->manifest_sealed) {
        sh_seal_arena_destroy(&s->seal_arena);
        s->manifest_sealed = 0;
    }
}

/* Phase 2: apply the OS-level sandbox built from the resolved manifest. */
static int hl_serve_apply_sandbox(HlServerState *s)
{
    if (!s->cfg.no_sandbox) {
        HlSandboxPolicy sandbox_policy;
        hl_sandbox_policy_from_manifest(&sandbox_policy, &s->manifest);

        /* CLI-mode apps (app.main registered) never accept inbound
         * connections — narrow the sandbox accordingly. The runtime
         * vtable's has_main was populated in mod_app.c at app-load time,
         * so this check is purely informational here. */
        HlRuntime *rt = hl_app_context_runtime(s->app);
        int has_main_only = rt && rt->vt
            && rt->vt->has_main && rt->vt->has_main(rt)
            && (!rt->vt->has_server_handlers
                || !rt->vt->has_server_handlers(rt));
        if (has_main_only) {
            /* Pure CLI app (main, no handlers) doesn't accept inbound
             * connections. Init-then-serve apps still need bind+listen
             * so the server loop can run after main returns. */
            sandbox_policy.network_inbound = 0;
        }

        if (hl_sandbox_apply(&sandbox_policy, s->app_dir,
                              s->cfg.no_db ? NULL : s->cfg.db_path, s->ca_bundle_path,
                              s->cfg.tls_cert_path, s->cfg.tls_key_path) != 0) {
            log_error("[hull:c] sandbox enforcement failed");
            return -1;
        }
    } else {
        log_warn("[hull:c] kernel sandbox disabled (--no-sandbox)");
    }
    return 0;
}

/* Phase 3: CORS + route wiring + static + agent API. */
static int hl_serve_wire_routes(HlServerState *s)
{
    HlRuntime *rt = hl_app_context_runtime(s->app);
    const HlVfs *app_vfs = hl_app_context_app_vfs(s->app);
    if (s->cors_cfg) {
        /* CORS config was built and sealed in hl_serve_wire_caps —
         * the pointer is mprotect-RO and outlives the server.  Earlier
         * versions stack-allocated this struct here and passed its
         * address to kl_server_use; the moment this function returned
         * the pointer dangled (Keel later dereferenced into whatever
         * the stack had been reused for). */
        kl_server_use(&s->server, "*", "/*", kl_cors_middleware, s->cors_cfg);
        log_info("[hull:c] CORS enabled (%d origin(s), sealed)",
                 s->manifest.cors_origin_count);
    }

    /* Wire routes into Keel (after sandbox is applied) */
    if (rt->vt->wire_routes_server(rt, &s->server, track_route_alloc) != 0) {
        return -1;
    }

    /* Auto-register static file serving (after sandbox is applied).
     *
     * The middleware activates when the app has any static/ content
     * OR the platform VFS ships stdlib assets under static/hull/
     * (e.g. when an htmx widget module is linked in). Either trigger
     * is enough — the lookup tries app VFS first, then stdlib VFS,
     * then dev-mode filesystem. */
    {
        const HlVfs *platform_vfs = hl_app_context_platform_vfs(s->app);
        int has_app_static = hl_vfs_has_prefix(app_vfs, "static/");
        if (!has_app_static) {
            char static_dir[4096];
            snprintf(static_dir, sizeof(static_dir), "%s/static", s->app_dir);
            struct stat sdir;
            if (stat(static_dir, &sdir) == 0 && S_ISDIR(sdir.st_mode))
                has_app_static = 1;
        }
        int has_stdlib_static = platform_vfs
            && hl_vfs_has_prefix(platform_vfs, "static/hull/");
        if (has_app_static || has_stdlib_static) {
            HlStaticCtx *sctx = track_route_alloc(sizeof(HlStaticCtx));
            sctx->vfs = hl_app_context_app_vfs(s->app);
            sctx->stdlib_vfs = has_stdlib_static ? platform_vfs : NULL;
            kl_server_use(&s->server, "GET", "/static/*",
                          hl_static_middleware, sctx);
        }
    }

    /* Register agent diagnostic endpoints (opt-in via --agent-api) */
    s->agent_api_ctx = (HlAgentApiCtx){ .app_dir = s->app_dir,
                                        .db_path = s->cfg.no_db ? NULL : s->cfg.db_path };
    if (s->cfg.agent_api_mode)
        hl_agent_api_register(&s->server, &s->agent_api_ctx);
    return 0;
}

/* Phase 4: log + enter the event loop until the server stops. */
static void hl_serve_run(HlServerState *s)
{
    HlRuntime *rt = hl_app_context_runtime(s->app);
    log_info("[hull:c] listening on %s://%s:%d (%s runtime)",
             s->server_tls_ctx ? "https" : "http",
             s->cfg.bind_addr, s->cfg.port, rt->vt->name);

    /* Enter event loop */
    if (kl_server_run(&s->server) < 0)
        log_error("[hull:c] server exited with error: %s",
                  kl_strerror(s->server.last_error));

    log_info("[hull:c] server stopped");
}

/* Phase 5: tear down all resources after the event loop returns. */
static void hl_serve_teardown_after_serve(HlServerState *s)
{
    /* Free thread pool BEFORE server — join workers, drain queues while
     * server infrastructure (connections, event loop) is still valid. */
    if (s->thread_pool) {
        hl_async_backend()->pool_free(s->thread_pool);
        s->thread_pool = NULL;
    }

    /* Free client connection pool (closes idle connections) */
    if (s->cpool_ok == 0) {
        kl_cpool_free(&s->client_pool);
        s->cpool_ok = -1; /* mark freed */
    }

    /* Free compression context (shared by compress + decompress) */
    if (s->comp_ctx) {
        kl_compress_miniz_ctx_destroy(s->comp_ctx);
        s->comp_ctx = NULL;
    }

    /* Cleanup — free manifest AFTER server stops (env_cfg and
     * http_cfg reference its strings during runtime). For a sealed
     * manifest hl_manifest_free is a no-op for strings (alloc=NULL);
     * the actual memory is released when the seal arena is destroyed
     * MUCH later — after hl_app_context_free, after WASM/GPU caches,
     * after TLS contexts. Runtime shutdown still touches the cap
     * configs (fs_cfg/http_cfg/env_cfg) which alias manifest string
     * pointers, so the mapping must remain live until every consumer
     * has had its turn. Without that ordering, the runtime's
     * shutdown callbacks dereference into munmap'd pages and crash
     * with SIGSEGV/SIGABRT mid-shutdown. */
    if (s->manifest_extracted) {
        hl_manifest_free(&s->manifest);
        s->manifest_extracted = 0;
    }

    /* Detach the borrowed async_ctx + net_ctx pointers from the runtime
     * before app_context_free destroys the runtime — the wraps themselves
     * are owned by HlServerState and unwrapped further down. */
    {
        HlRuntime *rt = hl_app_context_runtime(s->app);
        if (rt) {
            rt->async_ctx = NULL;
            rt->net_ctx = NULL;
        }
    }

    /* Free app context (runtime + DB + VFS + stmt cache).
     * WASM/GPU static caches are external — freed separately below. */
    hl_app_context_free(s->app);
    s->app = NULL;

    /* Now unwrap the async-backend wrap — borrowed, doesn't destroy
     * KlServer's KlEventCtx (server's own free does that). */
    if (s->async_ctx) {
        hl_async_backend_keel_unwrap(s->async_ctx);
        s->async_ctx = NULL;
    }

    /* Same for the net-backend wrap — borrowed, doesn't destroy
     * KlServer (server's own free does that). */
    if (s->net_ctx) {
        hl_net_backend_keel_unwrap(s->net_ctx);
        s->net_ctx = NULL;
    }

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
    if (s->client_tls_ctx) {
        kl_tls_mbedtls_ctx_destroy(s->client_tls_ctx);
        s->client_tls_ctx = NULL;
    }
    if (s->server_tls_ctx) {
        kl_tls_mbedtls_ctx_destroy(s->server_tls_ctx);
        s->server_tls_ctx = NULL;
    }

    /* Sealed manifest arena: LAST to be destroyed. Every consumer
     * above (runtime / cap configs / WASM cache GC finalizers / TLS
     * contexts that may have manifest-host pointers) has aliased
     * pointers into this mapping; munmapping it earlier turns those
     * aliases into faults. */
    if (s->manifest_sealed) {
        sh_seal_arena_destroy(&s->seal_arena);
        s->manifest_sealed = 0;
    }
}

/* Build a NULL-terminated env_allowlist from the manifest. The returned
 * array points into manifest storage (no allocation), so it's valid only
 * for the lifetime of the manifest. Caller frees the outer array. */
static const char **hl_build_env_allowlist(const HlManifest *m)
{
    if (!m || m->env_count <= 0) return NULL;
    const char **arr = calloc((size_t)m->env_count + 1, sizeof(char *));
    if (!arr) return NULL;
    for (int i = 0; i < m->env_count; i++)
        arr[i] = m->env[i];
    arr[m->env_count] = NULL;
    return arr;
}

/* CLI-mode dispatch: instead of starting Keel, invoke app.main(ctx)
 * once and exit with its return code. Called after wire_caps +
 * apply_sandbox when the runtime reports has_main(). For Phase 1, app
 * args are empty (no positional pass-through wired in yet — covered in
 * Phase 2 by `hull run`). */
static int hl_serve_run_main(HlServerState *s)
{
    HlRuntime *rt = hl_app_context_runtime(s->app);
    const char **env_allow = hl_build_env_allowlist(&s->manifest);
    log_info("[hull:c] running app.main (%s runtime)", rt->vt->name);

    int rc = 1;
    int run = rt->vt->run_main(rt, &s->server,
                                s->cfg.app_argc, s->cfg.app_args,
                                env_allow, &rc);
    free((void *)env_allow);

    s->cli_exit_code = rc;
    if (run != 0)
        return -1;

    log_info("[hull:c] app.main exited with code %d", rc);
    return 0;
}

static int hl_serve_wire_and_start(HlServerState *s)
{
    if (hl_serve_wire_caps(s) != 0) {
        hl_serve_undo_caps(s);
        return -1;
    }
    if (hl_serve_apply_sandbox(s) != 0) {
        hl_serve_undo_caps(s);
        return -1;
    }

    /* Lifecycle (one rule covers all three modes):
     *
     *   1. If app.main is registered, run it once on the event loop
     *      thread. Non-zero return = fatal → exit with that code.
     *   2. If any server handlers (routes, middleware, timers, ws,
     *      sse) are registered, enter the HTTP serve loop until
     *      SIGINT.
     *   3. Otherwise return — the app had nothing left to do.
     *
     * Pure CLI (main, no handlers) and pure web (handlers, no main)
     * are the special cases; init-then-serve (both registered) is
     * the unified case. */
    HlRuntime *rt = hl_app_context_runtime(s->app);
    int has_main = rt->vt->has_main && rt->vt->has_main(rt);
    int has_handlers = rt->vt->has_server_handlers
                       && rt->vt->has_server_handlers(rt);

    if (has_main) {
        s->app_main_only = !has_handlers;
        int main_rc = hl_serve_run_main(s);
        if (main_rc != 0) {
            /* run_main returned non-zero — either an internal error
             * (-1) or main itself exited non-zero. Either way, don't
             * enter the server loop. */
            hl_serve_teardown_after_serve(s);
            return main_rc;
        }
        if (s->cli_exit_code != 0) {
            /* main returned non-zero — short-circuit even if handlers
             * are registered. Matches the shell-style "main exit code
             * wins" rule. */
            hl_serve_teardown_after_serve(s);
            return 0;        /* hl_serve_wire_and_start success; the
                              * outer caller reads s->cli_exit_code. */
        }
        if (!has_handlers) {
            /* Pure CLI — main was the whole program. */
            hl_serve_teardown_after_serve(s);
            return 0;
        }
        /* Fall through: main succeeded and handlers exist. The HTTP
         * serve loop takes over below as if main had been a startup
         * hook. */
    }

    if (hl_serve_wire_routes(s) != 0) {
        hl_serve_undo_caps(s);
        return -1;
    }

    /* Close the registration bindings (app.get / use / use_post / ws /
     * sse / every / daily).  From here on, calls into those bindings
     * (e.g. from a request handler that mistakenly tries dynamic route
     * registration) throw a structured error instead of silently
     * dropping the request into a table no consumer reads.  Matches the
     * router freeze below — together they make "no registration after
     * the serve loop starts" enforceable from both the script side
     * (clear error message) and the C side (RO route table).
     *
     * Intentionally flipped BEFORE kl_server_freeze.  If the freeze
     * itself fails (logged below, non-fatal), we WANT the flag to
     * stay set anyway — the serve loop is about to start and no late
     * registration should land regardless of whether the router seal
     * succeeded.  The two seals are independent halves of the same
     * "no late registration" invariant. */
    {
        HlRuntime *rt_close = hl_app_context_runtime(s->app);
        if (rt_close) rt_close->registration_closed = 1;
    }

    /* Freeze the routing tables (Keel v2.3.0+).  After this call,
     * KlRouter's route + middleware tables live in an mprotect-RO
     * arena — a heap-write primitive that would otherwise overwrite
     * a handler function pointer or relax a middleware gate faults
     * instead.  Failure is non-fatal here (logged) because the
     * server is still correct without the seal; the seal is a
     * hardening measure, not a correctness one. */
    if (kl_server_freeze(&s->server) != 0) {
        log_warn("[hull:c] kl_server_freeze failed — server unfrozen "
                 "(routing tables remain writable, hardening reduced)");
    } else {
        log_debug("[hull:c] router frozen (RO routing tables)");
    }

    hl_serve_run(s);
    hl_serve_teardown_after_serve(s);
    return 0;
}

/* ── Cleanup (checks init flags before freeing) ──────────────────────
 * Called on any failure path.  Tears down resources in reverse init
 * order, checking flags to avoid double-free or use-before-init.
 * WASM/GPU are NOT destroyed here — they're only destroyed on the
 * success path (in hl_serve_wire_and_start, after kl_server_run). */
static void hl_serve_cleanup(HlServerState *s)
{
    /* Free manifest if extracted but wire_and_start didn't finish.
     * For a sealed manifest this is a no-op (sealed flag short-circuits);
     * the actual mapping is destroyed at the END of this function, AFTER
     * every consumer that may hold aliased pointers into it (cap configs
     * on the runtime, TLS ctxs, server's route table). */
    if (s->manifest_extracted)
        hl_manifest_free(&s->manifest);

    /* Free thread pool BEFORE server (via the backend vtable) */
    if (s->thread_pool) {
        hl_async_backend()->pool_free(s->thread_pool);
        s->thread_pool = NULL;
    }

    /* Free client connection pool */
    if (s->cpool_ok == 0)
        kl_cpool_free(&s->client_pool);

    /* Free compression context */
    if (s->comp_ctx)
        kl_compress_miniz_ctx_destroy(s->comp_ctx);

    /* Free client TLS context */
    if (s->client_tls_ctx)
        kl_tls_mbedtls_ctx_destroy(s->client_tls_ctx);

    /* Free server TLS context (if server init failed after TLS ctx create) */
    if (s->server_tls_ctx)
        kl_tls_mbedtls_ctx_destroy(s->server_tls_ctx);

    if (s->server_init)
        kl_server_free(&s->server);

    /* Detach the borrowed async_ctx + net_ctx from the runtime before
     * app_context_free destroys the runtime — both wraps are owned by
     * HlServerState. */
    if (s->app) {
        HlRuntime *rt = hl_app_context_runtime(s->app);
        if (rt) {
            rt->async_ctx = NULL;
            rt->net_ctx = NULL;
        }
    }

    /* Free app context (runtime + DB + VFS + stmt cache) */
    if (s->app) {
        hl_app_context_free(s->app);
        s->app = NULL;
    }

    /* Unwrap the async-backend wrap (borrowed — does not destroy
     * KlServer's KlEventCtx). */
    if (s->async_ctx) {
        hl_async_backend_keel_unwrap(s->async_ctx);
        s->async_ctx = NULL;
    }

    /* Same for the net-backend wrap. */
    if (s->net_ctx) {
        hl_net_backend_keel_unwrap(s->net_ctx);
        s->net_ctx = NULL;
    }

    free_route_allocs(&s->alloc);

    /* Sealed manifest arena: LAST to be destroyed. Cap configs
     * (env_cfg.allowed = manifest.env, http_cfg.allowed_hosts =
     * manifest.hosts) and TLS contexts alias the sealed strings;
     * unmapping the arena earlier would turn those aliases into
     * faults during runtime/server cleanup above. */
    if (s->manifest_sealed) {
        sh_seal_arena_destroy(&s->seal_arena);
        s->manifest_sealed = 0;
    }

    log_debug("[hull:c] peak memory: %zu bytes", hl_alloc_peak(&s->alloc));
}

/* ── Server mode (default) ──────────────────────────────────────────── */

int hull_serve(int argc, char **argv)
{
    HlServerState s;
    memset(&s, 0, sizeof(s));

    /* Phase 1: parse CLI args */
    int rc = hl_serve_parse_args(&s, argc, argv);
    if (rc != 0) return rc < 0 ? 1 : 0; /* -1 = error, 1 = help shown */

    /* Phase 2: resolve entry point + app_dir */
    if (hl_serve_resolve_entry(&s, argv[0]) != 0) return 1;

    /* Phase 3 (VFS) + Phase 6 (DB) are now handled by hl_app_context_init */

    /* Phase 4: validate TLS pair, detect runtime */
    if (hl_serve_validate_config(&s) != 0) return 1;

    /* Phase 5: logging, allocator, route arena */
    if (hl_serve_init_logging(&s) != 0) return 1;

    /* Phase 7: Keel server + TLS */
    if (hl_serve_init_server(&s) != 0) { hl_serve_cleanup(&s); return 1; }

    /* Mark this (the server's main) thread as the event-loop thread
     * before the worker pool is created, so thread-affinity assertions on
     * the loop-only paths (segment mutation, async submit, GPU compile)
     * have a happens-before edge to every worker. Compiled out in release. */
    hl_event_loop_mark_current();

    /* Phase 8: thread pool, client pool, compression (non-fatal) */
    hl_serve_init_infra(&s);

    /* Phase 9: VFS + DB + runtime init via HlAppContext (deferred app load) */
    if (hl_serve_init_app_context(&s) != 0) { hl_serve_cleanup(&s); return 1; }

    /* Phase 10: signature verify, phase-1 sandbox, load app */
    if (hl_serve_load_app(&s) != 0) { hl_serve_cleanup(&s); return 1; }

    /* Phase 11: manifest, caps, phase-2 sandbox, routes, event loop */
    int wire_rc = hl_serve_wire_and_start(&s);
    int ret;
    if (s.app_main_only) {
        /* Pure CLI (app.main only, no handlers) — main's return value
         * is the process exit code. */
        ret = s.cli_exit_code;
    } else if (s.cli_exit_code != 0) {
        /* Init-then-serve, but main short-circuited with a non-zero
         * exit. Honour main's exit code; the serve loop was never
         * entered. */
        ret = s.cli_exit_code;
    } else {
        /* Either handlers-only (no main) or main-then-serve where
         * main succeeded; defer to whether the server loop finished
         * cleanly. */
        ret = (wire_rc == 0) ? 0 : 1;
    }

    /* Final cleanup (handles any resources not freed by wire_and_start) */
    hl_serve_cleanup(&s);
    return ret;
}
