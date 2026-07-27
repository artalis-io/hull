/*
 * manifest_extract_file.c — runtime-neutral helper that spins up a
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
 * Used by both the JS-enabled and JS-disabled paths below — keep it
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

    /* Only reading app.manifest(): tolerate an unresolvable `hull:*` import (a
     * feature-module stdlib file that rides the composed feature, e.g. hull:tui)
     * so extraction succeeds instead of failing and forcing callers onto their
     * fail-safe path (issue #114). */
    js->manifest_extract_lenient = 1;

    if (hl_js_load_app(js, path) != 0) {
        hl_js_free(js);
        free(js);
        if (out_err) *out_err = strdup_safe(
            "failed to load app (syntax error, throw at top-level, "
            "or unresolved import?)");
        return -1;
    }

    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue manifest = JS_GetPropertyStr(ctx, global, "__hull_manifest");
    JS_FreeValue(ctx, global);

    /* Treat exception, undefined, and null all as "no manifest
     * declared" — leave *out_json NULL and return success. */
    if (JS_IsException(manifest) || JS_IsUndefined(manifest) || JS_IsNull(manifest)) {
        JS_FreeValue(ctx, manifest);
        hl_js_free(js);
        free(js);
        return 0;
    }

    JSValue json = JS_JSONStringify(ctx, manifest, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, manifest);
    if (JS_IsException(json)) {
        JS_FreeValue(ctx, json);
        hl_js_free(js);
        free(js);
        if (out_err) *out_err = strdup_safe("JSON.stringify(manifest) failed");
        return -1;
    }

    size_t json_len = 0;
    const char *json_str = JS_ToCStringLen(ctx, &json_len, json);
    if (!json_str) {
        JS_FreeValue(ctx, json);
        hl_js_free(js);
        free(js);
        if (out_err) *out_err = strdup_safe(
            "cannot convert JSON value to string");
        return -1;
    }

    /* Heap-copy the JSON BEFORE tearing down the JS runtime — the
     * cstring lifetime ends with JS_FreeCString. */
    char *copy = malloc(json_len + 1);
    if (!copy) {
        JS_FreeCString(ctx, json_str);
        JS_FreeValue(ctx, json);
        hl_js_free(js);
        free(js);
        if (out_err) *out_err = strdup_safe("out of memory");
        return -1;
    }
    memcpy(copy, json_str, json_len);
    copy[json_len] = '\0';

    JS_FreeCString(ctx, json_str);
    JS_FreeValue(ctx, json);
    hl_js_free(js);
    free(js);

    if (out_json) {
        *out_json = copy;
    } else {
        /* Caller doesn't want the buffer — don't leak it. */
        free(copy);
    }
    if (out_json_len) *out_json_len = json_len;
    return 0;
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
