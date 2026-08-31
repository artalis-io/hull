/*
 * reqctx.h - Tagged per-request context for middleware->handler data passing
 *
 * Replaces the raw void* + size_t-prefix layout on KlHttpRequest.ctx.
 * Supports three kinds: JSON string (test dispatch), Lua registry ref,
 * and JS value ref - avoiding JSON round-trips between middleware hops.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_REQCTX_H
#define HL_REQCTX_H

#include <stddef.h>
#include <stdint.h>

#define HL_REQCTX_JSON     1   /* JSON string (test dispatch) */
#define HL_REQCTX_LUA_REF  2   /* Lua registry reference */
#define HL_REQCTX_JS_VAL   3   /* JS value (stored as raw bytes) */

typedef struct HlReqCtx {
    int     kind;
    union {
        struct {
            char   *data;      /* owned, NUL-terminated */
            size_t  len;
        } json;                /* kind == HL_REQCTX_JSON */
        int lua_ref;           /* kind == HL_REQCTX_LUA_REF, LUA_REGISTRYINDEX ref */
        char js_val_bytes[16]; /* kind == HL_REQCTX_JS_VAL, JSValue stored as bytes
                                * (16 bytes covers both NaN-boxing uint64_t and
                                *  struct { JSValueUnion; int64_t } layouts) */
    };
} HlReqCtx;

#endif /* HL_REQCTX_H */
