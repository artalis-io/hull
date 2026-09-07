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
 * - it's the only place in runtime/lua/ that would otherwise need to
 * cross the sibling-runtime boundary.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MANIFEST_EXTRACT_FILE_H
#define HL_MANIFEST_EXTRACT_FILE_H

#include <stddef.h>

/**
 * @brief Load a `.js` app, JSON-stringify its declared manifest, and
 *        return a heap-allocated copy - in an ISOLATED CHILD PROCESS.
 *
 * Runs the extraction by re-execing `hull __extract-manifest-js` and
 * reading the result back from a temp file. The isolation exists because
 * a transient QuickJS runtime can abort the WHOLE PROCESS during teardown
 * on an app that leaves a self-referential pending microtask: the cycle
 * collector in `JS_FreeRuntime` double-frees the promise resolver and the
 * allocator raises SIGABRT (issue #427). In-process that kills `hull build`
 * with no diagnostic; in a child it is contained.
 *
 * The child writes its result BEFORE tearing its runtime down, so a
 * teardown abort still yields a usable manifest and the build SUCCEEDS
 * rather than merely failing cleanly.
 *
 * A child that cannot be launched at all (no resolvable `hull` path, spawn
 * refused) falls back to running in-process, which is exactly the old
 * behaviour - isolation is best-effort hardening, never a new way to fail.
 * A child that DID launch is never retried in-process: that would re-run
 * the very abort the isolation exists to contain.
 *
 * @param path         Filesystem path to the JS entry point.
 * @param hull_exe     Path to the running `hull` binary, for the re-exec
 *                     (the tool VM's `__hull_exe`). NULL is allowed and
 *                     selects the in-process fallback when the platform
 *                     cannot resolve its own path (cosmo has no /proc).
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
 * Build with `HL_ENABLE_JS=0` to drop this - the JS runtime isn't
 * linked then. Callers in CLI / build-tool flavors should check the
 * macro themselves before invoking.
 */
int hl_manifest_extract_js_from_file(const char *path,
                                       const char *hull_exe,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err);

/**
 * @brief The extraction itself, run IN THIS PROCESS.
 *
 * Same contract as hl_manifest_extract_js_from_file() minus @p hull_exe.
 * This is the half that spins up the transient HlJS, and therefore the
 * half that can abort during QuickJS teardown (issue #427).
 *
 * Two callers, both deliberate:
 *   - `hull __extract-manifest-js` (the isolated child), and
 *   - hl_manifest_extract_js_from_file()'s fallback when no child could
 *     be launched.
 * Everything else must go through hl_manifest_extract_js_from_file().
 *
 * @param early_result_path  When non-NULL, the outcome is written there via
 *        hl_manifest_extract_write_result() BEFORE the JS runtime is torn
 *        down. That ordering is the whole point: teardown is where QuickJS
 *        aborts, and by then the manifest is already safely on disk. Pass
 *        NULL when the caller consumes the return value directly.
 */
int hl_manifest_extract_js_in_process(const char *path,
                                       const char *early_result_path,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err);

/**
 * @brief Write one extraction outcome to @p out_path.
 *
 * Format is a single header line then the payload verbatim:
 *   "HULLMANIFEST1 <status>" then a newline then the payload; status is one
 *   of `start` / `ok` / `none` / `err`.
 * `start` is written by the child on entry, before anything can abort, and is
 * overwritten by the real result. It is what lets the parent tell a child that
 * never launched (no file at all) from one that launched and died mid-run -
 * a distinction the spawn exit status cannot make, since a signal death and a
 * failed fork are both -1.
 *
 * @return 0 on success; -1 on any write failure (the partial file is removed).
 */
int hl_manifest_extract_write_result(const char *out_path, const char *status,
                                     const char *payload, size_t payload_len);

#endif /* HL_MANIFEST_EXTRACT_FILE_H */
