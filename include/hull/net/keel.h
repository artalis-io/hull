/**
 * @file net/keel.h
 * @brief Keel-backend-specific glue for HlNetBackend.
 *
 * Mirror of include/hull/async/keel.h, one layer up: functions that
 * don't fit the generic HlNetBackend vtable because they expose Keel
 * types directly. Used by serve.c to wrap an existing KlServer into
 * an HlNetBackendCtx, so other consumers can route async-op
 * suspend/complete through the vtable instead of calling
 * kl_async_suspend / kl_async_complete by hand.
 *
 * Callers outside the Keel-aware boundary should NOT include this
 * header — they go through include/hull/net_backend.h's vtable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_NET_KEEL_H
#define HL_NET_KEEL_H

typedef struct HlNetBackendCtx HlNetBackendCtx;
typedef struct KlServer        KlServer;

/**
 * Wrap an existing KlServer into an HlNetBackendCtx that doesn't own
 * the underlying server. Use this in server-mode (HTTP=1) so vtable
 * consumers can route async-op suspend/complete through the net
 * backend instead of touching Keel directly.
 *
 * The returned ctx must be freed with hl_net_backend_keel_unwrap,
 * which only releases the wrapper — KlServer remains owned by the
 * caller.
 *
 * Returns NULL on allocation failure.
 */
HlNetBackendCtx *hl_net_backend_keel_wrap(KlServer *server);

/**
 * Free a wrapper returned by hl_net_backend_keel_wrap. Does NOT
 * destroy the wrapped KlServer.
 */
void hl_net_backend_keel_unwrap(HlNetBackendCtx *ctx);

#endif /* HL_NET_KEEL_H */
