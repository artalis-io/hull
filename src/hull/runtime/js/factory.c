/*
 * runtime/js/factory.c — JavaScript runtime factory
 *
 * Heap-allocates an HlJS, builds HlJSConfig from HlAppContextOpts,
 * initializes via hl_js_init, and returns the embedded HlRuntime *.
 * Counterpart in runtime/lua/factory.c.
 *
 * Architectural roadmap item K.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_JS

#include "hull/runtime/factory.h"
#include "hull/runtime/js.h"
#include "hull/app_context.h"

#include <stdlib.h>
#include <string.h>

static int js_create(HlRuntime **out, const HlAppContextOpts *opts,
                     const HlRuntimeBaseConfig *base)
{
    if (!out || !opts) return -1;

    HlJS *js = (HlJS *)calloc(1, sizeof(HlJS));
    if (!js) return -1;

    /* Copy base config into rt->base BEFORE init (parallel to Lua side). */
    if (base) {
        js->base.db_registry    = base->db_registry;
        js->base.alloc          = base->alloc;
        js->base.app_vfs        = base->app_vfs;
        js->base.platform_vfs   = base->platform_vfs;
        js->base.thread_pool    = base->thread_pool;
        js->base.db_path        = base->db_path;
        js->base.compress       = base->compress;
#ifdef HL_ENABLE_WASM
        js->base.wasm_cache     = base->wasm_cache;
#endif
#ifdef HL_ENABLE_GPU
        js->base.gpu_ctx        = base->gpu_ctx;
#endif
    }
    if (!js->base.alloc && opts->alloc) js->base.alloc = opts->alloc;

    /* HlJSConfig has no sandbox toggle — QuickJS is always sandboxed
     * (eval removed, std/os not loaded). */
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    if (opts->heap_limit > 0)
        cfg.max_heap_bytes = (size_t)opts->heap_limit;
    if (opts->stack_limit > 0)
        cfg.max_stack_bytes = (size_t)opts->stack_limit;
    if (opts->instruction_limit > 0)
        cfg.max_instructions = opts->instruction_limit;

    if (hl_js_init(js, &cfg) != 0) {
        free(js);
        return -1;
    }

    js->base.vt = &hl_js_vtable;
    *out = &js->base;
    return 0;
}

static void js_destroy(HlRuntime *rt)
{
    if (!rt) return;
    HlJS *js = (HlJS *)rt;
    hl_js_free(js);
    free(js);
}

const HlRuntimeFactory hl_js_factory = {
    .name            = "js",
    .entry_extension = ".js",
    .create          = js_create,
    .destroy         = js_destroy,
    .vtable          = &hl_js_vtable,
};

#endif /* HL_ENABLE_JS */
