/*
 * mod_buffer.h — Shared declarations for per-module JS binding files
 *
 * Provides common includes, inline helpers, the unified buffer protocol
 * declaration, and all hl_js_init_*_module function prototypes so the
 * dispatcher (modules.c) and individual mod_*.c files can see each other.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_JS_MOD_BUFFER_H
#define HULL_JS_MOD_BUFFER_H

#include "hull/runtime/js.h"
#include "hull/buffer.h"
#include "hull/cap/fs.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"
#include "quickjs.h"

#include <stdlib.h>
#include <string.h>

/* Module-declaration gate for native JS modules. Declared here so every
 * mod_*.c sees it via mod_buffer.h. Implementation in runtime.c. */
int hl_js_check_module_declared(JSContext *ctx,
                                 const char *canonical_name,
                                 const char *runtime_name);

/* ── Shared JS class IDs ─────────────────────────────────────────── */

/* MappedBuffer class (fs.mmap) — used by fs, compute, gpu, buffer */
extern JSClassID js_mmap_class_id;

/* HlImage class — used by image, buffer */
extern JSClassID js_image_class_id;

#ifdef HL_ENABLE_WASM
/* WasmBuffer class — used by compute, gpu, buffer */
extern JSClassID js_wasm_buf_class_id;
#endif

/* ── Inline helpers ──────────────────────────────────────────────── */

/* Retrieve HlJS context from JS opaque pointer */
static inline HlJS *get_hl_js(JSContext *ctx)
{
    return (HlJS *)JS_GetContextOpaque(ctx);
}

/* Compiler-safe memory zeroing that won't be optimized away */
static inline void secure_zero(void *p, size_t n)
{
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

/* ── Unified buffer protocol ─────────────────────────────────────── */

/*
 * Extract a buffer view from a JS value. Tries (in order):
 * MappedBuffer -> WasmBuffer -> ArrayBuffer -> string.
 * Sets *str_needs_free = 1 if the data came from JS_ToCStringLen
 * (caller must JS_FreeCString). Returns 1 on success, 0 on failure.
 */
int js_get_buffer(JSContext *ctx, JSValueConst val,
                  HlBufferView *out, const char **str_out, int *needs_free);

/* ── Module init functions ───────────────────────────────────────── */

int hl_js_init_app_module(JSContext *ctx, HlJS *js);
int hl_js_init_db_module(JSContext *ctx, HlJS *js);
int hl_js_init_json_module(JSContext *ctx, HlJS *js);
int hl_js_init_time_module(JSContext *ctx, HlJS *js);
int hl_js_init_env_module(JSContext *ctx, HlJS *js);
int hl_js_init_crypto_module(JSContext *ctx, HlJS *js);
int hl_js_init_log_module(JSContext *ctx, HlJS *js);
int hl_js_init_http_module(JSContext *ctx, HlJS *js);
int hl_js_init_smtp_module(JSContext *ctx, HlJS *js);
int hl_js_init_template_module(JSContext *ctx, HlJS *js);
int hl_js_init_worker_module(JSContext *ctx, HlJS *js);
int hl_js_init_server_module(JSContext *ctx, HlJS *js);
int hl_js_init_fs_module(JSContext *ctx, HlJS *js);
int hl_js_init_blob_module(JSContext *ctx, HlJS *js);
int hl_js_init_mime_module(JSContext *ctx, HlJS *js);
int hl_js_init_image_module(JSContext *ctx, HlJS *js);

#ifdef HL_ENABLE_WASM
int hl_js_init_compute_module(JSContext *ctx, HlJS *js);
#endif

#ifdef HL_ENABLE_GPU
int hl_js_init_gpu_module(JSContext *ctx, HlJS *js);
#endif

#ifdef HL_ENABLE_TUI
int hl_js_init_tui_module(JSContext *ctx, HlJS *js);
#endif

/* ── WebSocket modules (split: server in mod_ws_server.c,
 *    client in mod_ws_client.c) ─────────────────────────────────── */

int hl_js_init_ws_server_module(JSContext *ctx, HlJS *js);
int hl_js_init_ws_client_module(JSContext *ctx, HlJS *js);

/* Server-side conn class ID (in mod_ws_server.c) — referenced by
 * runtime/js/ws.c (Keel callback dispatcher). The client conn class
 * is internal to mod_ws_client.c. */
extern JSClassID js_ws_conn_class_id;

/* WS conn helpers (defined in js/mod_ws.c) */
struct HlWsConn;
JSValue hl_js_ws_push_conn(JSContext *ctx, struct HlWsConn *conn);
void    hl_js_ws_invalidate_conn(JSContext *ctx, struct HlWsConn *conn);

/* ── SSE module (defined in js/mod_sse.c) ────────────────────────── */

extern JSClassID js_sse_stream_class_id;

void    hl_js_sse_register_class(JSContext *ctx);
JSValue hl_js_sse_create_stream(JSContext *ctx, KlResponse *res);
int     hl_js_sse_stream_is_closed(JSContext *ctx, JSValueConst val);
void    hl_js_sse_stream_force_close(JSContext *ctx, JSValueConst val);

#endif /* HULL_JS_MOD_BUFFER_H */
