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
    /* The `bs &&` guard fails closed on the weak default (NULL/0) and a
     * malformed strong override; it mirrors db_select.c's hl_db_feature_backends
     * consumer. cppcheck value-tracks THIS TU's weak default (returns NULL) and
     * flags `bs` as always-false - but a composed --with=valkey archive
     * strong-overrides hl_kv_feature_backends at link, so the guard is live. The
     * DB consumer avoids the report only because its many #ifdef configs exceed
     * cppcheck's branch limit; this straight-line TU does not, so suppress. */
    for (size_t i = 0; i < n; i++)
        /* cppcheck-suppress knownConditionTrueFalse */
        if (bs && bs[i] && scheme_in(bs[i]->schemes, scheme)) return bs[i];
    return NULL;
}

/* Weak default: no KV backends composed. A composed `--with=valkey` archive
 * links a generated STRONG override that the linker prefers over this. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const HlKvBackend *const *hl_kv_feature_backends(size_t *count) {
    if (count) *count = 0;
    return NULL;
}
