/*
 * manifest_extract_file.c - runtime-neutral helper that spins up a
 * transient JS runtime to read app.manifest({...}) from a `.js` file.
 *
 * See include/hull/manifest_extract_file.h for the rationale + the
 * caller-side contract. The only consumer today is
 * src/hull/runtime/lua/mod_tool.c (`tool.extract_manifest_js`); this
 * TU isolates the `hull/runtime/js.h` include so the Lua tool binding
 * doesn't have to cross the sibling-runtime boundary itself.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest_extract_file.h"

#include <stdlib.h>
#include <string.h>

/* Copy a NUL-terminated string onto the heap (malloc'd). NULL-safe.
 * Used by both the JS-enabled and JS-disabled paths below - keep it
 * above the HL_ENABLE_JS gate. */
static char *strdup_safe(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

#ifdef HL_ENABLE_JS

#include "hull/runtime/js.h"
#include "hull/stdlib_feature.h"   /* hl_platform_vfs_init / _dispose */
#include "hull/vfs.h"              /* HlVfs */
#include "quickjs.h"

int hl_manifest_extract_js_from_file(const char *path,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err)
{
    if (out_json)     *out_json = NULL;
    if (out_json_len) *out_json_len = 0;
    if (out_err)      *out_err = NULL;
    if (!path) {
        if (out_err) *out_err = strdup_safe("null path");
        return -1;
    }

    HlJS *js = calloc(1, sizeof(*js));
    if (!js) {
        if (out_err) *out_err = strdup_safe("out of memory");
        return -1;
    }

    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    if (hl_js_init(js, &cfg) != 0) {
        free(js);
        if (out_err) *out_err = strdup_safe("hl_js_init failed");
        return -1;
    }

    /* Give the transient extractor the SAME composed platform VFS the real app
     * runtime gets. Without it js->base.platform_vfs is NULL, so every stdlib
     * `hull:*` import (hull:web:attachment-serve, hull:json, ...) misses the VFS
     * lookup in hl_js_module_loader and falls to the manifest-extract lenient
     * stub below - and an app that imports a NAMED export from such a module then
     * dies at link with "Could not find export X", failing extraction. Composing
     * it lets stdlib modules resolve for real; a genuine feature-only module
     * (hull:tui, not in the base VFS) still hits the stub. Disposed in cleanup. */
    HlVfs pvfs;
    void *pvfs_owned = NULL;
    hl_platform_vfs_init(&pvfs, &pvfs_owned);
    js->base.platform_vfs = &pvfs;

    /* Only reading app.manifest(): tolerate an unresolvable `hull:*` import (a
     * feature-module stdlib file that rides the composed feature, e.g. hull:tui)
     * so extraction succeeds instead of failing and forcing callers onto their
     * fail-safe path (issue #114). */
    js->manifest_extract_lenient = 1;

    /* Single cleanup path (goto) so the composed platform VFS is disposed on
     * every exit without repeating it at each error site. */
    int    rc       = 0;
    char  *copy     = NULL;
    size_t json_len = 0;

    /* Run the app; a top-level throw / rejected import now fails the load
     * (hl_js_load_app observes the rejected module eval promise). Do NOT bail
     * on that yet: app.manifest() runs synchronously before any throw, so the
     * authoritative manifest may already be captured. */
    int load_rc = hl_js_load_app(js, path);

    {
        JSContext *ctx = js->ctx;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue manifest = JS_GetPropertyStr(ctx, global, "__hull_manifest");
        JS_FreeValue(ctx, global);

        int have_manifest = !(JS_IsException(manifest) ||
                              JS_IsUndefined(manifest) || JS_IsNull(manifest));

        /* Capture-then-tolerate, parity with the Lua extractor: if app.manifest
         * was called we HAVE the authoritative composition info and use it even
         * though the module later threw. If NO manifest was captured, a load
         * FAILURE (throw/unresolved import BEFORE app.manifest) is a real
         * extraction failure - fatal, since extraction drives composition; a
         * SUCCESSFUL run with no app.manifest() is a valid manifest-less app. */
        if (!have_manifest) {
            JS_FreeValue(ctx, manifest);
            if (load_rc != 0) {
                if (out_err) *out_err = strdup_safe(
                    "failed to load app (syntax error, throw at top-level, "
                    "or unresolved import?)");
                rc = -1;
            }
            goto cleanup;   /* rc stays 0 for a valid manifest-less app */
        }

        JSValue json = JS_JSONStringify(ctx, manifest, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(ctx, manifest);
        if (JS_IsException(json)) {
            JS_FreeValue(ctx, json);
            if (out_err) *out_err = strdup_safe("JSON.stringify(manifest) failed");
            rc = -1;
            goto cleanup;
        }

        const char *json_str = JS_ToCStringLen(ctx, &json_len, json);
        if (!json_str) {
            JS_FreeValue(ctx, json);
            if (out_err) *out_err = strdup_safe(
                "cannot convert JSON value to string");
            rc = -1;
            goto cleanup;
        }

        /* Heap-copy the JSON BEFORE tearing down the JS runtime - the
         * cstring lifetime ends with JS_FreeCString. */
        copy = malloc(json_len + 1);
        if (!copy) {
            JS_FreeCString(ctx, json_str);
            JS_FreeValue(ctx, json);
            if (out_err) *out_err = strdup_safe("out of memory");
            json_len = 0;
            rc = -1;
            goto cleanup;
        }
        memcpy(copy, json_str, json_len);
        copy[json_len] = '\0';

        JS_FreeCString(ctx, json_str);
        JS_FreeValue(ctx, json);
    }

cleanup:
    hl_js_free(js);
    free(js);
    hl_platform_vfs_dispose(pvfs_owned);

    if (rc == 0 && copy && out_json) {
        *out_json = copy;
    } else {
        /* Error, no-manifest, or caller doesn't want the buffer - don't leak. */
        free(copy);
    }
    if (out_json_len) *out_json_len = json_len;
    return rc;
}

#else /* HL_ENABLE_JS */

int hl_manifest_extract_js_from_file(const char *path,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err)
{
    (void)path;
    if (out_json)     *out_json = NULL;
    if (out_json_len) *out_json_len = 0;
    if (out_err)      *out_err = strdup_safe(
        "this hull was built without HL_ENABLE_JS");
    return -1;
}

#endif /* HL_ENABLE_JS */
