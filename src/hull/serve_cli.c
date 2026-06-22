/*
 * serve_cli.c — `hull run` driver for HL_ENABLE_HTTP_SERVER=0 builds.
 *
 * Replaces serve.c on CLI builds. The flow:
 *   1. Parse CLI args (`-d <path>`, `--no-migrate`, `--no-sandbox`,
 *      `--` separator for app args).
 *   2. Load app via HlAppContext (migrations run here if HL_ENABLE_DB
 *      and -d points at a real file).
 *   3. Wire fs / env / http (incl. TLS for HL_ENABLE_HTTP_CLIENT=1)
 *      configs from manifest.
 *   4. Apply sandbox.
 *   5. Create the async backend ctx + thread pool so app.main can
 *      use hull.sleep, db.async, compute.async, gpu.async, http.fetch
 *      (async ops on a CLI build run on the poll backend's loop).
 *   6. Invoke app.main(ctx); return its exit code.
 *
 * Limitations vs the full serve.c:
 *   - No HTTP server (HL_ENABLE_HTTP_SERVER=0 → no Keel server, no
 *     routing, no middleware/ws/sse). Apps must use app.main as the
 *     entry point.
 *   - HTTP client (http.fetch) only available when
 *     HL_ENABLE_HTTP_CLIENT=1; otherwise the binding is filtered out
 *     of the runtime.
 *   - In-process HTTP test harness (cap/test.c, test_runner.c) is
 *     dropped along with the server, so `hull test` is unavailable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/shared/async_backend.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h"
#include "hull/runtime.h"
#include "hull/sandbox.h"
#include "hull/serve.h"
#include "hull/cap/fs.h"
#include "hull/cap/env.h"

#ifdef HL_ENABLE_HTTP_CLIENT
#include "hull/cap/http.h"
#include "hull/cacert.h"
#include <keel/client.h>
#include <keel/tls_mbedtls.h>
#endif

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static const char **build_env_allowlist(const HlManifest *m)
{
    if (!m || m->env_count <= 0) return NULL;
    const char **arr = calloc((size_t)m->env_count + 1, sizeof(char *));
    if (!arr) return NULL;
    for (int i = 0; i < m->env_count; i++)
        arr[i] = m->env[i];
    arr[m->env_count] = NULL;
    return arr;
}

/* Extract entry point (first positional arg) and the `--` separator.
 * Returns the entry point's argv index, fills app_argc/app_argv pointers
 * to the slice past `--`. Returns -1 if no entry point given. */
static int cli_parse_args(int argc, char **argv,
                          int *out_app_argc, char ***out_app_argv,
                          int *out_no_migrate, int *out_no_sandbox,
                          const char **out_db_path)
{
    int entry_idx = -1;
    *out_app_argc = 0;
    *out_app_argv = NULL;
    *out_no_migrate = 0;
    *out_no_sandbox = 0;
    *out_db_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            *out_app_argv = &argv[i + 1];
            *out_app_argc = argc - i - 1;
            break;
        }
        if (strcmp(argv[i], "--no-migrate") == 0) {
            *out_no_migrate = 1;
            continue;
        }
        if (strcmp(argv[i], "--no-sandbox") == 0) {
            *out_no_sandbox = 1;
            continue;
        }
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            *out_db_path = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') continue;
        if (entry_idx < 0) entry_idx = i;
    }
    return entry_idx;
}

int hull_serve(int argc, char **argv)
{
    int app_argc = 0;
    char **app_argv = NULL;
    int no_migrate = 0, no_sandbox = 0;
    const char *db_path = NULL;

    int entry_idx = cli_parse_args(argc, argv, &app_argc, &app_argv,
                                    &no_migrate, &no_sandbox, &db_path);
    if (!db_path) db_path = getenv("HULL_DB");
    if (entry_idx < 0) {
        fprintf(stderr,
            "hull: no entry point given. Usage: hull run <app.lua|app.js> "
            "[-- args...]\n");
        return 1;
    }

    /* Derive app_dir from the entry point path (parent directory of
     * the .lua/.js file, or "." if no slash). app_context needs both. */
    const char *entry = argv[entry_idx];
    char entry_abs[4096];
    if (realpath(entry, entry_abs) != NULL)
        entry = entry_abs;

    char app_dir[4096];
    const char *slash = strrchr(entry, '/');
    if (slash) {
        size_t len = (size_t)(slash - entry);
        if (len >= sizeof(app_dir)) {
            fprintf(stderr, "hull: entry path too long\n");
            return 1;
        }
        memcpy(app_dir, entry, len);
        app_dir[len] = '\0';
    } else {
        app_dir[0] = '.';
        app_dir[1] = '\0';
    }

    /* Init app context — runs migrations + loads the app. */
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = {
        .app_dir         = app_dir,
        .entry_point     = entry,
        .db_path         = db_path,        /* NULL → ":memory:" */
        .worker_db_path  = db_path,        /* worker-thread sqlite connection */
        .no_migrate      = no_migrate,
        .sandbox         = !no_sandbox,
        .gate_modules    = 1,
    };
    if (hl_app_context_init(&ctx, &opts) != 0) {
        fprintf(stderr, "hull: failed to initialize app context\n");
        return 1;
    }

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) {
        fprintf(stderr, "hull: no runtime available\n");
        hl_app_context_free(ctx);
        return 1;
    }

    /* CLI mode requires app.main — server routes can't be registered
     * on HL_ENABLE_HTTP_SERVER=0 builds (the bindings are dropped). */
    if (!rt->vt->has_main || !rt->vt->has_main(rt)) {
        fprintf(stderr,
            "hull: app did not register app.main(). This build was "
            "compiled with HL_ENABLE_HTTP_SERVER=0 and cannot serve "
            "HTTP. Either add app.main(fn) to your app, or rebuild "
            "hull with HL_ENABLE_HTTP_SERVER=1.\n");
        hl_app_context_free(ctx);
        return 1;
    }

    /* Sandbox: extract manifest + apply policy. The runtime gates were
     * already applied via gate_modules; this is the OS-level sandbox. */
    HlManifest manifest = {0};
    if (rt->vt->extract_manifest(rt, &manifest) != 0) {
        log_warn("[hull:cli] manifest extraction failed; running without policy");
    }

    /* Wire per-capability configs from the manifest. serve.c does this
     * in `wire_caps` for the server build; in CLI mode we do the same
     * dance inline so capabilities that gate on manifest declarations
     * (fs.read paths, env allowlist, http hosts + TLS) work out of
     * app.main. */
    HlFsConfig fs_cfg = {0};
    if (manifest.fs_read_count > 0 || manifest.fs_write_count > 0) {
        fs_cfg.base_dir = app_dir;
        fs_cfg.base_len = strlen(app_dir);
        rt->fs_cfg = &fs_cfg;
    }

    HlEnvConfig env_cfg = {0};
    if (manifest.env_count > 0) {
        env_cfg.allowed = manifest.env;
        env_cfg.count   = manifest.env_count;
        rt->env_cfg = &env_cfg;
    }

#ifdef HL_ENABLE_HTTP_CLIENT
    /* http.fetch needs allowlisted hosts + a TLS client for https://.
     * The embedded Mozilla CA bundle is the default trust anchor —
     * matches the resolution order in serve.c minus the system-store
     * probe (which adds platform-specific cruft we don't need for CLI). */
    HlHttpConfig http_cfg = {0};
    KlAllocator kalloc    = kl_allocator_default();
    KlTlsConfig tls_cfg   = {0};
    KlTlsCtx *tls_ctx     = NULL;

    if (manifest.hosts_count > 0) {
        http_cfg.allowed_hosts     = manifest.hosts;
        http_cfg.count             = manifest.hosts_count;
        http_cfg.timeout_ms        = KL_CLIENT_DEFAULT_TIMEOUT_MS;
        http_cfg.max_response_size = KL_CLIENT_DEFAULT_MAX_RESP;
        http_cfg.follow_redirects  = 1;

        const unsigned char *emb_data = NULL;
        size_t emb_len = 0;
        if (hl_embedded_ca_bundle(&emb_data, &emb_len) == 0) {
            tls_ctx = kl_tls_mbedtls_client_ctx_create_from_buf(
                emb_data, emb_len, &kalloc);
            if (tls_ctx) {
                tls_cfg.ctx         = tls_ctx;
                tls_cfg.factory     = (KlTlsFactory)kl_tls_mbedtls_create;
                tls_cfg.ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy;
                http_cfg.tls        = &tls_cfg;
            }
        }
        rt->http_cfg = &http_cfg;
    }
#endif

    if (!no_sandbox) {
        HlSandboxPolicy sandbox_policy;
        hl_sandbox_policy_from_manifest(&sandbox_policy, &manifest);
        /* CLI mode → no inbound network. */
        sandbox_policy.network_inbound = 0;

        if (hl_sandbox_apply(&sandbox_policy, app_dir, db_path,
                             NULL, NULL, NULL) != 0) {
            log_error("[hull:cli] sandbox enforcement failed");
#ifdef HL_ENABLE_HTTP_CLIENT
            if (tls_ctx) kl_tls_mbedtls_ctx_destroy(tls_ctx);
#endif
            hl_manifest_free(&manifest);
            hl_app_context_free(ctx);
            return 1;
        }
    }

    const char **env_allow = build_env_allowlist(&manifest);

    /* Create an event loop and worker pool for app.main. The runtime's
     * vt_*_run_main drives this loop while main is suspended on async
     * ops (hull.sleep, compute.async, db.async, etc.). */
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *async_ctx = NULL;
    if (be->init(&async_ctx, NULL) != 0) {
        fprintf(stderr, "[hull:cli] failed to init async backend\n");
#ifdef HL_ENABLE_HTTP_CLIENT
        if (tls_ctx) kl_tls_mbedtls_ctx_destroy(tls_ctx);
#endif
        free((void *)env_allow);
        hl_manifest_free(&manifest);
        hl_app_context_free(ctx);
        return 1;
    }
    rt->async_ctx = async_ctx;

    /* Worker pool for async ops that delegate to threads (db.async,
     * compute.async, gpu.async). Non-fatal if it fails — the runtime
     * checks for NULL before submitting. */
    HlAsyncBackendPool *pool = NULL;
    if (be->pool_create(&pool, async_ctx, 4, 64) != 0) {
        pool = NULL;
        fprintf(stderr, "[hull:cli] thread pool init failed — async ops unavailable\n");
    }
    rt->thread_pool = pool;

    int rc = 1;
    int run = rt->vt->run_main(rt, NULL, app_argc, app_argv, env_allow, &rc);

    /* Detach borrowed pointers before tearing down (mirrors serve.c). */
    rt->thread_pool = NULL;
    rt->async_ctx = NULL;
    rt->fs_cfg = NULL;
    rt->env_cfg = NULL;
#ifdef HL_ENABLE_HTTP_CLIENT
    rt->http_cfg = NULL;
    if (tls_ctx) kl_tls_mbedtls_ctx_destroy(tls_ctx);
#endif
    if (pool) be->pool_free(pool);
    be->free(async_ctx);

    free((void *)env_allow);
    hl_manifest_free(&manifest);
    hl_app_context_free(ctx);

    return (run == 0) ? rc : (rc ? rc : 1);
}
