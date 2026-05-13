/*
 * cacert.c — Embedded Mozilla CA bundle accessor
 *
 * Compiled into libhull_platform.a so hull AND apps built via `hull build`
 * both inherit the embedded bundle. When HL_EMBED_CA_BUNDLE is undefined,
 * the accessor returns -1 and clients fall back to the system CA path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cacert.h"

#ifdef HL_EMBED_CA_BUNDLE
#include "embedded_cacert.h"
/* xxd emits:
 *   unsigned char embedded_cacert[];
 *   unsigned int  embedded_cacert_len;
 * (We append a trailing NUL during generation, so len includes it.) */
#endif

int hl_embedded_ca_bundle(const unsigned char **data, size_t *len)
{
    if (!data || !len) return -1;
#ifdef HL_EMBED_CA_BUNDLE
    *data = embedded_cacert;
    *len  = embedded_cacert_len;
    return 0;
#else
    *data = NULL;
    *len  = 0;
    return -1;
#endif
}

const char *hl_embedded_ca_bundle_label(void)
{
#ifdef HL_EMBED_CA_BUNDLE
    /* HL_CA_BUNDLE_DATE is set by the Makefile when generating the header */
    #ifdef HL_CA_BUNDLE_DATE
    return HL_CA_BUNDLE_DATE;
    #else
    return "embedded";
    #endif
#else
    return "none";
#endif
}
