/*
 * test_runner.h - Shared test-discovery + per-file orchestration
 *
 * Used by `hull test` (commands/test.c) and `hull agent test`
 * (agent_lib.c). Each call site provides a writer; the runner handles:
 *   - test-file discovery (test_*.{lua,js} in app_dir)
 *   - route wiring + test global registration (via HlRuntimeVtable)
 *   - per-file load + run + clear loop (via HlRuntimeVtable::run_test_file)
 *
 * Folds 4 sibling functions (Lua/JS × stdout/JSON) into one orchestrator
 * + 2 thin writers (item I step 2; H1 + H2 of the architecture roadmap).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_TEST_RUNNER_H
#define HL_TEST_RUNNER_H

#include <stddef.h>

/* Forward declarations */
typedef struct HlAppContext     HlAppContext;
typedef struct HlTestCaseResult HlTestCaseResult;

/*
 * Writer interface - call site fills in callbacks to emit output.
 * Any callback may be NULL to skip that event.
 */
typedef struct HlTestRunnerWriter {
    void *user;

    /* Called before each test file starts. */
    void (*on_file_start)(void *user, const char *file_basename);

    /* Called if the file failed to load (load error, syntax error, …).
     * `err` is a borrowed pointer (valid until the next runner call). */
    void (*on_file_load_error)(void *user, const char *err);

    /* Called after each file completes. results[0..count) is the per-case
     * detail; file_total/passed/failed are the file-level summary. */
    void (*on_file_end)(void *user,
                        const HlTestCaseResult *results, int count,
                        int file_total, int file_passed, int file_failed);

    /* Called once after every file has run. NULL == no summary. */
    void (*on_summary)(void *user,
                       int grand_total, int grand_passed, int grand_failed);
} HlTestRunnerWriter;

/*
 * Run all test files in `ctx`'s app directory matching the runtime's
 * convention (test_*.lua or test_*.js). Calls writer callbacks for
 * progress / output.
 *
 * Returns the grand-total fail count (0 = all pass).
 * Returns negative on setup failure (no routes registered, no test
 * files found, etc.) and emits a load_error to the writer.
 *
 * Note: ctx must already be loaded (app code executed) - the runner
 * uses ctx's HlRuntime to drive route + test_global wiring.
 */
int hl_test_runner_run(HlAppContext *ctx, const HlTestRunnerWriter *writer);

/*
 * The test run's event loop and worker pool, so the code a test drives -
 * app.main under test.run_main / runMain, a JS async test body - can wait
 * on hull.sleep, http.fetch, db.async, compute.async. An entry point that
 * makes its own app context for the run (hull test, hull agent test):
 *
 *   HlTestRunnerLoop loop;
 *   hl_test_runner_loop_init(&loop);         before hl_app_context_init,
 *   opts.thread_pool = loop.pool;            so hull.worker registers
 *   ... hl_app_context_init(&ctx, &opts) ...
 *   hl_test_runner_loop_attach(&loop, rt);
 *   ... hl_test_runner_run(ctx, &writer) ...
 *   hl_test_runner_loop_detach(&loop, rt);   pool, then the runtime's
 *                                            pending ops, before the context
 *   hl_app_context_free(ctx);
 *   hl_test_runner_loop_free(&loop);         the loop, after the context
 *
 * Without one (the MCP warm context) a test's wait is refused, as before.
 * init returns 0, or -1 with nothing to free (the run goes on without a
 * loop); the pool is NULL when it could not be made.
 */
struct HlAsyncBackendCtx;
struct HlAsyncBackendPool;
struct HlRuntime;
typedef struct HlTestRunnerLoop {
    struct HlAsyncBackendCtx  *ctx;
    struct HlAsyncBackendPool *pool;
} HlTestRunnerLoop;

int  hl_test_runner_loop_init(HlTestRunnerLoop *loop);
void hl_test_runner_loop_attach(HlTestRunnerLoop *loop, struct HlRuntime *rt);
void hl_test_runner_loop_detach(HlTestRunnerLoop *loop, struct HlRuntime *rt);
void hl_test_runner_loop_free(HlTestRunnerLoop *loop);

#endif /* HL_TEST_RUNNER_H */
