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
 * The two per-RUNTIME references the spike found (luaopen_hull_compute from
 * modules.o, lua_push_wasm_buffer from mod_gpu.o, + the JS init twin) do NOT
 * live here: they are per-runtime and live in the pure runtime archives
 * (src/hull/runtime/{lua,js}/wasm_stub.c), so lua_push_wasm_buffer can push a
 * balanced nil and the Lua stub stays out of a JS-only app's link. Neither weak
 * body is ever REACHED on a compute-free app — modules.c gates the
 * compute-module register on wasm_cache (NULL without the feature) — they only
 * satisfy the link.
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
    /* Positive sentinel: the feature is NOT COMPOSED (compute-free app), an
     * expected/quiet state — distinct from a genuine WAMR init failure (<0), so
     * serve.c doesn't log a misleading "WAMR init failed" on every such app. */
    return HL_CAP_WASM_ABSENT;
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

/* The two per-RUNTIME compute-binding refs (luaopen_hull_compute /
 * lua_push_wasm_buffer + the JS init) live in the PURE RUNTIME archives, not
 * here: src/hull/runtime/{lua,js}/wasm_stub.c. Placing them where the VM is
 * linked lets lua_push_wasm_buffer push a balanced nil, and keeps the Lua stub
 * out of a JS-only app's link (this base TU is shared by both). */
