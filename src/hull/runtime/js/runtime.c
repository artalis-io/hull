/*
 * js_runtime.c — QuickJS runtime for Hull
 *
 * Initializes QuickJS with sandboxing: no eval, no std/os modules,
 * custom allocator, memory limits, instruction-count interrupt handler,
 * and custom module loader for hull:* built-in modules.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "hull/reqctx.h"
#include "hull/async.h"
#include "hull/alloc.h"
#include "hull/limits.h"
#include "hull/manifest.h"
#include "hull/cap/body.h"
#include "hull/cap/fs.h"
#include "hull/cap/env.h"
#include "hull/cap/http.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/test.h"
#include "quickjs.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>
#include <keel/websocket_client.h>
#include <keel/sse.h>

#include "hull/cap/ws.h"

#include "mod_buffer.h"

#include <sh_arena.h>

#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VFS: O(log n) lookups into sorted entry arrays */
#include "hull/vfs.h"

/* ── Forward declarations ───────────────────────────────────────────── */

/* Defined in js/async.c — registers hull global (sleep, etc.) */
extern void hl_js_add_hull_global(JSContext *ctx);

/* Defined in js/async.c — sets handler promise and timer ctx on continuation */
extern void hl_js_async_cont_set_handler_promise(HlAsyncCont *cont,
                                                   JSContext *ctx,
                                                   JSValue promise);
extern void hl_js_async_cont_set_timer(HlAsyncCont *cont, void *timer);

#include <time.h>

/* ── Forward declarations for module init functions ─────────────────── */

int hl_js_init_app_module(JSContext *ctx, HlJS *js);
int hl_js_init_db_module(JSContext *ctx, HlJS *js);
int hl_js_init_json_module(JSContext *ctx, HlJS *js);
int hl_js_init_time_module(JSContext *ctx, HlJS *js);
int hl_js_init_env_module(JSContext *ctx, HlJS *js);
int hl_js_init_crypto_module(JSContext *ctx, HlJS *js);
int hl_js_init_log_module(JSContext *ctx, HlJS *js);
int hl_js_init_smtp_module(JSContext *ctx, HlJS *js);
int hl_js_init_template_module(JSContext *ctx, HlJS *js);

/* ── Forward declarations for binding helpers (defined in bindings.c) ─ */

JSValue hl_js_make_request(JSContext *ctx, KlRequest *req);
JSValue hl_js_make_response(HlJS *js, KlResponse *res);

/* ── Interrupt handler (gas metering) ───────────────────────────────── */

static int hl_js_interrupt_handler(JSRuntime *rt, void *opaque)
{
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
 * Reject module names that contain path traversal sequences.
 * Returns 0 if safe, -1 if the name contains ".." or is absolute.
 */
static int hl_js_validate_module_name(const char *name)
{
    /* Reject absolute paths */
    if (name[0] == '/')
        return -1;

    /* Reject ".." path components */
    for (const char *p = name; *p; ) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0'))
            return -1;
        const char *slash = strchr(p, '/');
        if (!slash) break;
        p = slash + 1;
    }

    /* Reject embedded null bytes (shouldn't happen, but defense-in-depth) */
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

            /* Verify the resolved path doesn't introduce ".." traversal.
             * The resolved path may legitimately start with '/' when the
             * base module was loaded from an absolute filesystem path (dev
             * mode), so only reject ".." components, not absolute paths. */
            int has_dotdot = 0;
            for (const char *p = resolved; *p && !has_dotdot; ) {
                if (p[0] == '.' && p[1] == '.' &&
                    (p[2] == '/' || p[2] == '\0'))
                    has_dotdot = 1;
                const char *sl = strchr(p, '/');
                if (!sl) break;
                p = sl + 1;
            }
            if (has_dotdot) {
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

    /* Build filesystem path.  The normalizer resolves relative imports
     * against the base module directory, which may prepend app_dir
     * (e.g. "examples/todo/./locales/en.json").  Strip that prefix
     * to avoid doubling when we prepend app_dir ourselves. */
    const char *fs_name = module_name;
    const char *dot_slash = strstr(module_name, "/./");
    if (dot_slash)
        fs_name = dot_slash + 1; /* points to "./" */
    char path[HL_MODULE_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", js->app_dir, fs_name);
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
    if (js->base.db_handle)
        hl_js_worker_db_init();

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

/* Forward struct defs needed by hl_js_free (full defs later in file) */
typedef struct HlJSWsRoute HlJSWsRoute;
typedef struct HlJSSseRoute HlJSSseRoute;
struct HlJSWsRoute {
    HlJS *js;
    int   on_open_id;
    int   on_message_id;
    int   on_close_id;
    char  path[256];
};
struct HlJSSseRoute {
    HlJS *js;
    int   handler_id;
};

/* Forward declarations for WS client tracking */
typedef struct HlJSWsClientUD HlJSWsClientUD;

void hl_js_free(HlJS *js)
{
    if (!js)
        return;

    /* Cancel and free tracked timers */
    for (size_t i = 0; i < js->timer_count; i++) {
        HlJSTimer *t = (HlJSTimer *)js->timers[i];
        if (t->timer_id >= 0 && js->server)
            kl_timer_cancel(&js->server->ev, t->timer_id);
        hl_alloc_free(js->base.alloc, t, sizeof(HlJSTimer));
    }
    if (js->timers) {
        hl_alloc_free(js->base.alloc, js->timers,
                      js->timer_cap * sizeof(void *));
        js->timers = NULL;
        js->timer_count = 0;
        js->timer_cap = 0;
    }

    /* Free tracked route allocations */
    for (size_t i = 0; i < js->route_count; i++)
        hl_alloc_free(js->base.alloc, js->routes[i], sizeof(HlJSRoute));
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

    /* Free WebSocket registry */
    if (js->base.ws_registry) {
        hl_ws_registry_free(js->base.ws_registry);
        hl_alloc_free(js->base.alloc, js->base.ws_registry,
                      sizeof(HlWsRegistry));
        js->base.ws_registry = NULL;
    }

    /* Mark runtime as dead before JS_FreeContext/JS_FreeRuntime so UDF
     * destroy callbacks (fired by sqlite3_close) don't call JS_FreeValue
     * on a dead runtime */
    js->udf_runtime_alive = 0;

    if (js->ctx) {
        /* Free test state opaque data before deleting globals */
        hl_cap_test_free_js(js->ctx);

        /* Delete hull internal globals so GC can collect them */
        JSValue global = JS_GetGlobalObject(js->ctx);
        static const char *hull_globals[] = {
            "console", "hull",
            "__hull_routes", "__hull_route_defs",
            "__hull_middleware", "__hull_post_middleware",
            "__hull_config", "__hull_manifest", "__hull_statics",
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

/* ── Timer support ─────────────────────────────────────────────────── */

static void hl_js_timer_trampoline(void *user_data);

static int64_t hl_js_compute_daily_delay_ms(int hour, int minute, int use_local)
{
    time_t now = time(NULL);
    struct tm now_tm;
    if (use_local)
        localtime_r(&now, &now_tm);
    else
        gmtime_r(&now, &now_tm);

    int64_t now_secs = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;
    int64_t target_secs = hour * 3600 + minute * 60;

    int64_t delta = target_secs - now_secs;
    if (delta <= 0)
        delta += 86400;

    return delta * 1000;
}

static int hl_js_track_timer(HlJS *js, void *timer)
{
    if (js->timer_count >= js->timer_cap) {
        size_t new_cap = js->timer_cap ? js->timer_cap * 2 : 4;
        if (new_cap < js->timer_cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1;
        size_t old_sz = js->timer_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(js->base.alloc,
                                           js->timers, old_sz, new_sz);
        if (!new_arr)
            return -1;
        js->timers = new_arr;
        js->timer_cap = new_cap;
    }
    js->timers[js->timer_count++] = timer;
    return 0;
}

void hl_js_timer_reschedule(HlJSTimer *t)
{
    HlJS *js = t->js;
    int64_t delay_ms;

    if (t->daily)
        delay_ms = hl_js_compute_daily_delay_ms(t->hour, t->minute, t->localtime);
    else
        delay_ms = t->interval_ms;

    t->timer_id = kl_timer_add(&js->server->ev, (uint64_t)delay_ms,
                                hl_js_timer_trampoline, t);
    if (t->timer_id < 0)
        log_error("[hull:timer] failed to reschedule timer (handler_id=%d)",
                  t->handler_id);
}

static void hl_js_timer_trampoline(void *user_data)
{
    HlJSTimer *t = (HlJSTimer *)user_data;
    HlJS *js = t->js;
    JSContext *ctx = js->ctx;

    /* Skip if previous invocation still in flight (async) */
    if (t->in_flight) {
        t->timer_id = kl_timer_add(&js->server->ev, 1000,
                                    hl_js_timer_trampoline, t);
        return;
    }

    t->in_flight = 1;

    /* Reset scratch arena + guard stale txn */
    sh_arena_reset(js->scratch);
    hl_db_guard_stale_txn(js->base.db_handle);

    assert(js->dispatch_depth == 0 && "timer fired during active dispatch");
    js->dispatch_depth++;

    /* Clear per-request state (no connection) */
    js->active_conn = NULL;
    js->active_req = NULL;
    js->last_async_cont = NULL;
    js->async_pending = 0;
    js->active_timer = t;

    /* Reset instruction counter */
    js->instruction_count = 0;

    /* Look up handler */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue timers = JS_GetPropertyStr(ctx, global, "__hull_timers");
    JSValue handler = JS_GetPropertyUint32(ctx, timers, (uint32_t)t->handler_id);
    JS_FreeValue(ctx, timers);
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        t->in_flight = 0;
        js->active_timer = NULL;
        hl_js_timer_reschedule(t);
        return;
    }

    /* Call handler() */
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, handler);

    if (JS_IsException(ret)) {
        JSValue exception = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exception);
        log_error("[hull:timer] %s", msg ? msg : "unknown error");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exception);
        JS_FreeValue(ctx, ret);
        t->in_flight = 0;
        js->active_timer = NULL;
        js->dispatch_depth--;
        hl_js_timer_reschedule(t);
        return;
    }

    JSPromiseStateEnum state = JS_PromiseState(ctx, ret);

    if (state == JS_PROMISE_PENDING) {
        /* Async handler — wire handler_promise on the continuation */
        if (js->last_async_cont) {
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
        }
        /* Timer ctx was already set via js->active_timer at cont creation */
        JS_FreeValue(ctx, ret);
        /* in_flight stays 1; dispatch_depth stays elevated.
         * Both cleared when async resume completes. */
        js->active_timer = NULL;
        return;
    }

    /* Synchronous completion */
    int cancelled = 0;
    if (state == JS_PROMISE_FULFILLED) {
        JSValue result = JS_PromiseResult(ctx, ret);
        if (JS_IsBool(result) && JS_ToBool(ctx, result) == 0)
            cancelled = 1;
        JS_FreeValue(ctx, result);
    } else if (state == JS_PROMISE_REJECTED) {
        JSValue result = JS_PromiseResult(ctx, ret);
        const char *msg = JS_ToCString(ctx, result);
        log_error("[hull:timer] %s", msg ? msg : "unknown error");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, result);
    } else {
        /* Sync return (not a promise) — check for false */
        if (JS_IsBool(ret) && JS_ToBool(ctx, ret) == 0)
            cancelled = 1;
    }

    /* Drain microtasks */
    hl_js_run_jobs(js);

    JS_FreeValue(ctx, ret);
    t->in_flight = 0;
    js->active_timer = NULL;
    js->dispatch_depth--;

    if (!cancelled)
        hl_js_timer_reschedule(t);
}

/* ── Route tracking ────────────────────────────────────────────────── */

static int hl_js_track_route(HlJS *js, void *route)
{
    if (js->route_count >= js->route_cap) {
        size_t new_cap = js->route_cap ? js->route_cap * 2 : 8;
        if (new_cap < js->route_cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1; /* overflow */
        size_t old_sz = js->route_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(js->base.alloc,
                                           js->routes, old_sz, new_sz);
        if (!new_arr)
            return -1;
        js->routes = new_arr;
        js->route_cap = new_cap;
    }
    js->routes[js->route_count++] = route;
    return 0;
}

/* ── Generic tracked-allocation helper ──────────────────────────────── */

static int hl_js_track_alloc(HlJS *js, void ***arr, size_t *count,
                               size_t *cap, void *ptr)
{
    if (*count >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 4;
        if (new_cap < *cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1;
        size_t old_sz = *cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(js->base.alloc,
                                           *arr, old_sz, new_sz);
        if (!new_arr)
            return -1;
        *arr = new_arr;
        *cap = new_cap;
    }
    (*arr)[(*count)++] = ptr;
    return 0;
}

/* ── WebSocket callback trampolines ────────────────────────────────── */

static void hl_js_ws_on_open(KlWsServerConn *ws_conn, void *user_data)
{
    HlJSWsRoute *route = (HlJSWsRoute *)user_data;
    HlJS *js = route->js;
    JSContext *ctx = js->ctx;

    /* Register the connection in the registry */
    HlWsConn *conn = hl_ws_registry_add(js->base.ws_registry,
                                          route->path, ws_conn);
    if (!conn)
        return;

    if (route->on_open_id < 0)
        return;

    js->dispatch_depth++;
    js->active_conn = NULL; /* detached — no HTTP connection */
    js->active_req = NULL;
    js->active_timer = NULL;
    js->last_async_cont = NULL;
    js->async_pending = 0;
    js->instruction_count = 0;

    /* Look up handler */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue routes = JS_GetPropertyStr(ctx, global, "__hull_routes");
    JSValue handler = JS_GetPropertyUint32(ctx, routes,
                                            (uint32_t)route->on_open_id);
    JS_FreeValue(ctx, routes);
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        js->dispatch_depth--;
        return;
    }

    /* Push conn object */
    JSValue conn_obj = hl_js_ws_push_conn(ctx, conn);

    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 1, &conn_obj);
    JS_FreeValue(ctx, handler);
    JS_FreeValue(ctx, conn_obj);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        log_error("[hull:ws] on_open error: %s", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    } else {
        JSPromiseStateEnum state = JS_PromiseState(ctx, ret);
        if (state == JS_PROMISE_PENDING && js->last_async_cont) {
            extern void hl_js_async_cont_set_handler_promise(
                HlAsyncCont *cont, JSContext *c, JSValue promise);
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
            JS_FreeValue(ctx, ret);
            /* dispatch_depth stays elevated for async */
            return;
        }
    }

    hl_js_run_jobs(js);
    JS_FreeValue(ctx, ret);
    js->dispatch_depth--;
}

static void hl_js_ws_on_message(KlWsServerConn *ws_conn, const char *data,
                                  size_t len, int is_binary, void *user_data)
{
    HlJSWsRoute *route = (HlJSWsRoute *)user_data;
    HlJS *js = route->js;
    JSContext *ctx = js->ctx;

    if (route->on_message_id < 0)
        return;

    /* Find the HlWsConn */
    HlWsConn *conn = hl_ws_registry_find(js->base.ws_registry,
                                           route->path, ws_conn);
    if (!conn)
        return;

    js->dispatch_depth++;
    js->active_conn = NULL;
    js->active_req = NULL;
    js->active_timer = NULL;
    js->last_async_cont = NULL;
    js->async_pending = 0;
    js->instruction_count = 0;

    /* Look up handler */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue routes_arr = JS_GetPropertyStr(ctx, global, "__hull_routes");
    JSValue handler = JS_GetPropertyUint32(ctx, routes_arr,
                                            (uint32_t)route->on_message_id);
    JS_FreeValue(ctx, routes_arr);
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        js->dispatch_depth--;
        return;
    }

    /* Build args: conn, message, is_binary */
    JSValue conn_obj = hl_js_ws_push_conn(ctx, conn);
    JSValue msg_val = JS_NewStringLen(ctx, data, len);
    JSValue is_bin_val = JS_NewBool(ctx, is_binary);

    JSValue args[3] = { conn_obj, msg_val, is_bin_val };
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 3, args);
    JS_FreeValue(ctx, handler);
    JS_FreeValue(ctx, conn_obj);
    JS_FreeValue(ctx, msg_val);
    JS_FreeValue(ctx, is_bin_val);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg2 = JS_ToCString(ctx, exc);
        log_error("[hull:ws] on_message error: %s", msg2 ? msg2 : "unknown");
        if (msg2) JS_FreeCString(ctx, msg2);
        JS_FreeValue(ctx, exc);
    } else {
        JSPromiseStateEnum state = JS_PromiseState(ctx, ret);
        if (state == JS_PROMISE_PENDING && js->last_async_cont) {
            extern void hl_js_async_cont_set_handler_promise(
                HlAsyncCont *cont, JSContext *c, JSValue promise);
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
            JS_FreeValue(ctx, ret);
            return;
        }
    }

    hl_js_run_jobs(js);
    JS_FreeValue(ctx, ret);
    js->dispatch_depth--;
}

static void hl_js_ws_on_close(KlWsServerConn *ws_conn, uint16_t code,
                                const char *reason, size_t reason_len,
                                void *user_data)
{
    HlJSWsRoute *route = (HlJSWsRoute *)user_data;
    HlJS *js = route->js;
    JSContext *ctx = js->ctx;

    /* Find the HlWsConn */
    HlWsConn *conn = hl_ws_registry_find(js->base.ws_registry,
                                           route->path, ws_conn);
    if (!conn)
        return;

    conn->closed = 1;

    if (route->on_close_id >= 0) {
        js->dispatch_depth++;
        js->active_conn = NULL;
        js->active_req = NULL;
        js->active_timer = NULL;
        js->last_async_cont = NULL;
        js->async_pending = 0;
        js->instruction_count = 0;

        /* Look up handler */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue routes_arr = JS_GetPropertyStr(ctx, global, "__hull_routes");
        JSValue handler = JS_GetPropertyUint32(ctx, routes_arr,
                                                (uint32_t)route->on_close_id);
        JS_FreeValue(ctx, routes_arr);
        JS_FreeValue(ctx, global);

        if (JS_IsFunction(ctx, handler)) {
            JSValue conn_obj = hl_js_ws_push_conn(ctx, conn);
            JSValue code_val = JS_NewInt32(ctx, code);
            JSValue reason_val = (reason && reason_len > 0)
                                     ? JS_NewStringLen(ctx, reason, reason_len)
                                     : JS_NULL;

            JSValue args[3] = { conn_obj, code_val, reason_val };
            JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 3, args);
            JS_FreeValue(ctx, conn_obj);
            JS_FreeValue(ctx, code_val);
            JS_FreeValue(ctx, reason_val);

            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(ctx);
                const char *msg = JS_ToCString(ctx, exc);
                log_error("[hull:ws] on_close error: %s", msg ? msg : "unknown");
                if (msg) JS_FreeCString(ctx, msg);
                JS_FreeValue(ctx, exc);
            }
            hl_js_run_jobs(js);
            JS_FreeValue(ctx, ret);
        }

        JS_FreeValue(ctx, handler);
        js->dispatch_depth--;
    }

    /* Invalidate conn object and remove from registry */
    hl_js_ws_invalidate_conn(ctx, conn);
    hl_ws_registry_remove(js->base.ws_registry, conn);
}

/* ── SSE dispatch ──────────────────────────────────────────────────── */

static void hl_js_sse_handler(KlRequest *req, KlResponse *res,
                                void *user_data)
{
    HlJSSseRoute *route = (HlJSSseRoute *)user_data;
    HlJS *js = route ? route->js : NULL;
    if (!js || !js->ctx || !req || !res)
        return;
    JSContext *ctx = js->ctx;

    js->dispatch_depth++;

    /* Guard stale transactions */
    hl_db_guard_stale_txn(js->base.db_handle);

    /* Reset scratch + instruction counter */
    sh_arena_reset(js->scratch);
    js->instruction_count = 0;

    /* Set per-request async context */
    js->active_conn = kl_request_conn(req);
    js->active_req = req;
    js->last_async_cont = NULL;
    js->async_pending = 0;

    /* Get handler function */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue routes_arr = JS_GetPropertyStr(ctx, global, "__hull_routes");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(routes_arr) || !JS_IsArray(ctx, routes_arr)) {
        JS_FreeValue(ctx, routes_arr);
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;
        return;
    }

    JSValue handler = JS_GetPropertyUint32(ctx, routes_arr,
                                            (uint32_t)route->handler_id);
    JS_FreeValue(ctx, routes_arr);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;
        return;
    }

    /* Build request object */
    JSValue js_req = hl_js_make_request(ctx, req);

    /* Create SSE stream object (calls kl_sse_begin) */
    JSValue stream_obj = hl_js_sse_create_stream(ctx, res);
    if (JS_IsException(stream_obj)) {
        JS_FreeValue(ctx, handler);
        JS_FreeValue(ctx, js_req);
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "SSE init failed", 15);
        return;
    }

    /* Call handler(req, stream) */
    JSValue args[2] = { js_req, stream_obj };
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, handler);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        log_error("[hull:sse] handler error: %s", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        hl_js_sse_stream_force_close(ctx, stream_obj);
    } else {
        JSPromiseStateEnum state = JS_PromiseState(ctx, ret);
        if (state == JS_PROMISE_PENDING && js->last_async_cont) {
            /* Async SSE handler — wire handler_promise on continuation */
            extern void hl_js_async_cont_set_handler_promise(
                HlAsyncCont *cont, JSContext *c, JSValue promise);
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, js_req);
            JS_FreeValue(ctx, stream_obj);
            /* dispatch_depth + active_conn stay set — async resume will clear */
            return;
        }
        /* Sync completion — close stream if not already */
        if (!hl_js_sse_stream_is_closed(ctx, stream_obj))
            hl_js_sse_stream_force_close(ctx, stream_obj);
    }

    hl_js_run_jobs(js);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, js_req);
    JS_FreeValue(ctx, stream_obj);
    js->active_conn = NULL;
    js->active_req = NULL;
    js->dispatch_depth--;
}

/* ── Request dispatch ───────────────────────────────────────────────── */

int hl_js_dispatch(HlJS *js, int handler_id,
                     KlRequest *req, KlResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;

    /* dispatch_depth may be > 0 during self-fetch (outbox.flush → same server).
     * This is safe because the original handler is yielded and the new
     * dispatch runs on its own coroutine/promise with independent state. */
    js->dispatch_depth++;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_guard_stale_txn(js->base.db_handle);

    hl_js_reset_request(js);

    /* Set per-request async context (for hull.sleep / http.get access) */
    js->active_conn = kl_request_conn(req);
    js->active_req = req;
    js->last_async_cont = NULL;

    /* Get the handler function from the route registry */
    JSValue global = JS_GetGlobalObject(js->ctx);
    JSValue routes = JS_GetPropertyStr(js->ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes) || !JS_IsArray(js->ctx, routes)) {
        JS_FreeValue(js->ctx, routes);
        JS_FreeValue(js->ctx, global);
        js->active_conn = NULL;
        js->active_req = NULL;
        return -1;
    }

    JSValue handler = JS_GetPropertyUint32(js->ctx, routes,
                                            (uint32_t)handler_id);
    JS_FreeValue(js->ctx, routes);

    if (!JS_IsFunction(js->ctx, handler)) {
        JS_FreeValue(js->ctx, handler);
        JS_FreeValue(js->ctx, global);
        js->active_conn = NULL;
        js->active_req = NULL;
        return -1;
    }

    /* Build JS request and response objects */
    JSValue js_req = hl_js_make_request(js->ctx, req);
    JSValue js_res = hl_js_make_response(js, res);

    /* Call handler(req, res) */
    JSValue argv[2] = { js_req, js_res };
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);

    int result = 0;
    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        result = -1;
    } else if (JS_PromiseState(js->ctx, ret) == JS_PROMISE_PENDING) {
        /* Async handler — connection already suspended by hull.sleep
         * or similar async call. Store the outer handler promise on
         * the continuation (per-connection, not global) so the resume
         * callback can check when the handler completes. */
        extern void hl_js_async_cont_set_handler_promise(
            HlAsyncCont *cont, JSContext *ctx, JSValue promise);
        if (js->last_async_cont) {
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont,
                js->ctx, ret);
            js->last_async_cont = NULL;
        }
        js->async_pending = 1;
        result = 1; /* signal: handler suspended */
    } else if (JS_PromiseState(js->ctx, ret) == JS_PROMISE_REJECTED) {
        /* Async handler threw before its first await — the Promise is
         * immediately rejected (not an exception).  Log and return -1
         * so the caller writes a 500 response. */
        JSValue err = JS_PromiseResult(js->ctx, ret);
        const char *msg = JS_ToCString(js->ctx, err);
        log_error("[hull:c] async handler rejected: %s",
                  msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(js->ctx, msg);
        JS_FreeValue(js->ctx, err);
        result = -1;
    }

    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, js_res);
    JS_FreeValue(js->ctx, js_req);
    JS_FreeValue(js->ctx, handler);
    JS_FreeValue(js->ctx, global);

    if (result != 1) {
        /* Sync path — clean up middleware ctx */
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;

        if (req->ctx) {
            HlReqCtx *rctx = (HlReqCtx *)req->ctx;
            if (rctx->kind == HL_REQCTX_JS_VAL) {
                JSValue val;
                memcpy(&val, rctx->js_val_bytes, sizeof(val));
                JS_FreeValue(js->ctx, val);
            } else if (rctx->kind == HL_REQCTX_JSON) {
                hl_alloc_free(js->base.alloc, rctx->json.data, rctx->json.len + 1);
            }
            hl_alloc_free(js->base.alloc, rctx, sizeof(HlReqCtx));
            req->ctx = NULL;
        }
    }
    /* result == 1: handler suspended, dispatch_depth stays elevated
     * until async resume completes */

    /* Run any pending microtasks */
    hl_js_run_jobs(js);

    return result;
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

/* ── Route wiring ──────────────────────────────────────────────────── */

void hl_js_keel_handler(KlRequest *req, KlResponse *res, void *user_data)
{
    HlJSRoute *route = (HlJSRoute *)user_data;
    int rc = hl_js_dispatch(route->js, route->handler_id, req, res);
    if (rc < 0) {
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "Internal Server Error", 21);
    }
    /* rc == 1: handler suspended — don't write response.
     * Keel checks conn->state == KL_CONN_SUSPENDED and returns. */
}

int hl_js_wire_routes(HlJS *js, KlRouter *router)
{
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");

    if (JS_IsUndefined(defs) || !JS_IsArray(ctx, defs)) {
        JS_FreeValue(ctx, defs);
        JS_FreeValue(ctx, global);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < count; i++) {
        JSValue def = JS_GetPropertyUint32(ctx, defs, (uint32_t)i);
        if (JS_IsUndefined(def))
            continue;

        JSValue method_val = JS_GetPropertyStr(ctx, def, "method");
        JSValue pattern_val = JS_GetPropertyStr(ctx, def, "pattern");
        JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");

        const char *method_str = JS_ToCString(ctx, method_val);
        const char *pattern = JS_ToCString(ctx, pattern_val);
        int32_t handler_id = 0;
        JS_ToInt32(ctx, &handler_id, id_val);

        if (method_str && pattern) {
            HlJSRoute *route = hl_alloc_malloc(js->base.alloc,
                                                 sizeof(HlJSRoute));
            if (route) {
                route->js = js;
                route->handler_id = handler_id;
                hl_js_track_route(js, route);
                kl_router_add(router, method_str, pattern,
                              hl_js_keel_handler, route, NULL);
            }
        }

        if (pattern) JS_FreeCString(ctx, pattern);
        if (method_str) JS_FreeCString(ctx, method_str);
        JS_FreeValue(ctx, id_val);
        JS_FreeValue(ctx, pattern_val);
        JS_FreeValue(ctx, method_val);
        JS_FreeValue(ctx, def);
    }

    JS_FreeValue(ctx, defs);

    /* Wire pre-body middleware from __hull_middleware */
    JSValue mw_arr = JS_GetPropertyStr(ctx, global, "__hull_middleware");
    if (JS_IsArray(ctx, mw_arr)) {
        JSValue mw_len = JS_GetPropertyStr(ctx, mw_arr, "length");
        int32_t mw_count = 0;
        JS_ToInt32(ctx, &mw_count, mw_len);
        JS_FreeValue(ctx, mw_len);

        for (int32_t i = 0; i < mw_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, mw_arr, (uint32_t)i);
            if (JS_IsUndefined(entry)) continue;

            JSValue m_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue p_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *m = JS_ToCString(ctx, m_val);
            const char *p = JS_ToCString(ctx, p_val);
            int32_t hid = 0;
            JS_ToInt32(ctx, &hid, id_val);

            if (m && p) {
                HlJSRoute *r = hl_alloc_malloc(js->base.alloc, sizeof(HlJSRoute));
                if (r) {
                    r->js = js;
                    r->handler_id = hid;
                    hl_js_track_route(js, r);
                    kl_router_use(router, m, p, hl_js_keel_middleware, r);
                }
            }

            if (p) JS_FreeCString(ctx, p);
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, p_val);
            JS_FreeValue(ctx, m_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, mw_arr);

    /* Wire post-body middleware from __hull_post_middleware */
    JSValue post_arr = JS_GetPropertyStr(ctx, global, "__hull_post_middleware");
    if (JS_IsArray(ctx, post_arr)) {
        JSValue post_len = JS_GetPropertyStr(ctx, post_arr, "length");
        int32_t post_count = 0;
        JS_ToInt32(ctx, &post_count, post_len);
        JS_FreeValue(ctx, post_len);

        for (int32_t i = 0; i < post_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, post_arr, (uint32_t)i);
            if (JS_IsUndefined(entry)) continue;

            JSValue m_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue p_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *m = JS_ToCString(ctx, m_val);
            const char *p = JS_ToCString(ctx, p_val);
            int32_t hid = 0;
            JS_ToInt32(ctx, &hid, id_val);

            if (m && p) {
                HlJSRoute *r = hl_alloc_malloc(js->base.alloc, sizeof(HlJSRoute));
                if (r) {
                    r->js = js;
                    r->handler_id = hid;
                    hl_js_track_route(js, r);
                    kl_router_use_post(router, m, p, hl_js_keel_middleware, r);
                }
            }

            if (p) JS_FreeCString(ctx, p);
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, p_val);
            JS_FreeValue(ctx, m_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, post_arr);

    JS_FreeValue(ctx, global);

    return 0;
}

/* ── Server route wiring (with body reader factory) ────────────────── */

int hl_js_wire_routes_server(HlJS *js, KlServer *server,
                              void *(*alloc_fn)(size_t))
{
    (void)alloc_fn; /* routes always use Hull allocator */
    js->server = server; /* store for async operations (hull.sleep, etc.) */
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");

    if (JS_IsUndefined(defs) || !JS_IsArray(ctx, defs)) {
        JS_FreeValue(ctx, defs);
        JS_FreeValue(ctx, global);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < count; i++) {
        JSValue def = JS_GetPropertyUint32(ctx, defs, (uint32_t)i);
        if (JS_IsUndefined(def))
            continue;

        JSValue method_val = JS_GetPropertyStr(ctx, def, "method");
        JSValue pattern_val = JS_GetPropertyStr(ctx, def, "pattern");
        JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");

        const char *method_str = JS_ToCString(ctx, method_val);
        const char *pattern = JS_ToCString(ctx, pattern_val);
        int32_t handler_id = 0;
        JS_ToInt32(ctx, &handler_id, id_val);

        if (method_str && pattern) {
            HlJSRoute *route = hl_alloc_malloc(js->base.alloc,
                                                 sizeof(HlJSRoute));
            if (route) {
                route->js = js;
                route->handler_id = handler_id;
                hl_js_track_route(js, route);
                kl_server_route(server, method_str, pattern,
                                hl_js_keel_handler, route,
                                hl_cap_body_factory);
            }
        }

        if (pattern) JS_FreeCString(ctx, pattern);
        if (method_str) JS_FreeCString(ctx, method_str);
        JS_FreeValue(ctx, id_val);
        JS_FreeValue(ctx, pattern_val);
        JS_FreeValue(ctx, method_val);
        JS_FreeValue(ctx, def);
    }

    JS_FreeValue(ctx, defs);

    /* Wire middleware from __hull_middleware */
    JSValue mw = JS_GetPropertyStr(ctx, global, "__hull_middleware");
    if (!JS_IsUndefined(mw) && JS_IsArray(ctx, mw)) {
        JSValue mw_len_val = JS_GetPropertyStr(ctx, mw, "length");
        int32_t mw_count = 0;
        JS_ToInt32(ctx, &mw_count, mw_len_val);
        JS_FreeValue(ctx, mw_len_val);

        for (int32_t i = 0; i < mw_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, mw, (uint32_t)i);
            if (JS_IsUndefined(entry))
                continue;

            JSValue method_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue pattern_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *method_str = JS_ToCString(ctx, method_val);
            const char *pattern = JS_ToCString(ctx, pattern_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (method_str && pattern) {
                HlJSRoute *mw_ctx = hl_alloc_malloc(js->base.alloc,
                                                      sizeof(HlJSRoute));
                if (mw_ctx) {
                    mw_ctx->js = js;
                    mw_ctx->handler_id = handler_id;
                    hl_js_track_route(js, mw_ctx);
                    kl_server_use(server, method_str, pattern,
                                  hl_js_keel_middleware, mw_ctx);
                }
            }

            if (pattern) JS_FreeCString(ctx, pattern);
            if (method_str) JS_FreeCString(ctx, method_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, pattern_val);
            JS_FreeValue(ctx, method_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, mw);

    /* Wire post-body middleware from __hull_post_middleware */
    JSValue post_mw = JS_GetPropertyStr(ctx, global, "__hull_post_middleware");
    if (!JS_IsUndefined(post_mw) && JS_IsArray(ctx, post_mw)) {
        JSValue post_mw_len_val = JS_GetPropertyStr(ctx, post_mw, "length");
        int32_t post_mw_count = 0;
        JS_ToInt32(ctx, &post_mw_count, post_mw_len_val);
        JS_FreeValue(ctx, post_mw_len_val);

        for (int32_t i = 0; i < post_mw_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, post_mw, (uint32_t)i);
            if (JS_IsUndefined(entry))
                continue;

            JSValue method_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue pattern_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *method_str = JS_ToCString(ctx, method_val);
            const char *pattern = JS_ToCString(ctx, pattern_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (method_str && pattern) {
                HlJSRoute *mw_ctx = hl_alloc_malloc(js->base.alloc,
                                                      sizeof(HlJSRoute));
                if (mw_ctx) {
                    mw_ctx->js = js;
                    mw_ctx->handler_id = handler_id;
                    hl_js_track_route(js, mw_ctx);
                    kl_server_use_post(server, method_str, pattern,
                                       hl_js_keel_middleware, mw_ctx);
                }
            }

            if (pattern) JS_FreeCString(ctx, pattern);
            if (method_str) JS_FreeCString(ctx, method_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, pattern_val);
            JS_FreeValue(ctx, method_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, post_mw);

    /* Wire timers from __hull_timer_defs */
    JSValue timer_defs = JS_GetPropertyStr(ctx, global, "__hull_timer_defs");
    if (!JS_IsUndefined(timer_defs) && JS_IsArray(ctx, timer_defs)) {
        JSValue td_len_val = JS_GetPropertyStr(ctx, timer_defs, "length");
        int32_t td_count = 0;
        JS_ToInt32(ctx, &td_count, td_len_val);
        JS_FreeValue(ctx, td_len_val);

        for (int32_t i = 0; i < td_count; i++) {
            JSValue def = JS_GetPropertyUint32(ctx, timer_defs, (uint32_t)i);
            if (JS_IsUndefined(def))
                continue;

            JSValue type_val = JS_GetPropertyStr(ctx, def, "type");
            JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");

            const char *type_str = JS_ToCString(ctx, type_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (!type_str) {
                JS_FreeValue(ctx, id_val);
                JS_FreeValue(ctx, type_val);
                JS_FreeValue(ctx, def);
                continue;
            }

            HlJSTimer *t = hl_alloc_malloc(js->base.alloc,
                                             sizeof(HlJSTimer));
            if (!t) {
                JS_FreeCString(ctx, type_str);
                JS_FreeValue(ctx, id_val);
                JS_FreeValue(ctx, type_val);
                JS_FreeValue(ctx, def);
                continue;
            }

            memset(t, 0, sizeof(*t));
            t->js = js;
            t->handler_id = handler_id;

            int64_t delay_ms;
            if (strcmp(type_str, "daily") == 0) {
                JSValue hour_val = JS_GetPropertyStr(ctx, def, "hour");
                JSValue min_val = JS_GetPropertyStr(ctx, def, "minute");
                JSValue lt_val = JS_GetPropertyStr(ctx, def, "localtime");
                int32_t th = 0, tm = 0;
                JS_ToInt32(ctx, &th, hour_val);
                JS_ToInt32(ctx, &tm, min_val);
                t->hour = th;
                t->minute = tm;
                t->localtime = JS_ToBool(ctx, lt_val);
                JS_FreeValue(ctx, hour_val);
                JS_FreeValue(ctx, min_val);
                JS_FreeValue(ctx, lt_val);
                t->daily = 1;
                delay_ms = hl_js_compute_daily_delay_ms(t->hour, t->minute,
                                                         t->localtime);
                t->interval_ms = 0;
            } else {
                JSValue iv_val = JS_GetPropertyStr(ctx, def, "interval_ms");
                int64_t iv = 0;
                JS_ToInt64(ctx, &iv, iv_val);
                JS_FreeValue(ctx, iv_val);
                t->interval_ms = iv;
                t->daily = 0;
                delay_ms = iv;
            }

            t->timer_id = kl_timer_add(&server->ev, (uint64_t)delay_ms,
                                        hl_js_timer_trampoline, t);
            if (t->timer_id < 0) {
                hl_alloc_free(js->base.alloc, t, sizeof(HlJSTimer));
            } else {
                hl_js_track_timer(js, t);
            }

            JS_FreeCString(ctx, type_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, type_val);
            JS_FreeValue(ctx, def);
        }
    }
    JS_FreeValue(ctx, timer_defs);

    /* ── Wire WebSocket endpoints from __hull_ws_defs ──────────────── */
    JSValue ws_defs = JS_GetPropertyStr(ctx, global, "__hull_ws_defs");
    if (!JS_IsUndefined(ws_defs) && JS_IsArray(ctx, ws_defs)) {
        /* Initialize registry if needed */
        if (!js->base.ws_registry) {
            js->base.ws_registry = hl_alloc_malloc(js->base.alloc,
                                                      sizeof(HlWsRegistry));
            if (js->base.ws_registry)
                hl_ws_registry_init(js->base.ws_registry, js->base.alloc);
        }

        JSValue ws_len_val = JS_GetPropertyStr(ctx, ws_defs, "length");
        int32_t ws_count = 0;
        JS_ToInt32(ctx, &ws_count, ws_len_val);
        JS_FreeValue(ctx, ws_len_val);

        for (int32_t i = 0; i < ws_count; i++) {
            JSValue wd = JS_GetPropertyUint32(ctx, ws_defs, (uint32_t)i);
            if (JS_IsUndefined(wd)) continue;

            JSValue path_val = JS_GetPropertyStr(ctx, wd, "path");
            JSValue oo_val = JS_GetPropertyStr(ctx, wd, "on_open_id");
            JSValue om_val = JS_GetPropertyStr(ctx, wd, "on_message_id");
            JSValue oc_val = JS_GetPropertyStr(ctx, wd, "on_close_id");

            const char *path = JS_ToCString(ctx, path_val);
            int32_t on_open_id = -1, on_message_id = -1, on_close_id = -1;
            if (!JS_IsUndefined(oo_val)) JS_ToInt32(ctx, &on_open_id, oo_val);
            if (!JS_IsUndefined(om_val)) JS_ToInt32(ctx, &on_message_id, om_val);
            if (!JS_IsUndefined(oc_val)) JS_ToInt32(ctx, &on_close_id, oc_val);

            if (path) {
                HlJSWsRoute *ws_route = hl_alloc_malloc(js->base.alloc,
                                                           sizeof(HlJSWsRoute));
                if (ws_route) {
                    ws_route->js = js;
                    ws_route->on_open_id = on_open_id;
                    ws_route->on_message_id = on_message_id;
                    ws_route->on_close_id = on_close_id;
                    int wn = snprintf(ws_route->path, sizeof(ws_route->path),
                                      "%s", path);
                    if (wn < 0 || (size_t)wn >= sizeof(ws_route->path)) {
                        log_warn("[hull:js] app.ws: path too long (max 255 chars): %s",
                                 path);
                        hl_alloc_free(js->base.alloc, ws_route,
                                      sizeof(HlJSWsRoute));
                        ws_route = NULL;
                    }
                }
                if (ws_route) {
                    if (hl_js_track_alloc(js, &js->ws_routes,
                            &js->ws_route_count,
                            &js->ws_route_cap, ws_route) != 0) {
                        hl_alloc_free(js->base.alloc, ws_route,
                                      sizeof(HlJSWsRoute));
                    } else {
                        KlWsServerConfig *ws_cfg =
                            hl_alloc_malloc(js->base.alloc,
                                            sizeof(KlWsServerConfig));
                        if (ws_cfg) {
                            kl_ws_server_config_init(ws_cfg);
                            ws_cfg->callbacks.on_open = hl_js_ws_on_open;
                            ws_cfg->callbacks.on_message = hl_js_ws_on_message;
                            ws_cfg->callbacks.on_close = hl_js_ws_on_close;
                            ws_cfg->user_data = ws_route;
                            hl_js_track_alloc(js, &js->ws_cfgs,
                                               &js->ws_cfg_count,
                                               &js->ws_cfg_cap, ws_cfg);
                            kl_server_ws(server, path, ws_cfg);
                        }
                    }
                }
                JS_FreeCString(ctx, path);
            }

            JS_FreeValue(ctx, oc_val);
            JS_FreeValue(ctx, om_val);
            JS_FreeValue(ctx, oo_val);
            JS_FreeValue(ctx, path_val);
            JS_FreeValue(ctx, wd);
        }
    }
    JS_FreeValue(ctx, ws_defs);

    /* ── Wire SSE endpoints from __hull_sse_defs ───────────────────── */
    JSValue sse_defs = JS_GetPropertyStr(ctx, global, "__hull_sse_defs");
    if (!JS_IsUndefined(sse_defs) && JS_IsArray(ctx, sse_defs)) {
        JSValue sse_len_val = JS_GetPropertyStr(ctx, sse_defs, "length");
        int32_t sse_count = 0;
        JS_ToInt32(ctx, &sse_count, sse_len_val);
        JS_FreeValue(ctx, sse_len_val);

        for (int32_t i = 0; i < sse_count; i++) {
            JSValue sd = JS_GetPropertyUint32(ctx, sse_defs, (uint32_t)i);
            if (JS_IsUndefined(sd)) continue;

            JSValue path_val = JS_GetPropertyStr(ctx, sd, "path");
            JSValue id_val = JS_GetPropertyStr(ctx, sd, "handler_id");

            const char *path = JS_ToCString(ctx, path_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (path) {
                HlJSSseRoute *sse_route = hl_alloc_malloc(js->base.alloc,
                                                             sizeof(HlJSSseRoute));
                if (sse_route) {
                    sse_route->js = js;
                    sse_route->handler_id = handler_id;
                    if (hl_js_track_alloc(js, &js->sse_routes,
                            &js->sse_route_count,
                            &js->sse_route_cap, sse_route) != 0) {
                        hl_alloc_free(js->base.alloc, sse_route,
                                      sizeof(HlJSSseRoute));
                    } else {
                        kl_server_route(server, "GET", path,
                                        hl_js_sse_handler, sse_route, NULL);
                    }
                }
                JS_FreeCString(ctx, path);
            }

            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, path_val);
            JS_FreeValue(ctx, sd);
        }
    }
    JS_FreeValue(ctx, sse_defs);

    JS_FreeValue(ctx, global);
    return 0;
}

/* ── Middleware dispatch ────────────────────────────────────────────── */

int hl_js_dispatch_middleware(HlJS *js, int handler_id,
                              KlRequest *req, KlResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_guard_stale_txn(js->base.db_handle);

    hl_js_reset_request(js);

    /* Get the handler function from the route registry */
    JSValue global = JS_GetGlobalObject(js->ctx);
    JSValue routes = JS_GetPropertyStr(js->ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes) || !JS_IsArray(js->ctx, routes)) {
        JS_FreeValue(js->ctx, routes);
        JS_FreeValue(js->ctx, global);
        return -1;
    }

    JSValue handler = JS_GetPropertyUint32(js->ctx, routes,
                                            (uint32_t)handler_id);
    JS_FreeValue(js->ctx, routes);

    if (!JS_IsFunction(js->ctx, handler)) {
        JS_FreeValue(js->ctx, handler);
        JS_FreeValue(js->ctx, global);
        return -1;
    }

    /* Build JS request and response objects */
    JSValue js_req = hl_js_make_request(js->ctx, req);
    JSValue js_res = hl_js_make_response(js, res);

    /* Call handler(req, res) — capture return value */
    JSValue argv[2] = { js_req, js_res };
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);

    int result = 0;
    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        result = -1;
    } else {
        /* Capture return value: 0 = continue, non-zero = short-circuit */
        int32_t val = 0;
        if (JS_ToInt32(js->ctx, &val, ret) == 0)
            result = val;
    }

    /* Store req.ctx as a JS value ref so the next middleware
     * or handler can retrieve the object directly (no JSON round-trip). */
    JSValue ctx_val = JS_GetPropertyStr(js->ctx, js_req, "ctx");
    if (JS_IsObject(ctx_val)) {
        /* Free previous ctx if any */
        if (req->ctx) {
            HlReqCtx *old = (HlReqCtx *)req->ctx;
            if (old->kind == HL_REQCTX_JS_VAL) {
                JSValue old_val;
                memcpy(&old_val, old->js_val_bytes, sizeof(old_val));
                JS_FreeValue(js->ctx, old_val);
            } else if (old->kind == HL_REQCTX_JSON) {
                hl_alloc_free(js->base.alloc, old->json.data, old->json.len + 1);
            }
            hl_alloc_free(js->base.alloc, old, sizeof(HlReqCtx));
            req->ctx = NULL;
        }
        /* Store native JS value */
        HlReqCtx *rctx = hl_alloc_malloc(js->base.alloc, sizeof(HlReqCtx));
        if (rctx) {
            rctx->kind = HL_REQCTX_JS_VAL;
            JSValue dup = JS_DupValue(js->ctx, ctx_val);
            memcpy(rctx->js_val_bytes, &dup, sizeof(dup));
            req->ctx = rctx;
        }
    }
    JS_FreeValue(js->ctx, ctx_val);

    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, js_res);
    JS_FreeValue(js->ctx, js_req);
    JS_FreeValue(js->ctx, handler);
    JS_FreeValue(js->ctx, global);

    /* Run any pending microtasks */
    hl_js_run_jobs(js);

    return result;
}

int hl_js_keel_middleware(KlRequest *req, KlResponse *res, void *user_data)
{
    HlJSRoute *ctx = (HlJSRoute *)user_data;
    int rc = hl_js_dispatch_middleware(ctx->js, ctx->handler_id, req, res);
    if (rc < 0) {
        /* Middleware error — short-circuit with 500 */
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "Internal Server Error", 21);
        return 1; /* short-circuit */
    }
    return rc;
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

static int vt_js_wire_routes_server(HlRuntime *rt, KlServer *server,
                                     void *(*alloc_fn)(size_t))
{
    return hl_js_wire_routes_server((HlJS *)rt, server, alloc_fn);
}

static int vt_js_extract_manifest(HlRuntime *rt, HlManifest *out)
{
    HlJS *js = (HlJS *)rt;
    return hl_manifest_extract_js(js->ctx, out, js->base.alloc);
}

static void vt_js_destroy(HlRuntime *rt)
{
    hl_js_free((HlJS *)rt);
}

const HlRuntimeVtable hl_js_vtable = {
    .init                = vt_js_init,
    .load_app            = vt_js_load_app,
    .wire_routes_server  = vt_js_wire_routes_server,
    .extract_manifest    = vt_js_extract_manifest,
    .destroy             = vt_js_destroy,
    .name                = "QuickJS",
};
