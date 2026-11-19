/*
 * extract_manifest_js.c - `hull __extract-manifest-js <app.js> <result-file>`
 *
 * INTERNAL. The isolated child half of JS manifest extraction (issue #427).
 *
 * Reading `app.manifest({...})` out of a `.js` entry point means running the
 * app's top level in a transient QuickJS runtime. Tearing that runtime down
 * can abort the process: on an app that leaves a self-referential pending
 * microtask, JS_FreeRuntime's cycle collector double-frees the promise
 * resolver and the allocator raises SIGABRT. In `hull build` that killed the
 * build outright with no diagnostic. Here it kills only this child.
 *
 * The outcome is written to <result-file> BEFORE the runtime is torn down
 * (hl_manifest_extract_js_in_process's early_result_path), so even an aborted
 * teardown leaves the parent a complete result and the build succeeds.
 *
 * Deliberately silent on stdout: the app's own top-level `console.log` shares
 * this process's stdout, and mixing the two would corrupt the result. The
 * file is the only channel.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/extract_manifest_js.h"
#include "hull/manifest_extract_file.h"
#include "hull/runtime/cache_common.h"   /* hl_runtime_cache_code_off */
#include "hull/shared/generation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hl_cmd_extract_manifest_js(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    /* argv is rebased at the VERB by hl_command_dispatch (it passes
     * argc - cmd_idx, argv + cmd_idx), so argv[0] is "__extract-manifest-js"
     * and the two operands are argv[1] / argv[2]. Same convention as every
     * other handler, which all start their scan at i = 1. */
    if (argc < 3) {
        fprintf(stderr,
                "usage: hull __extract-manifest-js <entry.js> <result-file> [<generation fd>]\n"
                "  (internal - used by hull build to isolate JS manifest "
                "extraction)\n");
        return 2;
    }

    const char *path        = argv[1];
    const char *result_path = argv[2];
    /* `hull build` of an app with pipeline work hands the generation it
     * embeds on the descriptor named by the third operand
     * (hl_manifest_extract_js_with_generation). */
    int gen_fd = -1;
    if (argc >= 4) {
        char *end = NULL;
        long l = strtol(argv[3], &end, 10);
        if (!end || *end != '\0' || end == argv[3] || l < 3 || l > 65535) {
            fprintf(stderr, "hull __extract-manifest-js: bad descriptor %s\n",
                    argv[3]);
            return 2;
        }
        gen_fd = (int)l;
    }

    /* Its manifest is what hull build signs: no stdlib bytecode from the
     * runtime-key caches an app process can write (audit 22). */
    hl_runtime_cache_code_off(1);

    /* Claim the result file BEFORE running anything. The parent cannot tell a
     * child that never launched from one that launched and died: hl_tool_spawn
     * collapses both to -1 (spawn_and_wait returns WEXITSTATUS or -1, and a
     * SIGABRT child is !WIFEXITED). This marker is that distinction. Without
     * it the parent would fall back to IN-PROCESS extraction after an abort,
     * re-running the very crash the isolation exists to contain.
     *
     * It is overwritten by the real result moments later; it only has to
     * survive the window in which the runtime can abort. */
    if (hl_manifest_extract_write_result(result_path, "start", NULL, 0) != 0) {
        fprintf(stderr, "hull __extract-manifest-js: cannot write %s\n",
                result_path);
        return 1;
    }

    char  *json = NULL;
    char  *err  = NULL;
    size_t len  = 0;

    /* The overlay is installed before the entry runs: a TypeScript entry and
     * its .ts imports load from the generation, never from disk. */
    if (gen_fd >= 0 &&
        (hl_generation_take_fd(gen_fd) < 0 || hl_generation_apply(0) != 0)) {
        const char *why = hl_generation_error();
        (void)hl_manifest_extract_write_result(result_path, "err", why,
                                               strlen(why));
        return 1;
    }

    /* Writes the result itself, before teardown. Its return value only tells
     * us the exit code to use; the parent reads the file either way. */
    int rc = hl_manifest_extract_js_in_process(path, result_path,
                                               &json, &len, &err);
    free(json);
    free(err);
    return rc == 0 ? 0 : 1;
}
