/*
 * wasm_weakstub.c — weak no-op defaults for the WASM cap seam (WASM as a
 * composable feature, docs/wasm_feature.md).
 *
 * The plan moves WAMR + the wasm caps (cap/wasm*.c, worker_wasm.c, WAMR_OBJS)
 * out of the base into an embedded, auto-composed libhull_feature-wasm.a. Base
 * objects that STAY reference a small set of cap symbols directly:
 *
 *   app_context.o, serve.o     -> hl_cap_wasm_init / _destroy   (cache lifecycle)
 *   cap_db_udf.o               -> hl_cap_wasm_load / _instance_* (WASM-backed UDF)
 *   mod_buffer (per runtime)   -> hl_wasm_buffer_data / _len     (buffer protocol)
 *   mod_image  (per runtime)   -> hl_wasm_buffer_borrow / _release
 *   mod_gpu    (per runtime)   -> hl_wasm_buffer_create_adopted  (GPU -> WasmBuffer)
 *
 * The Phase-0 spike (an `nm` map over these objects) enumerated exactly these
 * eleven runtime-AGNOSTIC symbols as the core seam; this TU provides weak,
 * fail-closed defaults for them. When libhull_feature-wasm.a is composed it is
 * whole-archived, so the linker takes its strong definitions; when it is NOT
 * composed (a genuinely compute-free app, once the base is flipped in Phase 1)
 * these weak no-ops satisfy the link, `compute.available()` reads false, a
 * WASM-backed `db.udf` fails closed (function UDFs are untouched — they never
 * call these), and no `WasmBuffer` can exist (the other buffer-protocol types
 * are unaffected).
 *
 * This mirrors http_weakstub.c: real prototypes (the base already sees the cap
 * headers) so the definitions are ODR/LTO-clean against the strong ones. It is
 * additive and dormant while WAMR is still compiled into the base (Phase 0):
 * the strong cap definitions win, so behavior is byte-identical.
 *
 * The two per-RUNTIME references the spike also found (luaopen_hull_compute from
 * modules.o, lua_push_wasm_buffer from mod_gpu.o, + the JS twins) are deferred
 * to Phase 1: they only need stubbing once mod_compute actually moves into the
 * per-runtime bridge, and their degraded semantics (require("hull.compute") ->
 * nil) are part of the resolver integration there, not this cap seam.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stddef.h>

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"

/* ── Module cache lifecycle (app_context.o, serve.o) ─────────────────── */

__attribute__((weak)) int hl_cap_wasm_init(HlWasmCache *cache)
{
    (void)cache;
    return -1;   /* no runtime -> init "fails"; available() stays false */
}

__attribute__((weak)) void hl_cap_wasm_destroy(HlWasmCache *cache)
{
    (void)cache;
}

/* ── WASM-backed db.udf (cap_db_udf.o) ───────────────────────────────── */

__attribute__((weak)) int hl_cap_wasm_load(HlWasmCache *cache, const char *name,
                                           const struct HlVfs *app_vfs,
                                           const char *app_dir)
{
    (void)cache; (void)name; (void)app_vfs; (void)app_dir;
    return -1;
}

__attribute__((weak))
HlWasmInstance *hl_cap_wasm_instance_create(HlWasmCache *cache, const char *name,
                                            const HlWasmCallOpts *opts,
                                            const struct HlVfs *app_vfs,
                                            const char *app_dir,
                                            HlAllocator *alloc,
                                            const char **err_msg)
{
    (void)cache; (void)name; (void)opts; (void)app_vfs; (void)app_dir;
    (void)alloc;
    if (err_msg) *err_msg = "WASM feature not composed";
    return NULL;
}

__attribute__((weak))
int hl_cap_wasm_instance_call(HlWasmInstance *inst,
                              const void *input, size_t input_len,
                              void **output, size_t *output_len,
                              const HlWasmCallOpts *opts,
                              HlWasmCallbackFn cb_fn, void *cb_ctx,
                              HlAllocator *alloc, const char **err_msg)
{
    (void)inst; (void)input; (void)input_len; (void)output; (void)output_len;
    (void)opts; (void)cb_fn; (void)cb_ctx; (void)alloc;
    if (err_msg) *err_msg = "WASM feature not composed";
    return -1;
}

__attribute__((weak)) void hl_cap_wasm_instance_destroy(HlWasmInstance *inst)
{
    (void)inst;
}

/* ── Unified buffer protocol: the WasmBuffer type (mod_buffer / image / gpu) ── */

__attribute__((weak)) const void *hl_wasm_buffer_data(const HlWasmBuffer *buf)
{
    (void)buf;
    return NULL;
}

__attribute__((weak)) size_t hl_wasm_buffer_len(const HlWasmBuffer *buf)
{
    (void)buf;
    return 0;
}

__attribute__((weak)) void hl_wasm_buffer_borrow(HlWasmBuffer *buf)
{
    (void)buf;
}

__attribute__((weak)) void hl_wasm_buffer_release(void *buf)
{
    (void)buf;
}

__attribute__((weak))
HlWasmBuffer *hl_wasm_buffer_create_adopted(void *data, size_t len,
                                            HlAllocator *alloc)
{
    (void)data; (void)len; (void)alloc;
    return NULL;
}
