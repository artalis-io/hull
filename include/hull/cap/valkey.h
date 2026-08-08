/*
 * cap/valkey.h: the Valkey/Redis HlKvBackend (feature-only).
 *
 * Exposes the backend vtable (registered into the feature archive's generated
 * hl_kv_feature_backends) and a test seam to build a handle over an existing
 * connection (a socketpair, so test_valkey_backend can drive the op->RESP
 * mapping without a live server).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_VALKEY_H
#define HL_CAP_VALKEY_H

#include "hull/cap/kv_backend.h"
#include "hull/cap/valkey_conn.h"

/* The backend dispatch table (static const in valkey.c). */
extern const HlKvBackend hl_kv_backend_valkey;

/* Test/reuse seam: wrap an already-connected+handshaken HlValkeyConn as an
 * HlKvHandle (takes ownership of conn). Returns NULL on OOM. */
HlKvHandle *hl_kv_valkey_wrap(HlValkeyConn *conn);

#endif /* HL_CAP_VALKEY_H */
