/*
 * js_runtime.c — QuickJS runtime for Hull
 *
 * Initializes QuickJS with sandboxing: no eval, no std/os modules,
 * custom allocator, memory limits, instruction-count interrupt handler,
 * and custom module loader for hull:* built-in modules.
 *
 * This file holds VM lifecycle + sandbox + module loader + the
 * runtime-vtable wrappers. Request dispatch, middleware, route wiring,
 * timers, WebSockets, and SSE live in the sibling .c files split out
 * from this TU (see internal.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/alloc.h"
#include "hull/async_backend.h"
#include "hull/limits/runtime.h"
#include "hull/manifest.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"
#include "hull/path_normalize.h"
#include "hull/cap/test.h"
#include "hull/runtime/test.h"

#include <keel/keel.h>
#include <keel/body_reader_multipart.h>
#include <keel/websocket_server.h>

#include "hull/cap/ws.h"

#include <sh_arena.h>

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VFS: O(log n) lookups into sorted entry arrays */
#include "hull/vfs.h"

/* ── Forward declarations ───────────────────────────────────────────── */

/* Defined in js/async.c — registers hull global (sleep, etc.) */
extern void hl_js_add_hull_global(JSContext *ctx);

/* ── Interrupt handler (gas metering) ───────────────────────────────── */

static int hl_js_interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    HlJS *js = (HlJS *)opaque;
    js->instruction_count++;
    if (js->max_instructions > 0 &&
        js->instruction_count > js->max_instructions) {
        return 1; /* interrupt — JS_Eval returns exception */
    }
    return 0;
}

/* ── Module loader ──────────────────────────────────────────────────── */

/*
 * Reject module names that are absolute or contain NULs. Path-traversal
 * (`..`) is NOT rejected here: the resolver builds the full joined
 * path and runs hl_path_normalize, which fails closed if `..` would
 * collapse past the caller's base directory. Letting safe `..` through
 * — e.g. `./../models/user.js` from `routes/users.js` resolves to
 * `models/user.js` — matches the Lua require-gate behaviour.
 *
 * Returns 0 if safe, -1 if absolute.
 */
static int hl_js_validate_module_name(const char *name)
{
    if (name[0] == '/')
        return -1;
    return 0;
}

/*
 * Module name normalizer. For hull: prefix, return as-is.
 * For relative paths, resolve against the application root.
 */
static char *hl_js_module_normalize(JSContext *ctx,
                                       const char *base_name,
                                       const char *name, void *opaque)
{
    (void)base_name;
    (void)opaque;

    /* Defensive NULL guard — QuickJS's module loader is documented to
     * pass non-NULL names, but a NULL slipping through would crash at
     * the strncmp below. Mirrors hl_path_normalize's NULL handling. */
    if (!name) return NULL;

    /* hull:* modules are already normalized */
    if (strncmp(name, "hull:", 5) == 0)
        return js_strdup(ctx, name);

    /* Reject path traversal */
    if (hl_js_validate_module_name(name) != 0) {
        JS_ThrowReferenceError(ctx, "invalid module path: %s", name);
        return NULL;
    }

    /* Relative paths: resolve against app directory */
    if (name[0] == '.') {
        /* Find the directory of the base module */
        const char *last_slash = strrchr(base_name, '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - base_name);
            size_t name_len = strlen(name);
            /* Overflow guard */
            if (dir_len > SIZE_MAX / 2 || name_len > SIZE_MAX / 2)
                return NULL;
            size_t total = dir_len + 1 + name_len + 1;
            char *resolved = js_malloc(ctx, total);
            if (!resolved)
                return NULL;
            memcpy(resolved, base_name, dir_len);
            resolved[dir_len] = '/';
            memcpy(resolved + dir_len + 1, name, name_len + 1);

            /* Collapse `.` and `..` segments. hl_path_normalize returns
             * -1 if the result would escape past the root (i.e. more
             * `..` than path depth), which is the only traversal case
             * we need to reject. Previously this code rejected any
             * `..` substring before normalizing, which broke valid
             * cross-directory imports like `./../models/user.js` from
             * routes/users.js — a regression vs the Lua-side require
             * gate which already normalized correctly. */
            if (hl_path_normalize(resolved) != 0) {
                js_free(ctx, resolved);
                JS_ThrowReferenceError(ctx, "invalid module path: %s", name);
                return NULL;
            }

            return resolved;
        }
    }

    return js_strdup(ctx, name);
}

/*
 * Module-declaration gate for native modules. See internal.h for
 * the contract. Used by each mod_*.c init callback so QuickJS can
 * refuse imports of undeclared first-party modules at the same
 * boundary where the Lua require() gate fires (mod_fs.c).
 */
int hl_js_check_module_declared(JSContext *ctx,
                                 const char *canonical_name,
                                 const char *runtime_name)
{
    HlJS *js = JS_GetContextOpaque(ctx);
    if (!js) return 0;

    const HlModuleSpec *spec = hl_module_registry_find(canonical_name);
    if (!spec) return 0;  /* private / not registry-known — pass through */

    /* module_set wired (post-manifest): gate immediately. */
    if (js->base.module_set) {
        if (hl_module_set_contains_spec(js->base.module_set, spec)) return 0;

        char deps_buf[128];
        hl_module_registry_format_deps(spec, deps_buf, sizeof(deps_buf));
        char deps_part[160] = {0};
        if (deps_buf[0])
            snprintf(deps_part, sizeof(deps_part), " (also needs: %s)", deps_buf);

        JS_ThrowReferenceError(ctx,
            "module '%s' is not declared in app.manifest. "
            "Add \"%s@%d\" to the modules array%s. "
            "See `hull modules available` for the full list.",
            runtime_name, spec->name, (int)spec->api_major, deps_part);
        return -1;
    }

    /* Pre-manifest window: record the canonical name. serve.c validates
     * the tracker after the resolver runs, so undeclared imports that
     * slip through this window get caught synchronously at app-load
     * time. */
    hl_import_tracker_record(&js->base, spec->name);
    return 0;
}

/*
 * Module loader. Handles:
 * 1. hull:* prefix → built-in modules (registered at init time)
 * 2. Relative paths → load from filesystem (dev mode)
 */
static JSModuleDef *hl_js_module_loader(JSContext *ctx,
                                           const char *module_name,
                                           void *opaque)
{
    HlJS *js = (HlJS *)opaque;

    /* hull:* modules are pre-registered as native C modules — QuickJS
     * resolves them automatically. If we get here, it wasn't found as
     * a native module, so check the embedded JS stdlib registry. */
    if (strncmp(module_name, "hull:", 5) == 0) {
        /* Capability-aware gate. If the runtime has a resolved module
         * set wired AND the name maps to a known first-party module
         * not in that set, refuse before doing the VFS lookup. Names
         * outside the registry fall through to the normal "unknown
         * hull module" error.
         *
         * If module_set isn't wired yet (top-of-file import running
         * before app.manifest()), record the canonical name in the
         * runtime's import tracker. serve.c validates the tracker
         * against the resolved set after manifest extraction. */
        const HlModuleSpec *spec =
            hl_module_registry_find_runtime(module_name, ':');
        if (spec) {
            if (js->base.module_set) {
                if (!hl_module_set_contains_spec(js->base.module_set, spec)) {
                    char deps_buf[128];
                    hl_module_registry_format_deps(spec, deps_buf, sizeof(deps_buf));
                    char deps_part[160] = {0};
                    if (deps_buf[0])
                        snprintf(deps_part, sizeof(deps_part),
                                 " (also needs: %s)", deps_buf);
                    JS_ThrowReferenceError(ctx,
                        "module '%s' is not declared in app.manifest. "
                        "Add \"%s@%d\" to the modules array%s. "
                        "See `hull modules available` for the full list.",
                        module_name, spec->name, (int)spec->api_major, deps_part);
                    return NULL;
                }
            } else {
                hl_import_tracker_record(&js->base, spec->name);
            }
        }

        if (js->base.platform_vfs) {
            const HlEntry *e = hl_vfs_find(js->base.platform_vfs, module_name);
            if (e) {
                /* QuickJS lexer requires a '\0' sentinel after the
                 * source buffer; xxd arrays lack one, so copy. */
                char *src = js_malloc(ctx, (size_t)e->len + 1);
                if (!src) return NULL;
                memcpy(src, e->data, e->len);
                src[e->len] = '\0';
                JSValue func = JS_Eval(ctx, src, e->len,
                                       module_name,
                                       JS_EVAL_TYPE_MODULE |
                                       JS_EVAL_FLAG_COMPILE_ONLY);
                js_free(ctx, src);
                if (JS_IsException(func))
                    return NULL;
                JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func);
                JS_FreeValue(ctx, func);
                return m;
            }
        }
        /* "hull:something" that isn't a known native and isn't in the
         * VFS stdlib — almost always a typo. Probe the registry for a
         * near match and surface "did you mean?" if anything is close. */
        {
            char short_name[64];
            size_t i = 0;
            const char *src = module_name + 5;  /* skip "hull:" */
            while (*src && i + 1 < sizeof(short_name)) {
                short_name[i++] = (*src == ':') ? '/' : *src;
                src++;
            }
            short_name[i] = '\0';
            const HlModuleSpec *guess = hl_module_registry_suggest(short_name);
            if (guess) {
                char hint[128];
                const char *g = guess->name;
                size_t hpos = 0;
                while (*g && hpos + 1 < sizeof(hint)) {
                    hint[hpos++] = (*g == '/') ? ':' : *g;
                    g++;
                }
                hint[hpos] = '\0';
                JS_ThrowReferenceError(ctx,
                    "unknown hull module: %s — did you mean import \"%s\"?",
                    module_name, hint);
                return NULL;
            }
        }
        JS_ThrowReferenceError(ctx, "unknown hull module: %s", module_name);
        return NULL;
    }

    /* Check embedded app JS modules via VFS (hull build / make APP_DIR) */
    if (js->base.app_vfs && js->base.app_vfs->count > 0) {
        /* The normalizer may prepend the app dir path to relative imports
         * (e.g. "examples/todo/./locales/en.json").  Embedded entries use
         * the canonical "./" form, so find that suffix. */
        const char *lookup = module_name;
        const char *dot_slash = strstr(module_name, "/./");
        if (dot_slash)
            lookup = dot_slash + 1; /* points to "./" */

        const HlEntry *e = hl_vfs_find(js->base.app_vfs, lookup);
        if (!e && lookup != module_name)
            e = hl_vfs_find(js->base.app_vfs, module_name);

        if (e && e->name[0] == '.') {
            /* Check it's a JS/JSON entry (not Lua-only) */
            size_t elen = strlen(e->name);
            int is_js = (elen >= 3 && strcmp(e->name + elen - 3, ".js") == 0);
            int is_json = (elen >= 5 && strcmp(e->name + elen - 5, ".json") == 0);

            if (is_js || is_json) {
                const char *src = (const char *)e->data;
                size_t src_len = e->len;
                char *buf = NULL;

                /* QuickJS lexer requires a '\0' sentinel after the
                 * source buffer; xxd arrays lack one, so copy for
                 * plain JS files. JSON wrapping below already
                 * null-terminates via the suffix copy. */
                if (is_js && !is_json) {
                    buf = js_malloc(ctx, src_len + 1);
                    if (!buf) return NULL;
                    memcpy(buf, src, src_len);
                    buf[src_len] = '\0';
                    src = buf;
                }

                /* JSON → wrap as export default JSON.parse(`...`)
                 * Escape \, ` and ${ so the template literal passes
                 * the original JSON bytes through to JSON.parse. */
                if (is_json) {
                    const char *pfx = "export default JSON.parse(`";
                    const char *sfx = "`);\n";
                    size_t plen = strlen(pfx), slen = strlen(sfx);

                    /* Count chars that need a backslash prefix */
                    size_t extra = 0;
                    for (size_t k = 0; k < src_len; k++) {
                        if (src[k] == '\\' || src[k] == '`')
                            extra++;
                        else if (src[k] == '$' && k + 1 < src_len &&
                                 src[k + 1] == '{')
                            extra++;
                    }

                    size_t wrap_len = plen + src_len + extra + slen;
                    buf = js_malloc(ctx, wrap_len + 1);
                    if (!buf) return NULL;
                    memcpy(buf, pfx, plen);

                    size_t pos = plen;
                    for (size_t k = 0; k < src_len; k++) {
                        if (src[k] == '\\' || src[k] == '`') {
                            buf[pos++] = '\\';
                            buf[pos++] = src[k];
                        } else if (src[k] == '$' && k + 1 < src_len &&
                                   src[k + 1] == '{') {
                            buf[pos++] = '\\';
                            buf[pos++] = '$';
                        } else {
                            buf[pos++] = src[k];
                        }
                    }

                    memcpy(buf + pos, sfx, slen + 1);
                    src = buf;
                    src_len = pos + slen;
                }

                JSValue func = JS_Eval(ctx, src, src_len, module_name,
                                       JS_EVAL_TYPE_MODULE |
                                       JS_EVAL_FLAG_COMPILE_ONLY);
                if (buf) js_free(ctx, buf);
                if (JS_IsException(func))
                    return NULL;
                JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func);
                JS_FreeValue(ctx, func);
                return m;
            }
        }
    }

    /* Load from filesystem (development mode) */
    if (!js->app_dir) {
        JS_ThrowReferenceError(ctx, "no app directory configured");
        return NULL;
    }

    /* Build filesystem path. After the normalizer collapses `.` / `..`
     * segments, module_name is either an absolute path (when the base
     * module was loaded from an absolute filesystem path, dev mode) or
     * a clean app-relative path (when joining against an embedded /
     * tool-mode base). For absolute paths, fopen directly; for
     * relative, prepend app_dir. The old loader had to strip "/./"
     * here as a workaround because the normalizer left those in
     * place — no longer needed. */
    char path[HL_MODULE_PATH_MAX];
    int n;
    if (module_name[0] == '/') {
        n = snprintf(path, sizeof(path), "%s", module_name);
    } else {
        n = snprintf(path, sizeof(path), "%s/%s", js->app_dir, module_name);
    }
    if (n < 0 || (size_t)n >= sizeof(path)) {
        JS_ThrowReferenceError(ctx, "module path too long: %s", module_name);
        return NULL;
    }

    /* Read file */
    FILE *f = fopen(path, "rb");
    if (!f) {
        JS_ThrowReferenceError(ctx, "module not found: %s", module_name);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0 || size > HL_MODULE_MAX_SIZE) {
        fclose(f);
        JS_ThrowReferenceError(ctx, "module too large: %s", module_name);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        JS_ThrowInternalError(ctx, "seek failed: %s", module_name);
        return NULL;
    }

    char *buf = js_malloc(ctx, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    int read_err = ferror(f);
    fclose(f);
    if (read_err || nread != (size_t)size) {
        js_free(ctx, buf);
        return NULL;
    }
    buf[nread] = '\0';

    /* JSON file → wrap as: export default JSON.parse(`...`)
     * Escape \, ` and ${ so the template literal passes the original
     * JSON bytes through to JSON.parse (same logic as embedded path). */
    size_t mname_len = strlen(module_name);
    if (mname_len >= 5 &&
        strcmp(module_name + mname_len - 5, ".json") == 0) {
        const char *prefix = "export default JSON.parse(`";
        const char *suffix = "`);\n";
        size_t prefix_len = strlen(prefix);
        size_t suffix_len = strlen(suffix);

        /* Count chars that need a backslash prefix */
        size_t extra = 0;
        for (size_t k = 0; k < nread; k++) {
            if (buf[k] == '\\' || buf[k] == '`')
                extra++;
            else if (buf[k] == '$' && k + 1 < nread && buf[k + 1] == '{')
                extra++;
        }

        size_t wrap_len = prefix_len + nread + extra + suffix_len;
        char *wrap = js_malloc(ctx, wrap_len + 1);
        if (!wrap) { js_free(ctx, buf); return NULL; }
        memcpy(wrap, prefix, prefix_len);

        size_t pos = prefix_len;
        for (size_t k = 0; k < nread; k++) {
            if (buf[k] == '\\' || buf[k] == '`') {
                wrap[pos++] = '\\';
                wrap[pos++] = buf[k];
            } else if (buf[k] == '$' && k + 1 < nread &&
                       buf[k + 1] == '{') {
                wrap[pos++] = '\\';
                wrap[pos++] = '$';
            } else {
                wrap[pos++] = buf[k];
            }
        }

        memcpy(wrap + pos, suffix, suffix_len + 1);
        js_free(ctx, buf);
        buf = wrap;
        nread = pos + suffix_len;
    }

    /* Compile as module */
    JSValue func = JS_Eval(ctx, buf, nread, module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    js_free(ctx, buf);

    if (JS_IsException(func))
        return NULL;

    /* js_module_loader convention: return the module def from the
     * compiled module function */
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return m;
}

/* ── Sandbox: remove dangerous globals ──────────────────────────────── */

static void hl_js_sandbox(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    /* Remove eval() — dynamic code execution risk */
    JSAtom eval_atom = JS_NewAtom(ctx, "eval");
    JS_DeleteProperty(ctx, global, eval_atom, 0);
    JS_FreeAtom(ctx, eval_atom);

    /* Remove Function constructor — prevents new Function("...") code
     * execution.  Removing eval alone is insufficient because the
     * Function constructor can independently compile and execute
     * arbitrary code strings. */
    JSAtom fn_atom = JS_NewAtom(ctx, "Function");
    JS_DeleteProperty(ctx, global, fn_atom, 0);
    JS_FreeAtom(ctx, fn_atom);

    JS_FreeValue(ctx, global);
}

/* ── Console polyfill (routes through rxi/log.c) ────────────────────── */

static JSValue js_console_log_impl(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    static const int levels[] = { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_INFO };
    int level = (magic >= 0 && magic < 4) ? levels[magic] : LOG_INFO;
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            log_log(level, "js", 0, "[app] %s", str);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

static void hl_js_add_console(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_console_log_impl,
                             "log", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_console_log_impl,
                             "warn", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_console_log_impl,
                             "error", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_console_log_impl,
                             "info", 1, JS_CFUNC_generic_magic, 3));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/* ── Public API ─────────────────────────────────────────────────────── */

int hl_js_init(HlJS *js, const HlJSConfig *cfg)
{
    if (!js || !cfg)
        return -1;

    /* Save caller-set base fields before zeroing */
    HlRuntime saved_base = js->base;

    memset(js, 0, sizeof(*js));

    /* Restore caller-set base fields */
    js->base = saved_base;
    js->max_instructions = cfg->max_instructions;

    /* Create runtime (using default allocator for now;
     * custom KlAllocator routing added when Keel is linked) */
    js->rt = JS_NewRuntime();
    if (!js->rt)
        return -1;

    JS_SetMemoryLimit(js->rt, cfg->max_heap_bytes);
    JS_SetMaxStackSize(js->rt, cfg->max_stack_bytes);
    JS_SetGCThreshold(js->rt, cfg->gc_threshold);

    /*
     * Stack-top contract for JS_Eval call sites in Hull:
     *
     * QuickJS captures the C stack pointer at JS_NewRuntime time
     * (above, inside the constructor) and computes the stack-overflow
     * ceiling as `rt->stack_top - max_stack_bytes`. Every JS_Eval call
     * is then checked against that ceiling.
     *
     * For top-level JS_Eval entry points called from C at the SAME
     * stack depth as JS_NewRuntime (e.g. hl_js_load_app, called during
     * app_context init), this is a non-issue.
     *
     * For top-level JS_Eval entry points called from C at a DEEPER
     * stack depth than JS_NewRuntime (e.g. vt_js_run_test_file, called
     * through the shared test_runner), the available JS stack shrinks
     * by the C-frame difference — and small test scripts can spuriously
     * trip "stack overflow" because the runtime measures depth from
     * the wrong base.
     *
     * The rule: any C function that calls JS_Eval at a stack depth
     * not reached during JS_NewRuntime must call JS_UpdateStackTop(rt)
     * first to re-anchor the base. This is a no-op when the depth is
     * the same, and corrects the base when it's deeper. Module-loader
     * sites (js_module_loader, called BY JS_Eval) must NOT reset —
     * they're already inside JS execution.
     */

    /* Set interrupt handler for gas metering */
    JS_SetInterruptHandler(js->rt, hl_js_interrupt_handler, js);

    /* Set module loader */
    JS_SetModuleLoaderFunc(js->rt, hl_js_module_normalize,
                           hl_js_module_loader, js);

    /* Create context with selected intrinsics (NO eval) */
    js->ctx = JS_NewContextRaw(js->rt);
    if (!js->ctx) {
        JS_FreeRuntime(js->rt);
        js->rt = NULL;
        return -1;
    }

    /* Add intrinsics — eval intrinsic is needed for JS_Eval() from C,
     * but we remove the JS-visible eval() global in sandbox step */
    JS_AddIntrinsicBaseObjects(js->ctx);
    JS_AddIntrinsicDate(js->ctx);
    JS_AddIntrinsicEval(js->ctx);
    JS_AddIntrinsicStringNormalize(js->ctx);
    JS_AddIntrinsicRegExpCompiler(js->ctx);
    JS_AddIntrinsicRegExp(js->ctx);
    JS_AddIntrinsicJSON(js->ctx);
    JS_AddIntrinsicProxy(js->ctx);
    JS_AddIntrinsicMapSet(js->ctx);
    JS_AddIntrinsicTypedArrays(js->ctx);
    JS_AddIntrinsicPromise(js->ctx);

    /* Apply sandbox (remove eval global, etc.) */
    hl_js_sandbox(js->ctx);

    /* Add console polyfill */
    hl_js_add_console(js->ctx);

    /* Add hull global (sleep, etc.) */
    hl_js_add_hull_global(js->ctx);

    /* Store HlJS pointer in context opaque for C functions to access */
    JS_SetContextOpaque(js->ctx, js);

    /* Register worker VM init hooks (e.g. db.* for worker.dispatch).
     * Must happen before modules are registered since module init may
     * trigger worker VM creation. */
#ifdef HL_ENABLE_DB
    if (js->base.db_handle)
        hl_js_worker_db_init();
#endif

    /* Register hull:* built-in modules */
    if (hl_js_register_modules(js) != 0) {
        hl_js_free(js);
        return -1;
    }

    /* Register embedded JS stdlib modules */
    if (hl_js_register_stdlib(js) != 0) {
        hl_js_free(js);
        return -1;
    }

    /* Per-request scratch arena */
    js->scratch = hl_arena_create(js->base.alloc, HL_SCRATCH_SIZE);
    if (!js->scratch) {
        hl_js_free(js);
        return -1;
    }

    js->udf_runtime_alive = 1;

    return 0;
}

int hl_js_load_app(HlJS *js, const char *filename)
{
    if (!js || !js->ctx || !filename)
        return -1;

    /* Re-anchor JS stack base — see the stack-top contract block in
     * vt_js_init. After the runtime-factory refactor (item K) this
     * function is called from a shallower C stack than JS_NewRuntime,
     * so we update before JS_Eval to keep the check accurate. */
    JS_UpdateStackTop(js->rt);

    /* Extract app directory from filename (needed regardless of source) */
    size_t fn_len = strlen(filename);
    char *app_dir = hl_alloc_malloc(js->base.alloc, fn_len + 1);
    if (!app_dir)
        return -1;
    memcpy(app_dir, filename, fn_len + 1);
    char *last_slash = strrchr(app_dir, '/');
    if (last_slash)
        *last_slash = '\0';
    else {
        hl_alloc_free(js->base.alloc, app_dir, fn_len + 1);
        app_dir = hl_alloc_malloc(js->base.alloc, 2);
        if (!app_dir)
            return -1;
        app_dir[0] = '.';
        app_dir[1] = '\0';
        fn_len = 1;
    }
    js->app_dir = app_dir;
    js->app_dir_size = fn_len + 1;

    /* Try embedded VFS entry first (hull build binaries).
     * Convert filename to VFS name: "./basename"
     * e.g. "app.js" → "./app.js", "/path/to/app.js" → "./app.js" */
    if (js->base.app_vfs && js->base.app_vfs->count > 0) {
        const char *base = strrchr(filename, '/');
        base = base ? base + 1 : filename;
        char vfs_name[256];
        size_t blen = strlen(base);
        if (blen + 3 <= sizeof(vfs_name)) {
            vfs_name[0] = '.';
            vfs_name[1] = '/';
            memcpy(vfs_name + 2, base, blen + 1);

            const HlEntry *e = hl_vfs_find(js->base.app_vfs, vfs_name);
            if (e) {
                /* QuickJS lexer requires '\0' sentinel */
                size_t arena_saved = js->scratch->used;
                char *buf = sh_arena_alloc(js->scratch, (size_t)e->len + 1);
                if (!buf)
                    return -1;
                memcpy(buf, e->data, e->len);
                buf[e->len] = '\0';

                JSValue val = JS_Eval(js->ctx, buf, e->len, filename,
                                      JS_EVAL_TYPE_MODULE);
                js->scratch->used = arena_saved;

                if (JS_IsException(val)) {
                    hl_js_dump_error(js);
                    return -1;
                }
                JS_FreeValue(js->ctx, val);
                sh_arena_reset(js->scratch);
                return 0;
            }
        }
    }

    /* Load from filesystem (development mode) */
    FILE *f = fopen(filename, "rb");
    if (!f) {
        log_error("[hull:c] cannot open %s", filename);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 0 || size > HL_MODULE_MAX_SIZE) {
        fclose(f);
        log_error("[hull:c] %s too large", filename);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    /* Save arena position — buffer is only needed until
     * JS_Eval copies it into QuickJS bytecode. */
    size_t arena_saved = js->scratch->used;

    char *buf = sh_arena_alloc(js->scratch, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    int read_err = ferror(f);
    fclose(f);
    if (read_err || nread != (size_t)size) {
        js->scratch->used = arena_saved;
        return -1;
    }
    buf[nread] = '\0';

    /* Evaluate as ES module */
    JSValue val = JS_Eval(js->ctx, buf, nread, filename,
                          JS_EVAL_TYPE_MODULE);

    /* Reclaim file buffer — QuickJS owns the bytecode now */
    js->scratch->used = arena_saved;

    if (JS_IsException(val)) {
        hl_js_dump_error(js);
        return -1;
    }
    JS_FreeValue(js->ctx, val);

    /* Reset scratch arena — startup module loads no longer needed */
    sh_arena_reset(js->scratch);

    return 0;
}

int hl_js_run_jobs(HlJS *js)
{
    if (!js || !js->ctx)
        return 0;

    int count = 0;
    JSContext *ctx1;
    for (;;) {
        int ret = JS_ExecutePendingJob(js->rt, &ctx1);
        if (ret <= 0)
            break;
        count++;
    }
    return count;
}

void hl_js_gc(HlJS *js)
{
    if (js && js->rt)
        JS_RunGC(js->rt);
}

void hl_js_reset_request(HlJS *js)
{
    if (!js)
        return;
    js->instruction_count = 0;
    if (js->scratch)
        sh_arena_reset(js->scratch);
}

/* Forward declaration for WS client tracking */
typedef struct HlJSWsClientUD HlJSWsClientUD;

void hl_js_free(HlJS *js)
{
    if (!js)
        return;

    /* Cancel and free tracked timers — via async backend vtable. */
    {
        const HlAsyncBackend *be = hl_async_backend();
        for (size_t i = 0; i < js->timer_count; i++) {
            HlJSTimer *t = (HlJSTimer *)js->timers[i];
            if (t->timer_id > 0 && js->base.async_ctx)
                be->timer_cancel(js->base.async_ctx, (uint64_t)t->timer_id);
            hl_alloc_free(js->base.alloc, t, sizeof(HlJSTimer));
        }
    }
    if (js->timers) {
        hl_alloc_free(js->base.alloc, js->timers,
                      js->timer_cap * sizeof(void *));
        js->timers = NULL;
        js->timer_count = 0;
        js->timer_cap = 0;
    }

    /* Free tracked route allocations (and per-route multipart configs
     * stashed by hl_js_wire_routes_server). */
    for (size_t i = 0; i < js->route_count; i++) {
        HlJSRoute *r = (HlJSRoute *)js->routes[i];
        if (r && r->multipart_config) {
            hl_alloc_free(js->base.alloc, r->multipart_config,
                          sizeof(KlMultipartConfig));
        }
        hl_alloc_free(js->base.alloc, r, sizeof(HlJSRoute));
    }
    if (js->routes) {
        hl_alloc_free(js->base.alloc, js->routes,
                      js->route_cap * sizeof(void *));
        js->routes = NULL;
        js->route_count = 0;
        js->route_cap = 0;
    }

    /* Free tracked WS route allocations */
    for (size_t i = 0; i < js->ws_route_count; i++)
        hl_alloc_free(js->base.alloc, js->ws_routes[i], sizeof(HlJSWsRoute));
    if (js->ws_routes) {
        hl_alloc_free(js->base.alloc, js->ws_routes,
                      js->ws_route_cap * sizeof(void *));
        js->ws_routes = NULL;
        js->ws_route_count = 0;
        js->ws_route_cap = 0;
    }

    /* Free tracked WS config allocations */
    for (size_t i = 0; i < js->ws_cfg_count; i++)
        hl_alloc_free(js->base.alloc, js->ws_cfgs[i],
                      sizeof(KlWsServerConfig));
    if (js->ws_cfgs) {
        hl_alloc_free(js->base.alloc, js->ws_cfgs,
                      js->ws_cfg_cap * sizeof(void *));
        js->ws_cfgs = NULL;
        js->ws_cfg_count = 0;
        js->ws_cfg_cap = 0;
    }

    /* Free tracked SSE route allocations */
    for (size_t i = 0; i < js->sse_route_count; i++)
        hl_alloc_free(js->base.alloc, js->sse_routes[i], sizeof(HlJSSseRoute));
    if (js->sse_routes) {
        hl_alloc_free(js->base.alloc, js->sse_routes,
                      js->sse_route_cap * sizeof(void *));
        js->sse_routes = NULL;
        js->sse_route_count = 0;
        js->sse_route_cap = 0;
    }

    /* Free tracked WS client allocations */
    if (js->ws_clients) {
        hl_alloc_free(js->base.alloc, js->ws_clients,
                      js->ws_client_cap * sizeof(void *));
        js->ws_clients = NULL;
        js->ws_client_count = 0;
        js->ws_client_cap = 0;
    }

    /* Free WebSocket registry — HTTP-only; CLI builds never create one. */
#ifdef HL_ENABLE_HTTP_SERVER
    if (js->base.ws_registry) {
        hl_ws_registry_free(js->base.ws_registry);
        hl_alloc_free(js->base.alloc, js->base.ws_registry,
                      sizeof(HlWsRegistry));
        js->base.ws_registry = NULL;
    }
#endif

    /* Mark runtime as dead before JS_FreeContext/JS_FreeRuntime so UDF
     * destroy callbacks (fired by sqlite3_close) don't call JS_FreeValue
     * on a dead runtime */
    js->udf_runtime_alive = 0;

    if (js->ctx) {
        /* Free test state opaque data before deleting globals.
         * mod_test (and the test backend) is HTTP-only. */
#ifdef HL_ENABLE_HTTP_SERVER
        hl_js_test_free(js->ctx);
#endif

        /* Delete hull internal globals so GC can collect them */
        JSValue global = JS_GetGlobalObject(js->ctx);
        static const char *hull_globals[] = {
            "console", "hull",
            "__hull_routes", "__hull_route_defs",
            "__hull_middleware", "__hull_post_middleware",
            "__hull_manifest",
            "__hull_test_state", "__hull_async_promise", "test",
            "__hull_timers", "__hull_timer_defs",
            "__hull_ws_defs", "__hull_sse_defs",
        };
        for (size_t i = 0; i < sizeof(hull_globals)/sizeof(hull_globals[0]); i++) {
            JSAtom atom = JS_NewAtom(js->ctx, hull_globals[i]);
            JS_DeleteProperty(js->ctx, global, atom, 0);
            JS_FreeAtom(js->ctx, atom);
        }
        JS_FreeValue(js->ctx, global);
        JS_RunGC(js->rt);

        JS_FreeContext(js->ctx);
        js->ctx = NULL;
    }
    if (js->rt) {
        JS_FreeRuntime(js->rt);
        js->rt = NULL;
    }
    if (js->app_dir) {
        hl_alloc_free(js->base.alloc, (void *)js->app_dir, js->app_dir_size);
        js->app_dir = NULL;
        js->app_dir_size = 0;
    }
    hl_arena_free(js->base.alloc, js->scratch);
    js->scratch = NULL;
}

void hl_js_dump_error(HlJS *js)
{
    if (!js || !js->ctx)
        return;

    JSValue exception = JS_GetException(js->ctx);
    const char *str = JS_ToCString(js->ctx, exception);
    if (str) {
        log_error("[hull:c] js error: %s", str);
        JS_FreeCString(js->ctx, str);
    }

    /* Print stack trace if available */
    if (JS_IsError(js->ctx, exception)) {
        JSValue stack = JS_GetPropertyStr(js->ctx, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *stack_str = JS_ToCString(js->ctx, stack);
            if (stack_str) {
                log_error("[hull:c] %s", stack_str);
                JS_FreeCString(js->ctx, stack_str);
            }
        }
        JS_FreeValue(js->ctx, stack);
    }
    JS_FreeValue(js->ctx, exception);
}

/* ── Embedded JS stdlib registration ────────────────────────────────── */

int hl_js_register_stdlib(HlJS *js)
{
    if (!js || !js->ctx)
        return -1;

    /* Embedded JS stdlib modules are loaded on-demand by the module
     * loader (hl_js_module_loader checks hl_stdlib_js_entries[] for
     * hull:* names not found as native C modules). No eager compilation
     * needed — just verify the registry is accessible. */

    (void)js->base.platform_vfs; /* ensure linked */
    return 0;
}

/* ── Vtable adapters ───────────────────────────────────────────────── */

static int vt_js_init(HlRuntime *rt, const void *config)
{
    return hl_js_init((HlJS *)rt, (const HlJSConfig *)config);
}

static int vt_js_load_app(HlRuntime *rt, const char *filename)
{
    return hl_js_load_app((HlJS *)rt, filename);
}

#ifdef HL_ENABLE_HTTP_SERVER
static int vt_js_wire_routes_server(HlRuntime *rt, KlServer *server,
                                     void *(*alloc_fn)(size_t))
{
    return hl_js_wire_routes_server((HlJS *)rt, server, alloc_fn);
}
#else
/* CLI-only build: the vtable still has the slot for layout
 * compatibility, but it should never be invoked (serve.c, the only
 * caller, doesn't get compiled in). */
static int vt_js_wire_routes_server(HlRuntime *rt, KlServer *server,
                                     void *(*alloc_fn)(size_t))
{
    (void)rt; (void)server; (void)alloc_fn;
    return -1;
}
#endif

static int vt_js_extract_manifest(HlRuntime *rt, HlManifest *out)
{
    HlJS *js = (HlJS *)rt;
    return hl_manifest_extract_js(js->ctx, out, js->base.alloc);
}

/* Walk a JS array property of globalThis, calling cb with method+pattern.
 * Shared body for route + middleware enumeration; the visitor differs. */
typedef struct { const char *method, *pattern; } JsRouteRow;

static int js_read_route_array(JSContext *ctx, JSValue global, const char *prop,
                               void (*emit)(void *user, const JsRouteRow *r, const char *phase),
                               void *user, const char *phase)
{
    JSValue arr = JS_GetPropertyStr(ctx, global, prop);
    int seen = 0;
    if (!JS_IsUndefined(arr) && !JS_IsNull(arr)) {
        JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        for (int32_t i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            JSValue m = JS_GetPropertyStr(ctx, item, "method");
            JSValue p = JS_GetPropertyStr(ctx, item, "pattern");
            const char *ms = JS_ToCString(ctx, m);
            const char *ps = JS_ToCString(ctx, p);
            JsRouteRow row = { .method = ms, .pattern = ps };
            emit(user, &row, phase);
            seen++;
            JS_FreeCString(ctx, ms);
            JS_FreeCString(ctx, ps);
            JS_FreeValue(ctx, m);
            JS_FreeValue(ctx, p);
            JS_FreeValue(ctx, item);
        }
    }
    JS_FreeValue(ctx, arr);
    return seen;
}

typedef struct { HlRouteCb cb; void *user; } RouteEmitCtx;
static void route_emit(void *user, const JsRouteRow *r, const char *phase)
{
    (void)phase;
    RouteEmitCtx *c = user;
    c->cb(c->user, r->method, r->pattern);
}

static void vt_js_enumerate_routes(HlRuntime *rt, HlRouteCb cb, void *user)
{
    if (!cb) return;
    HlJS *js = (HlJS *)rt;
    JSValue global = JS_GetGlobalObject(js->ctx);
    RouteEmitCtx c = { .cb = cb, .user = user };
    js_read_route_array(js->ctx, global, "__hull_route_defs", route_emit, &c, NULL);
    JS_FreeValue(js->ctx, global);
}

typedef struct { HlMiddlewareCb cb; void *user; } MwEmitCtx;
static void mw_emit(void *user, const JsRouteRow *r, const char *phase)
{
    MwEmitCtx *c = user;
    c->cb(c->user, r->method, r->pattern, phase);
}

static void vt_js_enumerate_middleware(HlRuntime *rt, HlMiddlewareCb cb, void *user)
{
    if (!cb) return;
    HlJS *js = (HlJS *)rt;
    JSValue global = JS_GetGlobalObject(js->ctx);
    MwEmitCtx c = { .cb = cb, .user = user };
    js_read_route_array(js->ctx, global, "__hull_middleware",      mw_emit, &c, "pre");
    js_read_route_array(js->ctx, global, "__hull_post_middleware", mw_emit, &c, "post");
    JS_FreeValue(js->ctx, global);
}

#ifdef HL_ENABLE_HTTP_SERVER
static int vt_js_test_setup(HlRuntime *rt, KlRouter *router)
{
    HlJS *js = (HlJS *)rt;
    if (hl_js_wire_routes(js, router) != 0)
        return -1;
    hl_js_test_register(js->ctx, router, js);
    return 0;
}
#else
/* CLI-only build stub: hull test against a CLI app needs a different
 * harness (test.run_main) — placeholder so the vtable still links. */
static int vt_js_test_setup(HlRuntime *rt, KlRouter *router)
{
    (void)rt; (void)router;
    return -1;
}
#endif

static int vt_js_run_test_file(HlRuntime *rt, const char *file_path,
                               HlTestCaseResult *results, int max_results,
                               int *file_total, int *file_passed, int *file_failed,
                               const char **load_err)
{
#ifndef HL_ENABLE_HTTP_SERVER
    (void)rt; (void)file_path; (void)results; (void)max_results;
    (void)file_total; (void)file_passed; (void)file_failed;
    if (load_err) *load_err = "test runner unavailable on HTTP=0 builds";
    return -1;
#else
    static const char *err_open   = "cannot open file";
    static const char *err_read   = "cannot read file";
    static const char *err_oom    = "out of memory";
    HlJS *js = (HlJS *)rt;
    hl_js_test_clear(js->ctx);

    FILE *f = fopen(file_path, "r");
    if (!f) { if (load_err) *load_err = err_open; return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); if (load_err) *load_err = err_read; return -1; }
    long flen = ftell(f);
    if (flen < 0) { fclose(f); if (load_err) *load_err = err_read; return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); if (load_err) *load_err = err_read; return -1; }
    char *src = malloc((size_t)flen + 1);
    if (!src) { fclose(f); if (load_err) *load_err = err_oom; return -1; }
    if (fread(src, 1, (size_t)flen, f) != (size_t)flen) {
        free(src); fclose(f); if (load_err) *load_err = err_read; return -1;
    }
    src[flen] = '\0';
    fclose(f);

    /* Reset stack base — see the stack-top contract block in
     * vt_js_init (above). The test runner adds C frames between
     * JS_NewRuntime and JS_Eval, so without this anchoring even a
     * trivial test file trips "stack overflow". */
    JS_UpdateStackTop(js->rt);

    JSValue result = JS_Eval(js->ctx, src, (size_t)flen, file_path,
                             JS_EVAL_TYPE_MODULE);
    free(src);

    /* JS_Eval error message — caller receives a pointer to load_err_buf,
     * which lives until the next vt_js_run_test_file call. Hull's test
     * runner is single-threaded; if that ever changes, this becomes
     * per-HlJS state. */
    static char load_err_buf[1024];

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(js->ctx);
        JSValue msg_val = JS_GetPropertyStr(js->ctx, exc, "message");
        const char *msg = JS_ToCString(js->ctx, msg_val);
        snprintf(load_err_buf, sizeof(load_err_buf), "%s", msg ? msg : "unknown");
        if (msg) JS_FreeCString(js->ctx, msg);
        JS_FreeValue(js->ctx, msg_val);
        JS_FreeValue(js->ctx, exc);
        JS_FreeValue(js->ctx, result);
        if (load_err) *load_err = load_err_buf;
        return -1;
    }
    JS_FreeValue(js->ctx, result);

    /* hl_js_test_run unconditionally writes to file_total/passed/failed.
     * The shared runner always passes non-NULL; same reasoning as the
     * Lua side. */
    hl_js_test_run(js->ctx, file_total, file_passed, file_failed,
                   NULL, results, max_results);
    return 0;
#endif
}

static void vt_js_destroy(HlRuntime *rt)
{
    hl_js_free((HlJS *)rt);
}

/* ── CLI mode (app.main) ───────────────────────────────────────────────
 *
 * Phase 1: sync stdio (calls return values directly, not Promises). Full
 * async-inside-main (compute.async / gpu.async / http.fetch) needs Keel
 * event-loop integration in run_main_mode; deferred to Phase 1.5. The
 * synchronous Promise machinery in QuickJS (Promise.resolve, microtask
 * chains) still works because we drain pending jobs after main returns. */

static FILE *cli_stream_for(int magic)
{
    switch (magic) {
    case 0: return stdin;
    case 1: return stdout;
    case 2: return stderr;
    default: return NULL;
    }
}

static JSValue js_cli_read(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    FILE *f = cli_stream_for(magic);
    if (!f) return JS_ThrowInternalError(ctx, "invalid stream");

    /* read()        → "*l" line read
     * read("a"|"l") → that mode
     * read(n)       → n bytes */
    int want_bytes = 0;
    int64_t nbytes = 0;
    /* keep[] lives for the whole function so `mode` remains valid
     * after the parse block exits. Previously `keep` was declared
     * inside the parse block and `mode` was set to point into it —
     * technically a dangling pointer once the block went out of scope
     * (cppcheck flags as invalidLifetime). Behaviour was de-facto fine
     * because no other stack slot reused the bytes, but the lifetime
     * is now correct by construction. */
    char keep[2] = {'l', 0};
    const char *mode = keep;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsNumber(argv[0])) {
            if (JS_ToInt64(ctx, &nbytes, argv[0]) != 0) return JS_EXCEPTION;
            if (nbytes < 0) return JS_ThrowRangeError(ctx, "read: negative count");
            want_bytes = 1;
        } else {
            const char *s = JS_ToCString(ctx, argv[0]);
            if (!s) return JS_EXCEPTION;
            const char *src = (s[0] == '*' ? s + 1 : s);
            if (src[0] != 'a' && src[0] != 'l') {
                JSValue err = JS_ThrowRangeError(ctx,
                    "read: unsupported mode '%s' (use 'l', 'a', or n bytes)", s);
                JS_FreeCString(ctx, s);
                return err;
            }
            keep[0] = src[0];  /* mode already points at keep */
            JS_FreeCString(ctx, s);
        }
    }

    if (want_bytes) {
        if (nbytes == 0) return JS_NewString(ctx, "");
        char *buf = js_malloc(ctx, (size_t)nbytes + 1);
        if (!buf) return JS_EXCEPTION;
        size_t n = fread(buf, 1, (size_t)nbytes, f);
        buf[n] = 0;
        if (n == 0 && feof(f)) { js_free(ctx, buf); return JS_NULL; }
        JSValue r = JS_NewStringLen(ctx, buf, n);
        js_free(ctx, buf);
        return r;
    }

    if (mode[0] == 'a') {
        size_t cap = 4096, len = 0;
        char *buf = js_malloc(ctx, cap);
        if (!buf) return JS_EXCEPTION;
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (len + n + 1 > cap) {
                while (len + n + 1 > cap) cap *= 2;
                char *nb = js_realloc(ctx, buf, cap);
                if (!nb) { js_free(ctx, buf); return JS_EXCEPTION; }
                buf = nb;
            }
            memcpy(buf + len, chunk, n);
            len += n;
        }
        JSValue r = JS_NewStringLen(ctx, buf, len);
        js_free(ctx, buf);
        return r;
    }

    /* default: line read */
    size_t cap = 256, len = 0;
    char *buf = js_malloc(ctx, cap);
    if (!buf) return JS_EXCEPTION;
    int got = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        got = 1;
        if (c == '\n') break;
        if (len + 2 > cap) {
            cap *= 2;
            char *nb = js_realloc(ctx, buf, cap);
            if (!nb) { js_free(ctx, buf); return JS_EXCEPTION; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (!got) { js_free(ctx, buf); return JS_NULL; }
    JSValue r = JS_NewStringLen(ctx, buf, len);
    js_free(ctx, buf);
    return r;
}

static JSValue js_cli_write(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    FILE *f = cli_stream_for(magic);
    if (!f) return JS_ThrowInternalError(ctx, "invalid stream");
    for (int i = 0; i < argc; i++) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, argv[i]);
        if (!s) return JS_EXCEPTION;
        if (n && fwrite(s, 1, n, f) != n) {
            JS_FreeCString(ctx, s);
            return JS_ThrowInternalError(ctx, "write failed");
        }
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue js_cli_flush(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    FILE *f = cli_stream_for(magic);
    if (f) fflush(f);
    return JS_UNDEFINED;
}

static JSValue js_cli_close(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    /* No-op for stdin; we don't want to propagate fclose(stdin) to
     * other readers in the process. */
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_UNDEFINED;
}

/* .then / .catch callback wired to main's Promise in CLI mode. magic=0
 * → resolved, magic=1 → rejected. Captures the value on the runtime
 * and stops the event loop so vt_js_run_main can return. */
static JSValue js_cli_main_settle(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    HlJS *js = JS_GetContextOpaque(ctx);
    if (!js || !js->cli_main_value) return JS_UNDEFINED;

    JSValue *slot = (JSValue *)js->cli_main_value;
    JS_FreeValue(ctx, *slot);
    *slot = (argc >= 1) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    js->cli_main_rejected = (magic == 1) ? 1 : 0;
    js->cli_main_active   = 0;

    /* Tell the backend's event loop to exit so vt_js_run_main returns.
     * Works for both keel (kl_server_stop equivalent through the
     * wrapped event ctx) and the poll backend. */
    if (js->base.async_ctx)
        hl_async_backend()->stop(js->base.async_ctx);
    return JS_UNDEFINED;
}

static int vt_js_has_main(HlRuntime *rt)
{
    if (!rt) return 0;
    HlJS *js = (HlJS *)rt;
    if (!js->ctx) return 0;
    JSValue global = JS_GetGlobalObject(js->ctx);
    JSValue m = JS_GetPropertyStr(js->ctx, global, "__hull_main");
    int has = !JS_IsUndefined(m) && !JS_IsNull(m);
    JS_FreeValue(js->ctx, m);
    JS_FreeValue(js->ctx, global);
    return has;
}

/* True iff any route/middleware/timer/ws/sse handler has been
 * registered. Same set of globalThis keys js_app_dispatch_registered
 * walks in mod_app.c. */
static int vt_js_has_server_handlers(HlRuntime *rt)
{
    if (!rt) return 0;
    HlJS *js = (HlJS *)rt;
    if (!js->ctx) return 0;
    static const char *keys[] = {
        "__hull_route_defs", "__hull_middleware", "__hull_post_middleware",
        "__hull_timer_defs", "__hull_ws_defs", "__hull_sse_defs",
        NULL
    };
    JSValue global = JS_GetGlobalObject(js->ctx);
    int found = 0;
    for (int i = 0; keys[i] && !found; i++) {
        JSValue v = JS_GetPropertyStr(js->ctx, global, keys[i]);
        if (JS_IsArray(js->ctx, v)) {
            JSValue len_val = JS_GetPropertyStr(js->ctx, v, "length");
            uint32_t len = 0;
            JS_ToUint32(js->ctx, &len, len_val);
            JS_FreeValue(js->ctx, len_val);
            if (len > 0) found = 1;
        }
        JS_FreeValue(js->ctx, v);
    }
    JS_FreeValue(js->ctx, global);
    return found;
}

static int coerce_js_exit_code(JSContext *ctx, JSValue v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) return 0;
    if (JS_IsNumber(v)) {
        int32_t i = 0;
        if (JS_ToInt32(ctx, &i, v) != 0) return 1;
        if (i < 0) i = 0;
        if (i > 255) i = i & 0xff;
        return i;
    }
    const char *s = JS_ToCString(ctx, v);
    fprintf(stderr, "[hull:main] non-numeric return: %s\n",
            s ? s : "(unprintable)");
    if (s) JS_FreeCString(ctx, s);
    return 1;
}

static int vt_js_run_main(HlRuntime *rt, KlServer *server,
                           int argc, char **argv,
                           const char *const *env_allowlist,
                           int *exit_code_out)
{
    if (exit_code_out) *exit_code_out = 1;
    if (!rt || !exit_code_out) return -1;
    HlJS *js = (HlJS *)rt;
    JSContext *ctx = js->ctx;

    /* Same reasoning as the Lua side: CLI bypasses wire_routes_server,
     * so the server pointer isn't wired. Async ops (hull.sleep,
     * compute.async, gpu.async, http.fetch) reach for js->server. */
    if (server) js->server = server;

    /* Look up the main function. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue main_fn = JS_GetPropertyStr(ctx, global, "__hull_main");
    if (!JS_IsFunction(ctx, main_fn)) {
        JS_FreeValue(ctx, main_fn);
        JS_FreeValue(ctx, global);
        return -1;
    }

    /* Build ctx object. */
    JSValue ctxobj = JS_NewObject(ctx);

    /* ctx.args = [...argv] */
    JSValue args_arr = JS_NewArray(ctx);
    for (int i = 0; i < argc; i++) {
        JS_SetPropertyUint32(ctx, args_arr, (uint32_t)i,
                             JS_NewString(ctx, argv[i] ? argv[i] : ""));
    }
    JS_SetPropertyStr(ctx, ctxobj, "args", args_arr);

    /* ctx.env = { NAME: value, ... } from env_allowlist */
    JSValue env_obj = JS_NewObject(ctx);
    if (env_allowlist) {
        for (int i = 0; env_allowlist[i]; i++) {
            const char *name = env_allowlist[i];
            const char *val = getenv(name);
            if (val)
                JS_SetPropertyStr(ctx, env_obj, name, JS_NewString(ctx, val));
        }
    }
    JS_SetPropertyStr(ctx, ctxobj, "env", env_obj);

    /* ctx.stdin / stdout / stderr */
    struct { const char *prop; int magic; int writer; } streams[] = {
        { "stdin",  0, 0 },
        { "stdout", 1, 1 },
        { "stderr", 2, 1 },
    };
    for (size_t i = 0; i < sizeof(streams)/sizeof(streams[0]); i++) {
        JSValue s = JS_NewObject(ctx);
        if (streams[i].writer) {
            JS_SetPropertyStr(ctx, s, "write",
                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_cli_write,
                                     "write", 1, JS_CFUNC_generic_magic,
                                     streams[i].magic));
            JS_SetPropertyStr(ctx, s, "flush",
                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_cli_flush,
                                     "flush", 0, JS_CFUNC_generic_magic,
                                     streams[i].magic));
        } else {
            JS_SetPropertyStr(ctx, s, "read",
                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_cli_read,
                                     "read", 1, JS_CFUNC_generic_magic,
                                     streams[i].magic));
            JS_SetPropertyStr(ctx, s, "close",
                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_cli_close,
                                     "close", 0, JS_CFUNC_generic_magic,
                                     streams[i].magic));
        }
        JS_SetPropertyStr(ctx, ctxobj, streams[i].prop, s);
    }

    /* Call main(ctx). */
    JSValue call_argv[1] = { ctxobj };
    JSValue ret = JS_Call(ctx, main_fn, JS_UNDEFINED, 1, call_argv);
    JS_FreeValue(ctx, ctxobj);

    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, main_fn);
        JS_FreeValue(ctx, global);
        return -1;
    }

    /* Drain microtasks — resolves any synchronously-resolvable Promise. */
    hl_js_run_jobs(js);

    /* Branch on whether main returned a Promise. */
    JSValue value = ret;
    int owned_value = 0;
    int rejected = 0;
    JSPromiseStateEnum st = JS_PromiseState(ctx, ret);

    if (st == JS_PROMISE_PENDING) {
        /* Async main — attach C-side .then/.catch callbacks that stash
         * the resolved value on the runtime and stop the server, then
         * drive the Keel event loop until they fire. */
        if (!server) {
            fprintf(stderr, "[hull:main] internal: no server to drive event loop\n");
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, main_fn);
            JS_FreeValue(ctx, global);
            return -1;
        }

        JSValue settled_slot = JS_UNDEFINED;
        js->cli_main_active   = 1;
        js->cli_main_rejected = 0;
        js->cli_main_value    = &settled_slot;

        JSValue then_fn = JS_GetPropertyStr(ctx, ret, "then");
        JSValue resolve_cb = JS_NewCFunctionMagic(
            ctx, (JSCFunctionMagic *)js_cli_main_settle,
            "__hull_main_resolved", 1, JS_CFUNC_generic_magic, 0);
        JSValue reject_cb = JS_NewCFunctionMagic(
            ctx, (JSCFunctionMagic *)js_cli_main_settle,
            "__hull_main_rejected", 1, JS_CFUNC_generic_magic, 1);
        JSValue then_args[2] = { resolve_cb, reject_cb };
        JSValue chained = JS_Call(ctx, then_fn, ret, 2, then_args);
        JS_FreeValue(ctx, then_fn);
        JS_FreeValue(ctx, resolve_cb);
        JS_FreeValue(ctx, reject_cb);
        JS_FreeValue(ctx, chained);

        hl_js_run_jobs(js);  /* in case both already settled via microtasks */

        if (js->cli_main_active) {
            /* Drive the event loop via the async backend. On HTTP=1
             * this is a wrap around KlServer's KlEventCtx; on HTTP=0
             * it's the poll backend's standalone loop. The .then
             * callbacks attached above (js_cli_main_settle) call
             * backend->stop when main's promise settles. */
            (void)server;  /* unused — we go through the backend now */
            int srv_rc = (js->base.async_ctx)
                ? hl_async_backend()->run(js->base.async_ctx)
                : -1;
            if (srv_rc < 0)
                fprintf(stderr, "[hull:main] event loop error\n");
            hl_js_run_jobs(js);  /* drain anything that landed after stop */
        }

        value = settled_slot;
        owned_value = 1;
        rejected = js->cli_main_rejected;
        js->cli_main_value = NULL;
        js->cli_main_active = 0;
    } else if (st == JS_PROMISE_FULFILLED || st == JS_PROMISE_REJECTED) {
        value = JS_PromiseResult(ctx, ret);
        owned_value = 1;
        rejected = (st == JS_PROMISE_REJECTED);
    }

    int rc;
    if (rejected) {
        const char *msg = JS_ToCString(ctx, value);
        fprintf(stderr, "[hull:main] promise rejected: %s\n",
                msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(ctx, msg);
        rc = 1;
    } else {
        rc = coerce_js_exit_code(ctx, value);
    }

    if (owned_value) JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, main_fn);
    JS_FreeValue(ctx, global);

    *exit_code_out = rc;
    return rejected ? -1 : 0;
}

const HlRuntimeVtable hl_js_vtable = {
    .init                 = vt_js_init,
    .load_app             = vt_js_load_app,
    .wire_routes_server   = vt_js_wire_routes_server,
    .extract_manifest     = vt_js_extract_manifest,
    .enumerate_routes     = vt_js_enumerate_routes,
    .enumerate_middleware = vt_js_enumerate_middleware,
    .test_setup           = vt_js_test_setup,
    .run_test_file        = vt_js_run_test_file,
    .destroy              = vt_js_destroy,
    .name                 = "QuickJS",
    .test_file_pattern    = "test_*.js",
    .has_main             = vt_js_has_main,
    .has_server_handlers  = vt_js_has_server_handlers,
    .run_main             = vt_js_run_main,
};
