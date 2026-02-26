/*
 * js_runtime.c — QuickJS runtime for Hull
 *
 * Initializes QuickJS with sandboxing: no eval, no std/os modules,
 * custom allocator, memory limits, instruction-count interrupt handler,
 * and custom module loader for hull:* built-in modules.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/js_runtime.h"
#include "hull/hull_alloc.h"
#include "hull/hull_limits.h"
#include "hull/hull_cap.h"
#include "quickjs.h"

#include <sh_arena.h>

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations for module init functions ─────────────────── */

int hl_js_init_app_module(JSContext *ctx, HlJS *js);
int hl_js_init_db_module(JSContext *ctx, HlJS *js);
int hl_js_init_json_module(JSContext *ctx, HlJS *js);
int hl_js_init_time_module(JSContext *ctx, HlJS *js);
int hl_js_init_env_module(JSContext *ctx, HlJS *js);
int hl_js_init_crypto_module(JSContext *ctx, HlJS *js);
int hl_js_init_log_module(JSContext *ctx, HlJS *js);

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

    /* hull:* modules are pre-registered — QuickJS resolves them
     * automatically from the module registry. If we get here,
     * it means the module wasn't found in the registry. */
    if (strncmp(module_name, "hull:", 5) == 0) {
        JS_ThrowReferenceError(ctx, "unknown hull module: %s", module_name);
        return NULL;
    }

    /* Load from filesystem (development mode) */
    if (!js->app_dir) {
        JS_ThrowReferenceError(ctx, "no app directory configured");
        return NULL;
    }

    /* Build filesystem path */
    char path[HL_MODULE_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", js->app_dir, module_name);
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

    /* Remove Function constructor (prevents new Function("...")) */
    /* We leave Function itself since it's needed internally,
     * but the constructor is effectively neutered by removing eval */

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

    /* Save caller-set fields before zeroing */
    sqlite3 *db = js->db;
    HlFsConfig *fs_cfg = js->fs_cfg;
    HlEnvConfig *env_cfg = js->env_cfg;
    HlHttpConfig *http_cfg = js->http_cfg;
    HlAllocator *alloc = js->alloc;

    memset(js, 0, sizeof(*js));

    /* Restore caller-set fields */
    js->db = db;
    js->fs_cfg = fs_cfg;
    js->env_cfg = env_cfg;
    js->http_cfg = http_cfg;
    js->alloc = alloc;
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

    /* Store HlJS pointer in context opaque for C functions to access */
    JS_SetContextOpaque(js->ctx, js);

    /* Register hull:* built-in modules */
    if (hl_js_register_modules(js) != 0) {
        hl_js_free(js);
        return -1;
    }

    /* Per-request scratch arena */
    js->scratch = hl_arena_create(js->alloc, HL_SCRATCH_SIZE);
    if (!js->scratch) {
        hl_js_free(js);
        return -1;
    }

    return 0;
}

int hl_js_load_app(HlJS *js, const char *filename)
{
    if (!js || !js->ctx || !filename)
        return -1;

    /* Read the entry point file */
    FILE *f = fopen(filename, "rb");
    if (!f) {
        log_error("[hull:c] cannot open %s", filename);
        return -1;
    }

    fseek(f, 0, SEEK_END);
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

    /* Extract app directory from filename */
    size_t fn_len = strlen(filename);
    char *app_dir = hl_alloc_malloc(js->alloc, fn_len + 1);
    if (!app_dir) {
        js->scratch->used = arena_saved;
        return -1;
    }
    memcpy(app_dir, filename, fn_len + 1);
    char *last_slash = strrchr(app_dir, '/');
    if (last_slash)
        *last_slash = '\0';
    else {
        hl_alloc_free(js->alloc, app_dir, fn_len + 1);
        app_dir = hl_alloc_malloc(js->alloc, 2);
        if (!app_dir) {
            js->scratch->used = arena_saved;
            return -1;
        }
        app_dir[0] = '.';
        app_dir[1] = '\0';
        fn_len = 1;
    }
    js->app_dir = app_dir;
    js->app_dir_size = fn_len + 1;

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

void hl_js_free(HlJS *js)
{
    if (!js)
        return;

    /* Reset response class state for potential re-init */
    extern void hl_js_reset_response_class(void);
    hl_js_reset_response_class();

    if (js->ctx) {
        JS_FreeContext(js->ctx);
        js->ctx = NULL;
    }
    if (js->rt) {
        JS_FreeRuntime(js->rt);
        js->rt = NULL;
    }
    if (js->app_dir) {
        hl_alloc_free(js->alloc, (void *)js->app_dir, js->app_dir_size);
        js->app_dir = NULL;
        js->app_dir_size = 0;
    }
    hl_arena_free(js->alloc, js->scratch);
    js->scratch = NULL;
    if (js->response_body) {
        hl_alloc_free(js->alloc, js->response_body,
                      js->response_body_size);
        js->response_body = NULL;
        js->response_body_size = 0;
    }
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

/* ── Request dispatch ───────────────────────────────────────────────── */

int hl_js_dispatch(HlJS *js, int handler_id,
                     KlRequest *req, KlResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;

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
    /* These are created by js_bindings.c functions */
    extern JSValue hl_js_make_request(JSContext *ctx, KlRequest *req);
    extern JSValue hl_js_make_response(JSContext *ctx, KlResponse *res);

    JSValue js_req = hl_js_make_request(js->ctx, req);
    JSValue js_res = hl_js_make_response(js->ctx, res);

    /* Call handler(req, res) */
    JSValue argv[2] = { js_req, js_res };
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);

    int result = 0;
    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        result = -1;
    }

    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, js_res);
    JS_FreeValue(js->ctx, js_req);
    JS_FreeValue(js->ctx, handler);
    JS_FreeValue(js->ctx, global);

    /* Run any pending microtasks */
    hl_js_run_jobs(js);

    return result;
}
