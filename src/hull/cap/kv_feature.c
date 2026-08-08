/*
 * cap/kv_feature.c: base-resident KV feature seam.
 *
 * The weak default for hl_kv_feature_backends (returns an empty set) plus the
 * DSN-scheme selector. A composed `--with=valkey` feature archive links a
 * STRONG hl_kv_feature_backends (generated feature_registry.c) that shadows the
 * weak one; the selector then finds the composed backend by scheme. This file
 * is ALWAYS in the base (it is NOT excluded by HL_ENABLE_VALKEY), so a base
 * binary resolves KV schemes to "no backend -> hint to install the feature".
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/kv_backend.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Weak default: no KV backends composed. A feature archive overrides this. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const HlKvBackend *const *hl_kv_feature_backends(size_t *count) {
    if (count) *count = 0;
    return NULL;
}

/* Extract the DSN scheme (before "://") lowercased into buf. Returns length. */
static size_t dsn_scheme(const char *dsn, char *buf, size_t bufsz) {
    if (!dsn) return 0;
    const char *sep = strstr(dsn, "://");
    if (!sep) return 0;
    size_t n = (size_t)(sep - dsn);
    if (n == 0 || n >= bufsz) return 0;
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)dsn[i]);
    buf[n] = '\0';
    return n;
}

static int scheme_in(const char *const *schemes, const char *want) {
    if (!schemes) return 0;
    for (size_t i = 0; schemes[i]; i++)
        if (strcmp(schemes[i], want) == 0) return 1;
    return 0;
}

const HlKvBackend *hl_kv_backend_select(const char *dsn) {
    char scheme[24];
    if (dsn_scheme(dsn, scheme, sizeof scheme) == 0) return NULL;
    size_t n = 0;
    const HlKvBackend *const *bs = hl_kv_feature_backends(&n);
    for (size_t i = 0; i < n; i++)
        if (bs[i] && scheme_in(bs[i]->schemes, scheme)) return bs[i];
    return NULL;
}
