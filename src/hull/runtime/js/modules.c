/*
 * modules.c — hull:* module registry dispatcher for QuickJS
 *
 * All module implementations live in mod_*.c files. This file
 * contains only the hl_js_register_modules() entry point that
 * registers each module based on runtime availability.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

/* ════════════════════════════════════════════════════════════════════
 * Module registry — called by hl_js_init() to register all
 * hull:* built-in modules.
 * ════════════════════════════════════════════════════════════════════ */

int hl_js_register_modules(HlJS *js)
{
    if (!js || !js->ctx)
        return -1;

    /* Register hull:app module */
    if (hl_js_init_app_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:db module (only if database is available) */
#ifdef HL_ENABLE_DB
    if (js->base.db_handle) {
        if (hl_js_init_db_module(js->ctx, js) != 0)
            return -1;
    }
#endif

    /* Register hull:json module */
    if (hl_js_init_json_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:time module */
    if (hl_js_init_time_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:env module */
    if (hl_js_init_env_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:crypto module */
    if (hl_js_init_crypto_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:log module */
    if (hl_js_init_log_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:http module — always available; per-function checks
     * enforce that http_cfg is set (wired from manifest after load_app). */
    if (hl_js_init_http_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:smtp module — always available; per-function checks
     * enforce that smtp_cfg is set (wired from manifest after load_app). */
    if (hl_js_init_smtp_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:_template — internal bridge for hull:template stdlib */
    if (hl_js_init_template_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:worker module (only if thread pool is available) */
    if (js->base.thread_pool) {
        if (hl_js_init_worker_module(js->ctx, js) != 0)
            return -1;
    }

    /* Register hull:server module (always available) */
    if (hl_js_init_server_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:fs module — always available; per-function checks
     * enforce that fs_cfg is set (wired from manifest after load_app). */
    if (hl_js_init_fs_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:image module — always available */
    if (hl_js_init_image_module(js->ctx, js) != 0)
        return -1;

#ifdef HL_ENABLE_WASM
    /* Register hull:compute module (only if WASM runtime is available) */
    if (js->base.wasm_cache) {
        if (hl_js_init_compute_module(js->ctx, js) != 0)
            return -1;
    }
#endif

#ifdef HL_ENABLE_GPU
    /* Register hull:gpu module (only if GPU context is available) */
    if (js->base.gpu_ctx) {
        if (hl_js_init_gpu_module(js->ctx, js) != 0)
            return -1;
    }
#endif

    /* Register hull:ws module + SSE class (always available) */
    hl_js_sse_register_class(js->ctx);
    if (hl_js_init_ws_module(js->ctx, js) != 0)
        return -1;

    return 0;
}
