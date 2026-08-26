/*
 * agent/test.c — `hull agent test` — runs tests with JSON output.
 *
 * Uses the shared test_runner with a JSON writer (item I step 2).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/app_context.h"
#include "hull/cap/test.h"
#include "hull/runtime.h"
#include "hull/test_runner.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif

#include <sh_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* ── hl_agent_test ─────────────────────────────────────────────────── */

/* Single-path test runner via HlRuntimeVtable::test_setup + run_test_file
 * and the shared hl_test_runner_run orchestrator. Folds the previous
 * agent_test_lua_ctx + agent_test_js_ctx (~80 lines × 2) into ~70 lines
 * of JSON visitor + shared runner invocation. */

typedef struct {
    ShJsonWriter *w;
    int file_active;
    int grand_total, grand_passed, grand_failed;
} JsonTestWriterState;

static void json_test_on_file_start(void *user, const char *basename)
{
    JsonTestWriterState *s = user;
    sh_json_write_object_start(s->w);
    sh_json_write_kv_string(s->w, "name", basename);
    s->file_active = 1;
}

static void json_test_on_file_load_error(void *user, const char *err)
{
    JsonTestWriterState *s = user;
    if (!s->file_active) {
        sh_json_write_object_start(s->w);
        sh_json_write_kv_string(s->w, "name", "");
    }
    sh_json_write_kv_string(s->w, "error", err ? err : "unknown");
    sh_json_write_key(s->w, "tests");
    sh_json_write_array_start(s->w);
    sh_json_write_array_end(s->w);
    sh_json_write_object_end(s->w);
    s->file_active = 0;
}

static void json_test_on_file_end(void *user,
                                  const HlTestCaseResult *results, int count,
                                  int file_total, int file_passed, int file_failed)
{
    JsonTestWriterState *s = user;
    (void)file_total; (void)file_passed; (void)file_failed;
    sh_json_write_key(s->w, "tests");
    sh_json_write_array_start(s->w);
    for (int i = 0; i < count; i++) {
        const HlTestCaseResult *r = &results[i];
        sh_json_write_object_start(s->w);
        sh_json_write_kv_string(s->w, "name",   r->name);
        sh_json_write_kv_string(s->w, "status", r->passed ? "pass" : "fail");
        if (!r->passed && r->error[0])
            sh_json_write_kv_string(s->w, "error", r->error);
        sh_json_write_object_end(s->w);
    }
    sh_json_write_array_end(s->w);
    sh_json_write_object_end(s->w);
    s->file_active = 0;
}

static void json_test_on_summary(void *user, int total, int passed, int failed)
{
    JsonTestWriterState *s = user;
    s->grand_total  = total;
    s->grand_passed = passed;
    s->grand_failed = failed;
}

int hl_agent_test_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (rt && rt->vt && rt->vt->name)
        sh_json_write_kv_string(&w, "runtime",
            rt->vt->kind == HL_RT_LUA ? "lua" :
            rt->vt->kind == HL_RT_JS  ? "js"  : rt->vt->name);

    sh_json_write_key(&w, "files");
    sh_json_write_array_start(&w);

    JsonTestWriterState state = { .w = &w, .file_active = 0 };
    HlTestRunnerWriter writer = {
        .user                = &state,
        .on_file_start       = json_test_on_file_start,
        .on_file_load_error  = json_test_on_file_load_error,
        .on_file_end         = json_test_on_file_end,
        .on_summary          = json_test_on_summary,
    };
    int rc = hl_test_runner_run(ctx, &writer);

    sh_json_write_array_end(&w);
    sh_json_write_kv_int(&w, "total",  state.grand_total);
    sh_json_write_kv_int(&w, "passed", state.grand_passed);
    sh_json_write_kv_int(&w, "failed", state.grand_failed);
    sh_json_write_object_end(&w);

    /* rc<0 = setup failure (already emitted to writer); rc>=0 = fail count. */
    return (rc < 0 || state.grand_failed > 0) ? 1 : 0;
}
int hl_agent_test(const char *app_dir, ShJsonBuf *out)
{
    char entry_buf[4096];
    const char *entry = NULL;

#ifdef HL_ENABLE_LUA
    entry = hl_agent_detect_entry(app_dir, "lua", entry_buf, sizeof(entry_buf));
#endif
#ifdef HL_ENABLE_JS
    if (!entry)
        entry = hl_agent_detect_entry(app_dir, "js", entry_buf, sizeof(entry_buf));
#endif

    // cppcheck-suppress knownConditionTrueFalse
    if (!entry)
        return hl_agent_write_error(out, "no entry point found");

    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = { .app_dir = app_dir, .entry_point = entry };
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "runtime init failed");

    int rc = hl_agent_test_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}

/* ── hl_agent_context ─────────────────────────────────────── */
