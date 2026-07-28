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
#include "internal.h"  /* hl_js_request_register, hl_js_sse_register_class */
#include "hull/cap/tui.h"  /* hl_tui_feature_register_js (composed-feature seam) */
#include "hull/http_feature.h"  /* hl_js_register_http_modules (HTTP-feature seam, #114) */

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
    if (js->base.db_registry) {
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

    /* HTTP-dependent modules (http-client/server, smtp, ws-server/client, sse,
     * multipart) register through the HTTP-feature seam (hl_js_register_http_modules,
     * below) so this core registry no longer references them. See
     * hull/http_feature.h (#114). */

    /* Register hull:_template — internal bridge for hull:template stdlib */
    if (hl_js_init_template_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:worker module (only if thread pool is available) */
    if (js->base.thread_pool) {
        if (hl_js_init_worker_module(js->ctx, js) != 0)
            return -1;
    }

    /* Register hull:fs module — always available; per-function checks
     * enforce that fs_cfg is set (wired from manifest after load_app). */
    if (hl_js_init_fs_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:blob module — always available; per-function checks
     * gate on the singleton created by blob.init(). */
    if (hl_js_init_blob_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:mime module — pure utility, always available */
    if (hl_js_init_mime_module(js->ctx, js) != 0)
        return -1;

#ifdef HL_ENABLE_IMAGE
    /* Register hull:image module (dropped on a HL_ENABLE_IMAGE=0 build). */
    if (hl_js_init_image_module(js->ctx, js) != 0)
        return -1;
#endif

#ifdef HL_ENABLE_WASM
    /* Register hull:compute module (only if WASM runtime is available) */
    if (js->base.wasm_cache) {
        if (hl_js_init_compute_module(js->ctx, js) != 0)
            return -1;
    }
#endif

    /* hull:gpu registers only when a GPU backend is present (monolithic
     * HL_ENABLE_GPU build or a composed --with=gpu feature). */
    if (js->base.gpu_ctx) {
        if (hl_js_init_gpu_module(js->ctx, js) != 0)
            return -1;
    }

    /* TUI native module — registered by the tui feature (monolithic
     * HL_ENABLE_TUI or a composed --with=tui). Base ships a weak no-op; the
     * feature's strong override calls hl_js_init_tui_module. Gated at use time
     * by the resolver (requires the tui cap + manifest.tui). Unconditional so a
     * composed binary picks it up without HL_ENABLE_TUI. */
    if (hl_tui_feature_register_js(js->ctx, js) != 0)
        return -1;

    /* HTTP-feature seam (#114): registers http-client/server, smtp,
     * ws-server/client, the SSE class + the streaming-multipart request
     * classes. Weak no-op in a base with no HTTP composed; the strong override
     * (runtime/js/http_register.c) wins on a full base / composed http feature.
     * Consolidated here from the former inline #ifdef HL_ENABLE_HTTP_* blocks -
     * module registration is order-independent, so behavior is unchanged. */
    if (hl_js_register_http_modules(js->ctx, js) != 0)
        return -1;

    return 0;
}
