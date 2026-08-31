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

#include <keel/allocator.h>
#include <keel/http_router.h>

#include <stdlib.h>
#include <string.h>

#define HL_TEST_RUNNER_MAX_RESULTS 1024

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

    if (rt->vt->test_setup(rt, &router) != 0) {
        if (writer && writer->on_file_load_error)
            writer->on_file_load_error(writer->user, "no routes registered");
        kl_http_router_free(&router);
        return -1;
    }

    char **test_files = hl_tool_find_files(app_dir, pattern, NULL);
    if (!test_files || !test_files[0]) {
        if (writer && writer->on_file_load_error)
            writer->on_file_load_error(writer->user, "no test files found");
        if (test_files) free(test_files);
        kl_http_router_free(&router);
        return -1;
    }

    int grand_total = 0, grand_passed = 0, grand_failed = 0;
    HlTestCaseResult results[HL_TEST_RUNNER_MAX_RESULTS];

    for (char **fp = test_files; *fp; fp++) {
        const char *file = *fp;
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;

        if (writer && writer->on_file_start)
            writer->on_file_start(writer->user, basename);

        int file_total = 0, file_passed = 0, file_failed = 0;
        const char *load_err = NULL;
        memset(results, 0, sizeof(results));

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

        if (writer && writer->on_file_end)
            writer->on_file_end(writer->user, results, file_total,
                                file_total, file_passed, file_failed);

        grand_total  += file_total;
        grand_passed += file_passed;
        grand_failed += file_failed;
        free(*fp);
    }
    free(test_files);

    if (writer && writer->on_summary)
        writer->on_summary(writer->user, grand_total, grand_passed, grand_failed);

    kl_http_router_free(&router);
    return grand_failed;
}
