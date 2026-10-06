/*
 * manifest_js.c - Extract HlManifest from the manifest app.manifest() stored
 * (HlJS.manifest; globalThis.__hull_manifest is a read-only view of it)
 *
 * Split from manifest.c as part of architectural roadmap item G.
 * Compiles to an empty translation unit when HL_ENABLE_JS is not set.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "hull/utils/alloc.h"
#include "manifest_internal.h"
#include "log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HL_ENABLE_JS

#include "quickjs.h"
#include "hull/runtime/js.h"   /* HlJS.manifest */

/* As JS_ToCString, but NULL (nothing to free) when the string holds a NUL
 * byte - see manifest_lua.c's mstr. */
static const char *mjs_str(JSContext *ctx, JSValueConst v)
{
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (s && strlen(s) != n) { JS_FreeCString(ctx, s); return NULL; }
    if (!s) JS_FreeValue(ctx, JS_GetException(ctx));
    return s;
}

/* The manifest app.manifest() stored (HlJS.manifest, a frozen plain-data
 * copy held in C), or undefined. Never a global: an app could define
 * globalThis.__hull_manifest itself - a Proxy showing this extractor one
 * policy and the JSON encoder below another. */
static JSValue mjs_stored(JSContext *ctx)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->manifest) return JS_UNDEFINED;
    return JS_DupValue(ctx, *(JSValue *)js->manifest);
}

/* An OWN data property of @p obj, or undefined: never inherited (a field
 * declared as an array was read through Array.prototype, so
 * `fs: []` + `Array.prototype.write = ["."]` widened fs.write), never a
 * getter. */
static JSValue mjs_own_atom(JSContext *ctx, JSValueConst obj, JSAtom a)
{
    if (!JS_IsObject(obj)) return JS_UNDEFINED;
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, obj, a);
    if (has < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
    if (!has) return JS_UNDEFINED;
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    if (d.flags & JS_PROP_GETSET) { JS_FreeValue(ctx, d.value); return JS_UNDEFINED; }
    return d.value;
}

static JSValue mjs_own(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
    JSValue v = mjs_own_atom(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    return v;
}

/* A `wasm` limit: a number of at least 1, capped at @p max; anything else
 * (absent, an array, a string, an object, 0, a negative number, NaN) is
 * absent - 0, the default. Never converted through ToPrimitive: an array
 * reached the app-replaceable Array.prototype.valueOf, which picked the
 * enforced number while the signed JSON showed the array, and `{}` left an
 * exception pending; -1 was cast to unsigned and enforced as the maximum
 * (audit 7 c_core L1). */
static int64_t mjs_wasm_limit(JSContext *ctx, JSValueConst wasm,
                              const char *name, int64_t max)
{
    JSValue v = mjs_own(ctx, wasm, name);
    int64_t out = 0;
    if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(ctx, &d, v);   /* a number: runs no app code */
        if (d >= 1)                 /* false for NaN */
            out = d >= (double)max ? max : (int64_t)d;
    }
    JS_FreeValue(ctx, v);
    return out;
}

/* A plain object (not an array, not null): where the manifest expects an
 * object, an array is treated as absent. */
static int mjs_is_obj(JSContext *ctx, JSValueConst v)
{
    return JS_IsObject(v) && JS_IsArray(ctx, v) == 0;
}

/* An own array's length (0 when it is not an array). */
static int32_t mjs_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len_val = mjs_own(ctx, arr, "length");
    int32_t len = 0;
    if (JS_ToInt32(ctx, &len, len_val) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        len = 0;
    }
    JS_FreeValue(ctx, len_val);
    return len < 0 ? 0 : len;
}

/* An own array element (undefined for a hole). */
static JSValue mjs_elem(JSContext *ctx, JSValueConst arr, uint32_t i)
{
    JSAtom a = JS_NewAtomUInt32(ctx, i);
    if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
    JSValue v = mjs_own_atom(ctx, arr, a);
    JS_FreeAtom(ctx, a);
    return v;
}

/* Read a string array from a JS object property into a C array.
 * Strings are copied via hl_manifest_strdup; JS strings are freed immediately.
 * Returns number of strings read (capped at max). */
static int read_js_string_array(JSContext *ctx, JSValueConst obj,
                                 const char *field,
                                 const char **out, int max,
                                 HlAllocator *alloc)
{
    int count = 0;
    if (!mjs_is_obj(ctx, obj)) return 0;
    JSValue arr = mjs_own(ctx, obj, field);
    if (JS_IsArray(ctx, arr) != 1) {
        JS_FreeValue(ctx, arr);
        return 0;
    }

    int32_t len = mjs_len(ctx, arr);

    for (int32_t i = 0; i < len && count < max; i++) {
        JSValue elem = mjs_elem(ctx, arr, (uint32_t)i);
        if (JS_IsString(elem)) {
            const char *s = mjs_str(ctx, elem);
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

    JSValue manifest = mjs_stored(ctx);

    if (JS_IsUndefined(manifest) || JS_IsNull(manifest)) {
        JS_FreeValue(ctx, manifest);
        return -1; /* no manifest declared */
    }
    if (!mjs_is_obj(ctx, manifest)) {   /* app.manifest refuses one too */
        JS_FreeValue(ctx, manifest);
        return -1;
    }

    out->present = 1;

    /* fs = { read: [...], write: [...] } */
    JSValue fs = mjs_own(ctx, manifest, "fs");
    if (mjs_is_obj(ctx, fs)) {
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

    /* secrets = [...]: names a "$VAR" may read, not env.get */
    out->secrets_count = read_js_string_array(ctx, manifest, "secrets",
                                                out->secrets,
                                                HL_MANIFEST_MAX_SECRETS, alloc);

    /* hosts = [...] */
    out->hosts_count = read_js_string_array(ctx, manifest, "hosts",
                                              out->hosts,
                                              HL_MANIFEST_MAX_HOSTS, alloc);

    /* csp = "policy-string" or false */
    JSValue csp_val = mjs_own(ctx, manifest, "csp");
    if (JS_IsString(csp_val)) {
        const char *csp_str = mjs_str(ctx, csp_val);
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
    JSValue cors_val = mjs_own(ctx, manifest, "cors");
    if (mjs_is_obj(ctx, cors_val)) {
        out->cors_set = 1;
        out->cors_origin_count = read_js_string_array(ctx, cors_val, "origins",
                                                        out->cors_origins,
                                                        HL_MANIFEST_MAX_CORS_ORIGINS,
                                                        alloc);

        JSValue methods_val = mjs_own(ctx, cors_val, "methods");
        if (JS_IsString(methods_val)) {
            const char *s = mjs_str(ctx, methods_val);
            if (s) {
                out->cors_methods = hl_manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, methods_val);

        JSValue headers_val = mjs_own(ctx, cors_val, "headers");
        if (JS_IsString(headers_val)) {
            const char *s = mjs_str(ctx, headers_val);
            if (s) {
                out->cors_headers = hl_manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, headers_val);

        JSValue creds_val = mjs_own(ctx, cors_val, "credentials");
        if (JS_IsBool(creds_val))
            out->cors_credentials = JS_ToBool(ctx, creds_val);
        JS_FreeValue(ctx, creds_val);

        JSValue age_val = mjs_own(ctx, cors_val, "maxAge");
        if (JS_IsNumber(age_val)) {
            int32_t age = 0;
            JS_ToInt32(ctx, &age, age_val);
            out->cors_max_age = age;
        }
        JS_FreeValue(ctx, age_val);
    }
    JS_FreeValue(ctx, cors_val);

    /* wasm: { heap, stack, gas, timeoutMs, maxInput, maxOutput } */
    JSValue wasm_val = mjs_own(ctx, manifest, "wasm");
    if (mjs_is_obj(ctx, wasm_val)) {
        const int64_t u32 = (int64_t)UINT32_MAX;
        out->wasm_heap       = (uint32_t)mjs_wasm_limit(ctx, wasm_val, "heap", u32);
        out->wasm_stack      = (uint32_t)mjs_wasm_limit(ctx, wasm_val, "stack", u32);
        out->wasm_gas        = mjs_wasm_limit(ctx, wasm_val, "gas", INT64_MAX / 2);
        out->wasm_timeout_ms = (uint32_t)mjs_wasm_limit(ctx, wasm_val, "timeoutMs", u32);
        out->wasm_max_input  = (uint32_t)mjs_wasm_limit(ctx, wasm_val, "maxInput", u32);
        out->wasm_max_output = (uint32_t)mjs_wasm_limit(ctx, wasm_val, "maxOutput", u32);
    }
    JS_FreeValue(ctx, wasm_val);

    /* gpu: true  OR  gpu: { devices: [0, 1] } */
    JSValue gpu_val = mjs_own(ctx, manifest, "gpu");
    if (mjs_is_obj(ctx, gpu_val)) {
        out->gpu = 1;
        JSValue devs = mjs_own(ctx, gpu_val, "devices");
        if (JS_IsArray(ctx, devs) == 1) {
            int32_t len = mjs_len(ctx, devs);
            for (int32_t i = 0; i < len && out->gpu_device_count < HL_GPU_MAX_DEVICES; i++) {
                JSValue elem = mjs_elem(ctx, devs, (uint32_t)i);
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
    JSValue compute_val = mjs_own(ctx, manifest, "compute");
    if (JS_IsBool(compute_val))
        out->compute = JS_ToBool(ctx, compute_val);
    JS_FreeValue(ctx, compute_val);

    /* tui: true */
    JSValue tui_val = mjs_own(ctx, manifest, "tui");
    if (JS_IsBool(tui_val))
        out->tui = JS_ToBool(ctx, tui_val);
    JS_FreeValue(ctx, tui_val);

    /* modules: ["hull/crypto@1", "hull/db@1", ...] - an array of
     * canonical spec strings. The local variable / imported identifier
     * is a plain JS binding (the user chooses the name); the manifest
     * only declares which canonical modules are in scope.
     *
     * For back-compat we also accept the legacy object form when the
     * value is a non-array object (`{ crypto: "hull/crypto@1" }`) -
     * keys are ignored as cosmetic labels.
     *
     * Presence of `modules` (array OR object, even empty) sets
     * `modules_declared = 1`. */
    JSValue modules_val = mjs_own(ctx, manifest, "modules");

    if (JS_IsArray(ctx, modules_val) == 1) {
        out->modules_declared = 1;
        int32_t len = mjs_len(ctx, modules_val);
        if (len > HL_MANIFEST_MAX_MODULES)
            log_warn("[manifest] modules array exceeds "
                     "HL_MANIFEST_MAX_MODULES (%d), truncated",
                     HL_MANIFEST_MAX_MODULES);
        for (int32_t i = 0;
             i < len && out->modules_count < HL_MANIFEST_MAX_MODULES;
             i++) {
            JSValue elem = mjs_elem(ctx, modules_val, (uint32_t)i);
            if (JS_IsString(elem)) {
                const char *spec = mjs_str(ctx, elem);
                if (spec) {
                    /* Trailing '?' marks the module optional (skip, not error,
                     * when its build cap is absent); sits after the major. */
                    size_t speclen = strlen(spec);
                    int optional = (speclen > 0 && spec[speclen - 1] == '?');
                    const char *at = strchr(spec, '@');
                    if (at && at != spec) {
                        char *end = NULL;
                        long v = strtol(at + 1, &end, 10);
                        int end_ok = (*end == '\0') ||
                                     (optional && end[0] == '?' && end[1] == '\0');
                        if (end != at + 1 && end_ok &&
                            v >= 1 && v <= 255) {
                            size_t nlen = (size_t)(at - spec);
                            char *namebuf = hl_alloc_malloc(alloc, nlen + 1);
                            if (namebuf) {
                                memcpy(namebuf, spec, nlen);
                                namebuf[nlen] = '\0';
                                out->modules[out->modules_count].name      = namebuf;
                                out->modules[out->modules_count].api_major = (uint8_t)v;
                                out->modules[out->modules_count].optional  = (uint8_t)optional;
                                out->modules_count++;
                            }
                        } else {
                            log_warn("[manifest] modules[%d] = %s - invalid "
                                     "major version, ignored", (int)i, spec);
                        }
                    } else {
                        log_warn("[manifest] modules[%d] = %s - expected "
                                 "\"vendor/name@version\", ignored",
                                 (int)i, spec);
                    }
                    JS_FreeCString(ctx, spec);
                }
            }
            JS_FreeValue(ctx, elem);
        }
    } else if (mjs_is_obj(ctx, modules_val)) {
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
                JSValue v_val = mjs_own_atom(ctx, modules_val, props[i].atom);
                if (alias && JS_IsString(v_val)) {
                    const char *spec = mjs_str(ctx, v_val);
                    if (spec) {
                        size_t speclen = strlen(spec);
                        int optional = (speclen > 0 && spec[speclen - 1] == '?');
                        const char *at = strchr(spec, '@');
                        if (!at || at == spec) {
                            log_warn("[manifest] modules.%s = %s - expected "
                                     "\"vendor/name@version\", ignored",
                                     alias, spec);
                        } else {
                            char *end = NULL;
                            long v = strtol(at + 1, &end, 10);
                            int end_ok = (*end == '\0') ||
                                         (optional && end[0] == '?' && end[1] == '\0');
                            if (end == at + 1 || !end_ok ||
                                v < 1 || v > 255) {
                                log_warn("[manifest] modules.%s = %s - invalid "
                                         "major version, ignored", alias, spec);
                            } else {
                                size_t nlen = (size_t)(at - spec);
                                char *namebuf = hl_alloc_malloc(alloc, nlen + 1);
                                if (namebuf) {
                                    memcpy(namebuf, spec, nlen);
                                    namebuf[nlen] = '\0';
                                    out->modules[out->modules_count].name      = namebuf;
                                    out->modules[out->modules_count].api_major = (uint8_t)v;
                                    out->modules[out->modules_count].optional  = (uint8_t)optional;
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

    /* databases: { named: { cache: "./cache.db", primary: "$DATABASE_URL" },
     *              dynamic: { hosts: [...], schemes: [...] } }
     * named: name -> DSN string (a "$VAR"/"${VAR}" value is an env ref resolved
     *   at open; a value that merely contains '$' is literal).
     * dynamic: the db.open(dsn) allowlist. Mirror of the Lua parser. */
    JSValue db_val = mjs_own(ctx, manifest, "databases");
    if (mjs_is_obj(ctx, db_val)) {
        out->databases.declared = 1;

        JSValue named = mjs_own(ctx, db_val, "named");
        if (mjs_is_obj(ctx, named)) {
            JSPropertyEnum *props = NULL;
            uint32_t prop_count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, named,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < prop_count; i++) {
                    if (out->databases.named_count >= HL_MANIFEST_MAX_DATABASES) {
                        log_warn("[manifest] databases.named exceeds "
                                 "HL_MANIFEST_MAX_DATABASES (%d), truncated",
                                 HL_MANIFEST_MAX_DATABASES);
                        for (uint32_t j = i; j < prop_count; j++)
                            JS_FreeAtom(ctx, props[j].atom);
                        break;
                    }
                    size_t name_len = 0;
                    const char *name = JS_AtomToCStringLen(ctx, &name_len, props[i].atom);
                    if (name && memchr(name, '\0', name_len)) {   /* "a\0b" is not "a" */
                        JS_FreeCString(ctx, name);
                        name = NULL;
                    }
                    JSValue v_val = mjs_own_atom(ctx, named, props[i].atom);
                    const char *dsn_copy = NULL;
                    if (JS_IsString(v_val)) {
                        const char *s = mjs_str(ctx, v_val);
                        if (s) { dsn_copy = hl_manifest_strdup(alloc, s);
                                 JS_FreeCString(ctx, s); }
                    }
                    if (name && name[0] && dsn_copy && dsn_copy[0]) {
                        const char *ncopy = hl_manifest_strdup(alloc, name);
                        if (ncopy) {
                            out->databases.named[out->databases.named_count].name = ncopy;
                            out->databases.named[out->databases.named_count].dsn  = dsn_copy;
                            out->databases.named_count++;
                        }
                    } else {
                        log_warn("[manifest] databases.named.%s: expected a DSN "
                                 "string, ignored", name ? name : "?");
                    }
                    if (name) JS_FreeCString(ctx, name);
                    JS_FreeValue(ctx, v_val);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, named);

        JSValue dyn = mjs_own(ctx, db_val, "dynamic");
        if (mjs_is_obj(ctx, dyn)) {
            out->databases.dynamic.declared = 1;
            out->databases.dynamic.host_count =
                read_js_string_array(ctx, dyn, "hosts",
                                     out->databases.dynamic.hosts,
                                     HL_MANIFEST_MAX_DB_HOSTS, alloc);
            out->databases.dynamic.scheme_count =
                read_js_string_array(ctx, dyn, "schemes",
                                     out->databases.dynamic.schemes,
                                     HL_MANIFEST_MAX_DB_SCHEMES, alloc);
        }
        JS_FreeValue(ctx, dyn);

        JSValue internal = mjs_own(ctx, db_val, "internal");
        if (JS_IsString(internal)) {
            const char *s = mjs_str(ctx, internal);
            if (s && s[0])
                out->databases.internal = hl_manifest_strdup(alloc, s);
            if (s) JS_FreeCString(ctx, s);
        } else if (!JS_IsUndefined(internal) && !JS_IsNull(internal)) {
            log_warn("[manifest] databases.internal: expected a DSN string");
        }
        JS_FreeValue(ctx, internal);

        if (out->databases.named_count == 0 && !out->databases.dynamic.declared
            && !out->databases.internal)
            log_warn("[manifest] databases has no `named` or `dynamic` entry; "
                     "named connections now go under databases.named = {...}");
    }
    JS_FreeValue(ctx, db_val);

    /* kv: { dynamic: { hosts: [...], schemes: [...] } }
     * The kv.open({backend:"valkey", dsn}) allowlist (mirror of databases.dynamic
     * and the Lua parser). Every KV scheme is a network scheme gated by hosts. */
    JSValue kv_val = mjs_own(ctx, manifest, "kv");
    if (mjs_is_obj(ctx, kv_val)) {
        out->kv.declared = 1;
        JSValue kdyn = mjs_own(ctx, kv_val, "dynamic");
        if (mjs_is_obj(ctx, kdyn)) {
            out->kv.dynamic.declared = 1;
            out->kv.dynamic.host_count =
                read_js_string_array(ctx, kdyn, "hosts",
                                     out->kv.dynamic.hosts,
                                     HL_MANIFEST_MAX_DB_HOSTS, alloc);
            out->kv.dynamic.scheme_count =
                read_js_string_array(ctx, kdyn, "schemes",
                                     out->kv.dynamic.schemes,
                                     HL_MANIFEST_MAX_DB_SCHEMES, alloc);
        }
        JS_FreeValue(ctx, kdyn);
    }
    JS_FreeValue(ctx, kv_val);

    /* allowDynamicCode: true - opt-in to JIT / runtime codegen.
     * Rejected by hl_sandbox_apply unless --no-sandbox.
     * Also accept the snake_case form for parity with the Lua manifest. */
    JSValue adc_val = mjs_own(ctx, manifest, "allowDynamicCode");
    if (JS_IsUndefined(adc_val)) {
        JS_FreeValue(ctx, adc_val);
        adc_val = mjs_own(ctx, manifest, "allow_dynamic_code");
    }
    if (JS_IsBool(adc_val))
        out->allow_dynamic_code = JS_ToBool(ctx, adc_val);
    JS_FreeValue(ctx, adc_val);
    if (out->allow_dynamic_code)
        log_warn("[manifest] allowDynamicCode=true - kernel sandbox "
                 "will fail closed unless --no-sandbox is set");

    /* allowDynamicLibraries: true - opt-in to dlopen() of native libs. */
    JSValue adl_val = mjs_own(ctx, manifest, "allowDynamicLibraries");
    if (JS_IsUndefined(adl_val)) {
        JS_FreeValue(ctx, adl_val);
        adl_val = mjs_own(ctx, manifest, "allow_dynamic_libraries");
    }
    if (JS_IsBool(adl_val))
        out->allow_dynamic_libraries = JS_ToBool(ctx, adl_val);
    JS_FreeValue(ctx, adl_val);
    if (out->allow_dynamic_libraries)
        log_warn("[manifest] allowDynamicLibraries=true - kernel sandbox "
                 "will fail closed unless --no-sandbox is set");

    JS_FreeValue(ctx, manifest);
    return 0;
}


/* ── hl_manifest_json_js ─────────────────────────────────────────────── */

typedef struct { char *p; size_t len, cap; int oom; } MjBuf;

static void mj_put(MjBuf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (n > SIZE_MAX / 2 - b->len) { b->oom = 1; return; }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void mj_str(MjBuf *b, const char *s, size_t n)
{
    mj_put(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\'; esc[1] = (char)c;
            mj_put(b, esc, 2);
        } else if (c < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", c);
            mj_put(b, esc, 6);
        } else {
            mj_put(b, (const char *)&s[i], 1);
        }
    }
    mj_put(b, "\"", 1);
}

/* Encode @p v. Objects and arrays are walked through their OWN properties
 * (JS_GetOwnProperty: no getter runs, nothing is inherited); only data
 * properties holding JSON values are accepted. */
static int mj_value(JSContext *ctx, MjBuf *b, JSValueConst v, int depth)
{
    if (depth > 32) return -1;
    if (JS_IsNull(v))  { mj_put(b, "null", 4); return 0; }
    if (JS_IsBool(v))  {
        if (JS_ToBool(ctx, v)) mj_put(b, "true", 4); else mj_put(b, "false", 5);
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v) != 0 || d != d || d - d != 0) return -1;
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, v);   /* a primitive: no user code */
        if (!s) { JS_FreeValue(ctx, JS_GetException(ctx)); return -1; }
        mj_put(b, s, n);
        JS_FreeCString(ctx, s);
        return 0;
    }
    if (JS_IsString(v)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s) { JS_FreeValue(ctx, JS_GetException(ctx)); return -1; }
        mj_str(b, s, n);
        JS_FreeCString(ctx, s);
        return 0;
    }
    if (!JS_IsObject(v) || JS_IsFunction(ctx, v)) return -1;

    int is_arr = JS_IsArray(ctx, v);
    if (is_arr < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); return -1; }
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    int rc = 0, first = 1;
    mj_put(b, is_arr ? "[" : "{", 1);
    for (uint32_t i = 0; i < n && rc == 0; i++) {
        JSPropertyDescriptor d;
        int has = JS_GetOwnProperty(ctx, &d, v, tab[i].atom);
        if (has < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); rc = -1; break; }
        if (!has) continue;
        if (d.flags & JS_PROP_GETSET) {
            rc = -1;                       /* an accessor is not data */
        } else {
            if (!first) mj_put(b, ",", 1);
            first = 0;
            if (!is_arr) {
                /* Length-exact: a key with a NUL ("hosts\0") was cut at
                 * the NUL and encoded as a second "hosts" (L3). */
                size_t kl = 0;
                const char *k = JS_AtomToCStringLen(ctx, &kl, tab[i].atom);
                if (!k) { JS_FreeValue(ctx, JS_GetException(ctx)); rc = -1; }
                else {
                    if (memchr(k, '\0', kl)) rc = -1;
                    else { mj_str(b, k, kl); mj_put(b, ":", 1); }
                    JS_FreeCString(ctx, k);
                }
            }
            if (rc == 0) rc = mj_value(ctx, b, d.value, depth + 1);
        }
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    for (uint32_t i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    mj_put(b, is_arr ? "]" : "}", 1);
    return rc;
}

int hl_manifest_json_js(JSContext *ctx, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    /* The same C-held value the extractor reads (mjs_stored). */
    JSValue m = mjs_stored(ctx);
    if (JS_IsUndefined(m)) return 0;
    if (!mjs_is_obj(ctx, m)) { JS_FreeValue(ctx, m); return -1; }
    MjBuf b = {0};
    int rc = mj_value(ctx, &b, m, 0);
    JS_FreeValue(ctx, m);
    if (rc != 0 || b.oom || !b.p) { free(b.p); return -1; }
    *out = b.p;
    *out_len = b.len;
    return 0;
}

#endif /* HL_ENABLE_JS */
