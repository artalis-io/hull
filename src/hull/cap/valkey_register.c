/*
 * cap/valkey_register.c: strong hl_kv_feature_backends for the COMPILED-IN
 * Valkey/Redis backend (make HL_ENABLE_VALKEY=1).
 *
 * This one-symbol TU exists so the dev/test base can reach the backend through
 * the same hl_kv_backend_select path a --with= feature uses, WITHOUT a build-time
 * macro. The compiled-in base links this object (shadowing the weak default in
 * cap/kv_feature.c); the --with=valkey feature archive deliberately OMITS it, so
 * the generated feature_registry.c is the sole provider there and there is never
 * a duplicate symbol. It is filtered out of a non-valkey base build alongside
 * valkey.c / valkey_conn.c / respwire.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/valkey.h"

static const HlKvBackend *const g_valkey_backends[] = { &hl_kv_backend_valkey };

const HlKvBackend *const *hl_kv_feature_backends(size_t *count)
{
    if (count) *count = 1;
    return g_valkey_backends;
}
