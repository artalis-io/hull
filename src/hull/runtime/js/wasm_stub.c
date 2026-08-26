/*
 * wasm_stub.c - weak per-runtime compute-binding default (JS). See the Lua twin
 * (src/hull/runtime/lua/wasm_stub.c) for the rationale.
 *
 * hl_js_init_compute_module is referenced by js_modules.c's module register; the
 * strong def lives in the compute bridge (libhull_feature-wasm-js.a, mod_compute).
 * This weak no-op satisfies the link for a compute-free JS app (never reached -
 * modules.c gates the register on wasm_cache). It lives in the pure JS runtime
 * archive so it's linked only into JS apps.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"   /* hl_js_init_compute_module decl, JSContext, HlJS */

__attribute__((weak)) int hl_js_init_compute_module(JSContext *ctx, HlJS *js)
{
    (void)ctx; (void)js;
    return -1;
}
