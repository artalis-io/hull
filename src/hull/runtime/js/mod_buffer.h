/*
 * mod_buffer.h - Shared declarations for per-module JS binding files
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
#include "hull/utils/buffer.h"
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

/* MappedBuffer class (fs.mmap) - used by fs, compute, gpu, buffer */
extern JSClassID js_mmap_class_id;

/* HlImage class - used by image, buffer */
extern JSClassID js_image_class_id;

#ifdef HL_ENABLE_WASM
/* WasmBuffer class - used by compute, gpu, buffer */
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

/* An ArrayBuffer's bytes, or NULL with NO exception pending. For a probe
 * that falls back to another type: JS_GetArrayBuffer throws on anything but
 * an ArrayBuffer, and that TypeError stayed pending through a successful
 * fallback - a later JS_EXCEPTION return then surfaced the wrong error. */
static inline uint8_t *hl_js_array_buffer_probe(JSContext *ctx, size_t *len,
                                                JSValueConst v)
{
    uint8_t *p = JS_GetArrayBuffer(ctx, len, v);
    if (!p) JS_FreeValue(ctx, JS_GetException(ctx));
    return p;
}

/* A db param that is not a string, number, boolean, null or undefined. An
 * ArrayBuffer or a typed array is a BLOB: @p *view names its bytes, which
 * the caller copies at once (a later element's getter could detach or
 * resize the buffer) into out->s. A BigInt in the int64 range is an
 * integer. Anything else is a TypeError: they were bound as NULL, silently,
 * so a blob round-trip stored NULL (audit 23). 0, or -1 with an exception
 * pending. */
static inline int hl_js_db_param_other(JSContext *ctx, JSValueConst v,
                                       HlValue *out, const uint8_t **view)
{
    *view = NULL;
    if (JS_IsBigInt(ctx, v)) {
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, v) < 0) return -1;
        JSValue back = JS_NewBigInt64(ctx, i);
        if (JS_IsException(back)) return -1;
        int same = JS_StrictEq(ctx, back, v);
        JS_FreeValue(ctx, back);
        if (!same) {
            JS_ThrowRangeError(ctx, "db params: a BigInt outside the 64-bit "
                               "integer range");
            return -1;
        }
        out->type = HL_TYPE_INT;
        out->i = i;
        return 0;
    }
    if (JS_IsObject(v)) {
        size_t n = 0;
        uint8_t *p = hl_js_array_buffer_probe(ctx, &n, v);
        if (!p) {
            size_t off = 0, blen = 0, per = 0;
            JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &per);
            if (JS_IsException(ab)) {
                JS_FreeValue(ctx, JS_GetException(ctx));   /* not a view */
            } else {
                size_t ab_len = 0;
                uint8_t *raw = hl_js_array_buffer_probe(ctx, &ab_len, ab);
                JS_FreeValue(ctx, ab);
                if (raw && off <= ab_len && blen <= ab_len - off) {
                    p = raw + off;
                    n = blen;
                }
            }
        }
        if (p) {
            out->type = HL_TYPE_BLOB;
            out->len = n;
            *view = p;
            return 0;
        }
    }
    JS_ThrowTypeError(ctx, "db params: a value must be a string, number, "
                      "boolean, BigInt, null, ArrayBuffer or typed array");
    return -1;
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

/* The allocator an HlImage's pixels come from: this runtime's js_malloc_rt,
 * so they count against JS_SetMemoryLimit (and the GC threshold). */
struct HlImageAlloc;
void hl_js_image_alloc(JSContext *ctx, struct HlImageAlloc *out);

/* ── Module init functions ───────────────────────────────────────── */

int hl_js_init_app_module(JSContext *ctx, HlJS *js);
int hl_js_init_db_module(JSContext *ctx, HlJS *js);
int hl_js_init_kv_module(JSContext *ctx, HlJS *js);   /* hull:kv:_native */
int hl_js_init_encoding_module(JSContext *ctx, HlJS *js);   /* hull:encoding:_native */
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
/* Drop the runtime's reference on the blob store (hl_js_free). */
void hl_js_blob_release(HlJS *js);
int hl_js_init_mime_module(JSContext *ctx, HlJS *js);
int hl_js_init_image_module(JSContext *ctx, HlJS *js);
int hl_js_init_tar_module(JSContext *ctx, HlJS *js);

#ifdef HL_ENABLE_WASM
int hl_js_init_compute_module(JSContext *ctx, HlJS *js);
#endif

int hl_js_init_gpu_module(JSContext *ctx, HlJS *js);

#ifdef HL_ENABLE_TUI
int hl_js_init_tui_module(JSContext *ctx, HlJS *js);
#endif

/* ── WebSocket modules (split: server in mod_ws_server.c,
 *    client in mod_ws_client.c) ─────────────────────────────────── */

int hl_js_init_ws_server_module(JSContext *ctx, HlJS *js);
int hl_js_init_ws_client_module(JSContext *ctx, HlJS *js);

/* Server-side conn class ID (in mod_ws_server.c) - referenced by
 * runtime/js/ws.c (Keel callback dispatcher). */
extern JSClassID js_ws_conn_class_id;

/* Outbound WebSocket connection class and opaque (mod_ws_client.c; here so
 * the binding tests can build one around a placeholder connection). */
#include "hull/shared/async.h"   /* HlAsyncTeardown */
extern JSClassID js_ws_client_conn_class_id;
struct KlWsClientConn;
typedef struct HlJSWsClientUD {
    struct KlWsClientConn *client; /* NULL after free (finalizer only) */
    int             closed;      /* 1 after on_close: fail methods closed,
                                  * but keep `client` so the finalizer still
                                  * frees it (Keel does not free the conn on
                                  * close) */
    JSValue         on_open;     /* DupValue'd callbacks */
    JSValue         on_message;
    JSValue         on_close;
    JSValue         on_error;
    /* A reference the object holds on itself while connected. Not reported
     * by gc_mark, so the cycle collector sees an outside reference and keeps
     * the object - and the callbacks it marks - as a root, whether or not
     * the app kept the handle (audit 15: reported, the self-cycle was
     * garbage, and a dropped client was collected and closed silently).
     * Dropped one loop turn after the close (js_ws_client_finish), or by
     * runtime teardown. */
    JSValue         self_ref;
    int             release_scheduled;  /* self_ref release already queued */
    uint64_t        release_timer;      /* its backend timer, 0 once fired */
    JSContext      *ctx;
    HlJS           *js;
    /* On the runtime's teardown list while `client` is held: the Keel
     * connection lives on the server's event loop, which is freed before the
     * runtime, so teardown frees it first (audit 14 G1). */
    HlAsyncTeardown teardown;
} HlJSWsClientUD;

/* WS conn helpers (defined in js/mod_ws.c) */
struct HlWsConn;
JSValue hl_js_ws_push_conn(JSContext *ctx, struct HlWsConn *conn);
void    hl_js_ws_invalidate_conn(JSContext *ctx, struct HlWsConn *conn);

/* ── SSE module (defined in js/mod_sse.c) ────────────────────────── */

extern JSClassID js_sse_stream_class_id;

struct HlReqLife;

/* SSE stream opaque (the Lua twin is HlSseStreamUD in lua/mod_buffer.h). */
#include <keel/http_sse.h>
typedef struct HlJSSseStreamUD {
    KlHttpSse         sse;
    int               closed;
    struct HlReqLife *life;   /* the request's life (shared/req_life.h) */
} HlJSSseStreamUD;

void    hl_js_sse_register_class(JSContext *ctx);
JSValue hl_js_sse_create_stream(JSContext *ctx, KlHttpResponse *res,
                                struct HlReqLife *life);
int     hl_js_sse_stream_is_closed(JSContext *ctx, JSValueConst val);
void    hl_js_sse_stream_force_close(JSContext *ctx, JSValueConst val);

#endif /* HULL_JS_MOD_BUFFER_H */
