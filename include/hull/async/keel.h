/**
 * @file async/keel.h
 * @brief Keel-backend-specific glue.
 *
 * Functions that don't fit the generic HlAsyncBackend vtable because
 * they expose Keel types. Used by serve.c (which already speaks Keel)
 * to wire the existing KlHttpServer event loop into the vtable-aware
 * world so other consumers don't have to know about KlHttpServer at all.
 *
 * Callers outside the Keel-aware boundary should NOT include this
 * header - they go through include/hull/async_backend.h's vtable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_ASYNC_KEEL_H
#define HL_ASYNC_KEEL_H

typedef struct HlAsyncBackendCtx HlAsyncBackendCtx;
typedef struct KlEventCtx        KlEventCtx;

/**
 * Wrap an existing KlEventCtx into an HlAsyncBackendCtx that doesn't
 * own the underlying loop. Use this when the loop is owned by a
 * KlHttpServer and you want vtable consumers to share it - instead of
 * calling backend->init() which would create a fresh loop.
 *
 * The returned ctx must be freed with hl_async_backend_keel_unwrap,
 * which only releases the wrapper - the underlying KlEventCtx
 * remains owned by the caller (typically KlHttpServer).
 *
 * Returns NULL on allocation failure.
 */
HlAsyncBackendCtx *hl_async_backend_keel_wrap(KlEventCtx *ev);

/**
 * Free a wrapper returned by hl_async_backend_keel_wrap. Does NOT
 * destroy the underlying KlEventCtx.
 */
void hl_async_backend_keel_unwrap(HlAsyncBackendCtx *ctx);

#endif /* HL_ASYNC_KEEL_H */
