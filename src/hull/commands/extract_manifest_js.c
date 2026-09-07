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

#include <stdio.h>
#include <stdlib.h>

int hl_cmd_extract_manifest_js(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    if (argc < 4) {
        fprintf(stderr,
                "usage: hull __extract-manifest-js <entry.js> <result-file>\n"
                "  (internal - used by hull build to isolate JS manifest "
                "extraction)\n");
        return 2;
    }

    const char *path        = argv[2];
    const char *result_path = argv[3];

    char  *json = NULL;
    char  *err  = NULL;
    size_t len  = 0;

    /* Writes the result itself, before teardown. Its return value only tells
     * us the exit code to use; the parent reads the file either way. */
    int rc = hl_manifest_extract_js_in_process(path, result_path,
                                               &json, &len, &err);
    free(json);
    free(err);
    return rc == 0 ? 0 : 1;
}
