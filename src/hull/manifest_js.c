/*
 * manifest_js.c — Extract HlManifest from QuickJS globalThis.__hull_manifest
 *
 * Split from manifest.c as part of architectural roadmap item G.
 * Compiles to an empty translation unit when HL_ENABLE_JS is not set.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "hull/alloc.h"
#include "manifest_internal.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#ifdef HL_ENABLE_JS

#include "quickjs.h"

/* Read a string array from a JS object property into a C array.
 * Strings are copied via hl_manifest_strdup; JS strings are freed immediately.
 * Returns number of strings read (capped at max). */
static int read_js_string_array(JSContext *ctx, JSValueConst obj,
                                 const char *field,
                                 const char **out, int max,
                                 HlAllocator *alloc)
{
    int count = 0;
    JSValue arr = JS_GetPropertyStr(ctx, obj, field);
    if (JS_IsUndefined(arr) || !JS_IsArray(ctx, arr)) {
        JS_FreeValue(ctx, arr);
        return 0;
    }

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < len && count < max; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        if (JS_IsString(elem)) {
            const char *s = JS_ToCString(ctx, elem);
            if (s) {
                const char *copy = hl_manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
                if (copy)
                    out[count++] = copy;
            }
        }
        JS_FreeValue(ctx, elem);
    }

    JS_FreeValue(ctx, arr);
    return count;
}

int hl_manifest_extract_js(JSContext *ctx, HlManifest *out, HlAllocator *alloc)
{
    if (!ctx || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->alloc = alloc;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue manifest = JS_GetPropertyStr(ctx, global, "__hull_manifest");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(manifest) || JS_IsNull(manifest)) {
        JS_FreeValue(ctx, manifest);
        return -1; /* no manifest declared */
    }

    out->present = 1;

    /* fs = { read: [...], write: [...] } */
    JSValue fs = JS_GetPropertyStr(ctx, manifest, "fs");
    if (!JS_IsUndefined(fs) && !JS_IsNull(fs)) {
        out->fs_read_count = read_js_string_array(ctx, fs, "read",
                                                    out->fs_read,
                                                    HL_MANIFEST_MAX_PATHS, alloc);
        out->fs_write_count = read_js_string_array(ctx, fs, "write",
                                                     out->fs_write,
                                                     HL_MANIFEST_MAX_PATHS, alloc);
    }
    JS_FreeValue(ctx, fs);

    /* env = [...] */
    out->env_count = read_js_string_array(ctx, manifest, "env",
                                            out->env,
                                            HL_MANIFEST_MAX_ENVS, alloc);

    /* hosts = [...] */
    out->hosts_count = read_js_string_array(ctx, manifest, "hosts",
                                              out->hosts,
                                              HL_MANIFEST_MAX_HOSTS, alloc);

    /* csp = "policy-string" or false */
    JSValue csp_val = JS_GetPropertyStr(ctx, manifest, "csp");
    if (JS_IsString(csp_val)) {
        const char *csp_str = JS_ToCString(ctx, csp_val);
        if (csp_str && hl_manifest_csp_is_valid(csp_str)) {
            out->csp = hl_manifest_strdup(alloc, csp_str);
            out->csp_set = 1;
        }
        if (csp_str) JS_FreeCString(ctx, csp_str);
    } else if (JS_IsBool(csp_val) && !JS_ToBool(ctx, csp_val)) {
        out->csp = NULL;
        out->csp_set = 1;  /* explicitly disabled */
    }
    JS_FreeValue(ctx, csp_val);

    /* cors = { origins: [...], methods: "...", headers: "...",
     *          credentials: true, maxAge: 86400 } */
    JSValue cors_val = JS_GetPropertyStr(ctx, manifest, "cors");
    if (!JS_IsUndefined(cors_val) && !JS_IsNull(cors_val)) {
        out->cors_set = 1;
        out->cors_origin_count = read_js_string_array(ctx, cors_val, "origins",
                                                        out->cors_origins,
                                                        HL_MANIFEST_MAX_CORS_ORIGINS,
                                                        alloc);

        JSValue methods_val = JS_GetPropertyStr(ctx, cors_val, "methods");
        if (JS_IsString(methods_val)) {
            const char *s = JS_ToCString(ctx, methods_val);
            if (s) {
                out->cors_methods = hl_manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, methods_val);

        JSValue headers_val = JS_GetPropertyStr(ctx, cors_val, "headers");
        if (JS_IsString(headers_val)) {
            const char *s = JS_ToCString(ctx, headers_val);
            if (s) {
                out->cors_headers = hl_manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, headers_val);

        JSValue creds_val = JS_GetPropertyStr(ctx, cors_val, "credentials");
        if (JS_IsBool(creds_val))
            out->cors_credentials = JS_ToBool(ctx, creds_val);
        JS_FreeValue(ctx, creds_val);

        JSValue age_val = JS_GetPropertyStr(ctx, cors_val, "maxAge");
        if (JS_IsNumber(age_val)) {
            int32_t age = 0;
            JS_ToInt32(ctx, &age, age_val);
            out->cors_max_age = age;
        }
        JS_FreeValue(ctx, age_val);
    }
    JS_FreeValue(ctx, cors_val);

    /* wasm: { heap, stack, gas, maxInput, maxOutput } */
    JSValue wasm_val = JS_GetPropertyStr(ctx, manifest, "wasm");
    if (!JS_IsUndefined(wasm_val) && !JS_IsNull(wasm_val)) {
        JSValue v;
        int64_t iv;
        v = JS_GetPropertyStr(ctx, wasm_val, "heap");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_heap = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "stack");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_stack = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "gas");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_gas = iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "maxInput");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_max_input = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "maxOutput");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_max_output = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, wasm_val);

    /* gpu: true  OR  gpu: { devices: [0, 1] } */
    JSValue gpu_val = JS_GetPropertyStr(ctx, manifest, "gpu");
    if (JS_IsObject(gpu_val) && !JS_IsNull(gpu_val)) {
        out->gpu = 1;
        JSValue devs = JS_GetPropertyStr(ctx, gpu_val, "devices");
        if (JS_IsArray(ctx, devs)) {
            JSValue len_val = JS_GetPropertyStr(ctx, devs, "length");
            int32_t len = 0;
            JS_ToInt32(ctx, &len, len_val);
            JS_FreeValue(ctx, len_val);
            for (int32_t i = 0; i < len && out->gpu_device_count < HL_GPU_MAX_DEVICES; i++) {
                JSValue elem = JS_GetPropertyUint32(ctx, devs, (uint32_t)i);
                if (JS_IsNumber(elem)) {
                    int32_t d = 0;
                    JS_ToInt32(ctx, &d, elem);
                    out->gpu_devices[out->gpu_device_count++] = d;
                }
                JS_FreeValue(ctx, elem);
            }
        }
        JS_FreeValue(ctx, devs);
    } else if (JS_IsBool(gpu_val)) {
        out->gpu = JS_ToBool(ctx, gpu_val);
    }
    JS_FreeValue(ctx, gpu_val);

    /* compute: true */
    JSValue compute_val = JS_GetPropertyStr(ctx, manifest, "compute");
    if (JS_IsBool(compute_val))
        out->compute = JS_ToBool(ctx, compute_val);
    JS_FreeValue(ctx, compute_val);

    /* tui: true */
    JSValue tui_val = JS_GetPropertyStr(ctx, manifest, "tui");
    if (JS_IsBool(tui_val))
        out->tui = JS_ToBool(ctx, tui_val);
    JS_FreeValue(ctx, tui_val);

    /* modules: ["hull/crypto@1", "hull/db@1", ...] — an array of
     * canonical spec strings. The local variable / imported identifier
     * is a plain JS binding (the user chooses the name); the manifest
     * only declares which canonical modules are in scope.
     *
     * For back-compat we also accept the legacy object form when the
     * value is a non-array object (`{ crypto: "hull/crypto@1" }`) —
     * keys are ignored as cosmetic labels.
     *
     * Presence of `modules` (array OR object, even empty) sets
     * `modules_declared = 1`. */
    JSValue modules_val = JS_GetPropertyStr(ctx, manifest, "modules");

    if (JS_IsArray(ctx, modules_val)) {
        out->modules_declared = 1;
        JSValue len_val = JS_GetPropertyStr(ctx, modules_val, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        if (len > HL_MANIFEST_MAX_MODULES)
            log_warn("[manifest] modules array exceeds "
                     "HL_MANIFEST_MAX_MODULES (%d), truncated",
                     HL_MANIFEST_MAX_MODULES);
        for (int32_t i = 0;
             i < len && out->modules_count < HL_MANIFEST_MAX_MODULES;
             i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, modules_val, (uint32_t)i);
            if (JS_IsString(elem)) {
                const char *spec = JS_ToCString(ctx, elem);
                if (spec) {
                    const char *at = strchr(spec, '@');
                    if (at && at != spec) {
                        char *end = NULL;
                        long v = strtol(at + 1, &end, 10);
                        if (end != at + 1 && *end == '\0' &&
                            v >= 1 && v <= 255) {
                            size_t nlen = (size_t)(at - spec);
                            char *namebuf = hl_alloc_malloc(alloc, nlen + 1);
                            if (namebuf) {
                                memcpy(namebuf, spec, nlen);
                                namebuf[nlen] = '\0';
                                out->modules[out->modules_count].name      = namebuf;
                                out->modules[out->modules_count].api_major = (uint8_t)v;
                                out->modules_count++;
                            }
                        } else {
                            log_warn("[manifest] modules[%d] = %s — invalid "
                                     "major version, ignored", (int)i, spec);
                        }
                    } else {
                        log_warn("[manifest] modules[%d] = %s — expected "
                                 "\"vendor/name@version\", ignored",
                                 (int)i, spec);
                    }
                    JS_FreeCString(ctx, spec);
                }
            }
            JS_FreeValue(ctx, elem);
        }
    } else if (JS_IsObject(modules_val) && !JS_IsNull(modules_val)) {
        out->modules_declared = 1;
        JSPropertyEnum *props = NULL;
        uint32_t prop_count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, modules_val,
                                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < prop_count; i++) {
                if (out->modules_count >= HL_MANIFEST_MAX_MODULES) {
                    log_warn("[manifest] modules object exceeds HL_MANIFEST_MAX_MODULES (%d), truncated",
                             HL_MANIFEST_MAX_MODULES);
                    for (uint32_t j = i; j < prop_count; j++)
                        JS_FreeAtom(ctx, props[j].atom);
                    break;
                }
                const char *alias = JS_AtomToCString(ctx, props[i].atom);
                JSValue v_val = JS_GetProperty(ctx, modules_val, props[i].atom);
                if (alias && JS_IsString(v_val)) {
                    const char *spec = JS_ToCString(ctx, v_val);
                    if (spec) {
                        const char *at = strchr(spec, '@');
                        if (!at || at == spec) {
                            log_warn("[manifest] modules.%s = %s — expected "
                                     "\"vendor/name@version\", ignored",
                                     alias, spec);
                        } else {
                            char *end = NULL;
                            long v = strtol(at + 1, &end, 10);
                            if (end == at + 1 || *end != '\0' ||
                                v < 1 || v > 255) {
                                log_warn("[manifest] modules.%s = %s — invalid "
                                         "major version, ignored", alias, spec);
                            } else {
                                size_t nlen = (size_t)(at - spec);
                                char *namebuf = hl_alloc_malloc(alloc, nlen + 1);
                                if (namebuf) {
                                    memcpy(namebuf, spec, nlen);
                                    namebuf[nlen] = '\0';
                                    out->modules[out->modules_count].name      = namebuf;
                                    out->modules[out->modules_count].api_major = (uint8_t)v;
                                    out->modules_count++;
                                }
                            }
                        }
                        JS_FreeCString(ctx, spec);
                    }
                }
                if (alias) JS_FreeCString(ctx, alias);
                JS_FreeValue(ctx, v_val);
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }
    JS_FreeValue(ctx, modules_val);

    /* allowDynamicCode: true — opt-in to JIT / runtime codegen.
     * Rejected by hl_sandbox_apply unless --no-sandbox.
     * Also accept the snake_case form for parity with the Lua manifest. */
    JSValue adc_val = JS_GetPropertyStr(ctx, manifest, "allowDynamicCode");
    if (JS_IsUndefined(adc_val)) {
        JS_FreeValue(ctx, adc_val);
        adc_val = JS_GetPropertyStr(ctx, manifest, "allow_dynamic_code");
    }
    if (JS_IsBool(adc_val))
        out->allow_dynamic_code = JS_ToBool(ctx, adc_val);
    JS_FreeValue(ctx, adc_val);
    if (out->allow_dynamic_code)
        log_warn("[manifest] allowDynamicCode=true — kernel sandbox "
                 "will fail closed unless --no-sandbox is set");

    /* allowDynamicLibraries: true — opt-in to dlopen() of native libs. */
    JSValue adl_val = JS_GetPropertyStr(ctx, manifest, "allowDynamicLibraries");
    if (JS_IsUndefined(adl_val)) {
        JS_FreeValue(ctx, adl_val);
        adl_val = JS_GetPropertyStr(ctx, manifest, "allow_dynamic_libraries");
    }
    if (JS_IsBool(adl_val))
        out->allow_dynamic_libraries = JS_ToBool(ctx, adl_val);
    JS_FreeValue(ctx, adl_val);
    if (out->allow_dynamic_libraries)
        log_warn("[manifest] allowDynamicLibraries=true — kernel sandbox "
                 "will fail closed unless --no-sandbox is set");

    JS_FreeValue(ctx, manifest);
    return 0;
}

#endif /* HL_ENABLE_JS */
