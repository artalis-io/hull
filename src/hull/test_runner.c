/*
 * test_runner.c - Shared test-discovery + orchestration for hull test
 *                 and hull agent test.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/test_runner.h"
#include "hull/app_context.h"
#include "hull/cap/test.h"
#include "hull/cap/tool.h"
#include "hull/runtime.h"
#include "hull/manifest.h"
#include "hull/shared/async_backend.h"
#include "hull/shared/log_lock.h"
#include "hull/shared/thread_affinity.h"
#ifdef HL_ENABLE_DB
#include "hull/cap/db_budget.h"   /* hl_db_op_shutdown */
#endif

#include <keel/allocator.h>
#include <keel/http_router.h>

#include <stdlib.h>
#include <string.h>

#define HL_TEST_RUNNER_MAX_RESULTS 1024

/* ── The run's event loop (test_runner.h) ─────────────────────────────
 * The app.main runner's (serve_cli.c), minus the sandbox: hull test runs
 * none. The pool's threads start on its first submit. */
int hl_test_runner_loop_init(HlTestRunnerLoop *loop)
{
    if (!loop) return -1;
    loop->ctx = NULL;
    loop->pool = NULL;
    const HlAsyncBackend *be = hl_async_backend();
    if (!be || be->init(&loop->ctx, NULL) != 0) {
        loop->ctx = NULL;
        return -1;
    }
    /* Worker threads log concurrently, and the loop-only thread-affinity
     * assertions must see this thread as the loop's. */
    hl_log_make_threadsafe();
    hl_event_loop_mark_current();
    if (be->pool_create(&loop->pool, loop->ctx, 4, 64) != 0)
        loop->pool = NULL;
    return 0;
}

void hl_test_runner_loop_attach(HlTestRunnerLoop *loop, HlRuntime *rt)
{
    if (!loop || !loop->ctx || !rt) return;
    rt->async_ctx = loop->ctx;
    rt->thread_pool = loop->pool;
}

/* Teardown order as the app.main runner's (CLAUDE.md "Teardown order"):
 * the pool with the runtime alive, then what the runtime still holds on
 * the loop, then the runtime's pointers to both. */
void hl_test_runner_loop_detach(HlTestRunnerLoop *loop, HlRuntime *rt)
{
    if (!loop || !loop->ctx) return;
    const HlAsyncBackend *be = hl_async_backend();
    if (rt) { rt->thread_pool = NULL; rt->shutting_down = 1; }
    if (loop->pool) {
#ifdef HL_ENABLE_DB
        hl_db_op_shutdown(1);
#endif
        be->pool_free(loop->pool);
#ifdef HL_ENABLE_DB
        hl_db_op_shutdown(0);
#endif
        loop->pool = NULL;
    }
    if (rt && rt->vt && rt->vt->stop_async) rt->vt->stop_async(rt);
    if (rt) rt->async_ctx = NULL;
}

void hl_test_runner_loop_free(HlTestRunnerLoop *loop)
{
    if (!loop || !loop->ctx) return;
    hl_async_backend()->free(loop->ctx);
    loop->ctx = NULL;
}

static void test_runner_env_done(HlRuntime *rt, HlManifest *m, int have)
{
    rt->test_env_allow = NULL;
    if (have) hl_manifest_free(m);
}

int hl_test_runner_run(HlAppContext *ctx, const HlTestRunnerWriter *writer)
{
    HlRuntime *rt = hl_app_context_runtime(ctx);
    const char *app_dir = hl_app_context_app_dir(ctx);
    if (!rt || !rt->vt || !rt->vt->test_setup || !rt->vt->run_test_file)
        return -1;

    /* Test file glob comes from the vtable - keeps the runner free of
     * per-runtime knowledge. */
    const char *pattern = rt->vt->test_file_pattern;
    if (!pattern) return -1;

    /* Standalone KlHttpRouter so tests can dispatch HTTP in-process. */
    KlAllocator alloc = kl_allocator_default();
    KlHttpRouter router;
    kl_http_router_init(&router, &alloc);

    /* A server app's tests dispatch requests; a CLI app's (app.main and no
     * routes) run main with test.run_main. With neither there is nothing to
     * test. test_setup wires the routes, when there are any. */
    int has_main = rt->vt->has_main && rt->vt->has_main(rt);
    if (rt->vt->test_setup(rt, &router) != 0) {
        if (writer && writer->on_file_load_error)
            writer->on_file_load_error(writer->user, has_main
                ? "could not wire the app's routes"
                : "no routes or app.main registered");
        kl_http_router_free(&router);
        return -1;
    }

    /* test.run_main's opts.env may name only what the manifest's `env`
     * list allows (HlRuntime.test_env_allow). */
    HlManifest manifest;
    memset(&manifest, 0, sizeof manifest);
    int have_manifest = rt->vt->extract_manifest &&
                        rt->vt->extract_manifest(rt, &manifest) == 0;
    const char *env_allow[HL_MANIFEST_MAX_ENVS + 1];
    int nenv = 0;
    if (have_manifest)
        for (int i = 0; i < manifest.env_count && i < HL_MANIFEST_MAX_ENVS; i++)
            if (manifest.env[i]) env_allow[nenv++] = manifest.env[i];
    env_allow[nenv] = NULL;
    rt->test_env_allow = env_allow;

    char **test_files = hl_tool_find_files(app_dir, pattern, NULL);
    if (!test_files || !test_files[0]) {
        if (writer && writer->on_file_load_error)
            writer->on_file_load_error(writer->user, "no test files found");
        if (test_files) free(test_files);
        test_runner_env_done(rt, &manifest, have_manifest);
        kl_http_router_free(&router);
        return -1;
    }

    int grand_total = 0, grand_passed = 0, grand_failed = 0;
    /* On the heap: about 1.3 MB, more than a 1 MB main-thread stack
     * (Windows) holds. */
    HlTestCaseResult *results = calloc(HL_TEST_RUNNER_MAX_RESULTS, sizeof *results);
    if (!results) {
        for (char **fp = test_files; *fp; fp++) free(*fp);
        free(test_files);
        test_runner_env_done(rt, &manifest, have_manifest);
        kl_http_router_free(&router);
        return -1;
    }

    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        if (writer && writer->on_file_start)
            writer->on_file_start(writer->user, basename);

        int file_total = 0, file_passed = 0, file_failed = 0;
        const char *load_err = NULL;
        memset(results, 0, HL_TEST_RUNNER_MAX_RESULTS * sizeof *results);

        int rc = rt->vt->run_test_file(rt, file,
                                       results, HL_TEST_RUNNER_MAX_RESULTS,
                                       &file_total, &file_passed, &file_failed,
                                       &load_err);
        if (rc != 0) {
            if (writer && writer->on_file_load_error)
                writer->on_file_load_error(writer->user, load_err ? load_err : "unknown");
            grand_failed++;
            grand_total++;
            free(*fp);
            continue;
        }

        /* Only the stored results: a file with more cases than the array
         * holds reported file_total, and the writers read past its end. */
        int stored = file_total < HL_TEST_RUNNER_MAX_RESULTS
                   ? file_total : HL_TEST_RUNNER_MAX_RESULTS;
        if (writer && writer->on_file_end)
            writer->on_file_end(writer->user, results, stored,
                                file_total, file_passed, file_failed);

        grand_total  += file_total;
        grand_passed += file_passed;
        grand_failed += file_failed;
        free(*fp);
    }
    free(test_files);
    free(results);

    if (writer && writer->on_summary)
        writer->on_summary(writer->user, grand_total, grand_passed, grand_failed);

    test_runner_env_done(rt, &manifest, have_manifest);
    kl_http_router_free(&router);
    return grand_failed;
}
