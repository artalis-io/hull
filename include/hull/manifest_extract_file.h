/**
 * @file manifest_extract_file.h
 * @brief Cross-runtime helper: extract an app's manifest from a file
 *        on disk and return it as a JSON-encoded string.
 *
 * The Lua-side build tool (build.lua / manifest.lua / inspect.lua)
 * needs to read the manifest declared in `app.manifest({...})` regardless
 * of whether the entry-point is `app.lua` or `app.js`. The Lua tool VM
 * can execute Lua entries in-place, but `.js` apps need a transient
 * JS runtime + `hl_manifest_extract_js` to read.
 *
 * Wrapping that JS-side spin-up in a runtime-neutral function keeps the
 * runtime/lua/ binding layer (mod_tool.c) free of `hull/runtime/js.h`
 * — it's the only place in runtime/lua/ that would otherwise need to
 * cross the sibling-runtime boundary.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MANIFEST_EXTRACT_FILE_H
#define HL_MANIFEST_EXTRACT_FILE_H

#include <stddef.h>

/**
 * @brief Load a `.js` app, JSON-stringify its declared manifest, and
 *        return a heap-allocated copy.
 *
 * Spins up a transient HlJS, runs the file (which triggers the app's
 * `app.manifest({...})` call), reads `globalThis.__hull_manifest`, and
 * serializes it via `JSON.stringify`. Tears the runtime down on every
 * exit path.
 *
 * @param path         Filesystem path to the JS entry point.
 * @param out_json     On success: receives a malloc'd UTF-8 JSON string.
 *                     Caller frees with `free()`. On "no manifest declared"
 *                     receives NULL with no allocation. On error, undefined.
 * @param out_json_len Optional: receives the byte length of the returned
 *                     string (NULL ok if not needed).
 * @param out_err      Optional: receives a malloc'd error message on failure
 *                     (caller frees). NULL on success.
 *
 * @return 0 on success (including "no manifest"; check *out_json for NULL).
 *         -1 on failure (runtime init / file load / stringify error).
 *
 * Build with `HL_ENABLE_JS=0` to drop this — the JS runtime isn't
 * linked then. Callers in CLI / build-tool flavors should check the
 * macro themselves before invoking.
 */
int hl_manifest_extract_js_from_file(const char *path,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err);

#endif /* HL_MANIFEST_EXTRACT_FILE_H */
