/*
 * serve_cli.c — `hull run` driver for HL_ENABLE_HTTP=0 builds.
 *
 * Replaces serve.c on CLI-only builds. The flow is the same minus
 * Keel: load app via HlAppContext, resolve modules, apply sandbox,
 * invoke app.main, exit with the return code.
 *
 * Limitations of this driver compared to serve.c (documented in
 * docs/cli_mode.md as Phase 3d work):
 *   - No Keel event loop ⇒ async-in-main (hull.sleep, compute.async,
 *     gpu.async, http.fetch async, db.async) is not supported. Sync
 *     versions still work.
 *   - No thread pool wiring.
 *   - Test commands that target server apps (the existing in-process
 *     HTTP harness) aren't available.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/async_backend.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h"
#include "hull/runtime.h"
#include "hull/sandbox.h"
#include "hull/serve.h"

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
                          int *out_no_migrate, int *out_no_sandbox)
{
    int entry_idx = -1;
    *out_app_argc = 0;
    *out_app_argv = NULL;
    *out_no_migrate = 0;
    *out_no_sandbox = 0;

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

    int entry_idx = cli_parse_args(argc, argv, &app_argc, &app_argv,
                                    &no_migrate, &no_sandbox);
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
        .app_dir      = app_dir,
        .entry_point  = entry,
        .no_migrate   = no_migrate,
        .sandbox      = !no_sandbox,
        .gate_modules = 1,
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

    /* CLI mode requires app.main. */
    if (!rt->vt->has_main || !rt->vt->has_main(rt)) {
        fprintf(stderr,
            "hull: app did not register app.main(). This build was "
            "compiled with HL_ENABLE_HTTP=0 and cannot serve HTTP. "
            "Either register app.main(fn) in your app, or rebuild "
            "hull with HL_ENABLE_HTTP=1.\n");
        hl_app_context_free(ctx);
        return 1;
    }

    /* Sandbox: extract manifest + apply policy. The runtime gates were
     * already applied via gate_modules; this is the OS-level sandbox. */
    HlManifest manifest = {0};
    if (rt->vt->extract_manifest(rt, &manifest) != 0) {
        log_warn("[hull:cli] manifest extraction failed; running without policy");
    }

    if (!no_sandbox) {
        HlSandboxPolicy sandbox_policy;
        hl_sandbox_policy_from_manifest(&sandbox_policy, &manifest);
        /* CLI mode → no inbound network. */
        sandbox_policy.network_inbound = 0;

        if (hl_sandbox_apply(&sandbox_policy, NULL, NULL, NULL, NULL, NULL) != 0) {
            log_error("[hull:cli] sandbox enforcement failed");
            hl_manifest_free(&manifest);
            hl_app_context_free(ctx);
            return 1;
        }
    }

    const char **env_allow = build_env_allowlist(&manifest);

    /* Create an async-backend ctx — the poll backend on HTTP=0, the
     * keel backend on HTTP=1. The runtime's vt_*_run_main drives this
     * loop while main is suspended on async ops (hull.sleep at
     * minimum; other async ops still depend on a connection and
     * remain unavailable in CLI mode pending the worker-detached
     * follow-up). */
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *async_ctx = NULL;
    if (be->init(&async_ctx, NULL) != 0) {
        fprintf(stderr, "[hull:cli] failed to init async backend\n");
        free((void *)env_allow);
        hl_manifest_free(&manifest);
        hl_app_context_free(ctx);
        return 1;
    }
    rt->async_ctx = async_ctx;

    int rc = 1;
    int run = rt->vt->run_main(rt, NULL, app_argc, app_argv, env_allow, &rc);

    /* Detach borrowed pointer before tearing down (mirrors serve.c). */
    rt->async_ctx = NULL;
    be->free(async_ctx);

    free((void *)env_allow);
    hl_manifest_free(&manifest);
    hl_app_context_free(ctx);

    return (run == 0) ? rc : (rc ? rc : 1);
}
