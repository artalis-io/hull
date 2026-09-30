/*
 * cacert.c - Embedded Mozilla CA bundle accessor
 *
 * Compiled into libhull_platform.a so hull AND apps built via `hull build`
 * both inherit the embedded bundle. When HL_EMBED_CA_BUNDLE is undefined,
 * the accessor returns -1 and clients fall back to the system CA path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "hull/cacert.h"

#ifdef HL_EMBED_CA_BUNDLE
#include "embedded_cacert.h"
/* Generated header emits (after Makefile post-process for `const`):
 *   const unsigned char embedded_cacert[];
 *   const unsigned int  embedded_cacert_len;
 *
 * The `const` lands the bundle in `.rodata` so the OS read-only
 * segment protection prevents post-boot rewrites of the trust
 * store. Defense-in-depth against an attacker with a heap memory-
 * write bug who'd otherwise be able to add their own root CA.
 *
 * (We append a trailing NUL during generation, so len includes it
 * - mbedtls_x509_crt_parse requires NUL-termination for PEM.) */
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

/* The system store. Probed by opening rather than by stat: a path that exists
 * but cannot be read is not a usable bundle, and finding that out here beats
 * finding out inside a TLS handshake. */
const char *hl_ca_bundle_find_system(void)
{
    static const char *paths[] = {
        "/etc/ssl/cert.pem",                    /* macOS, Alpine */
        "/etc/ssl/certs/ca-certificates.crt",   /* Debian/Ubuntu */
        "/etc/pki/tls/certs/ca-bundle.crt",     /* RHEL/CentOS */
        NULL,
    };
    for (const char **p = paths; *p; p++) {
        FILE *f = fopen(*p, "r");
        if (f) { fclose(f); return *p; }
    }
    return NULL;
}

/* The published anchor (see cacert.h). Plain statics: written once at
 * startup before any connection exists, read-only afterwards. */
static int                  g_active_set;
static const unsigned char *g_active_data;
static size_t               g_active_len;
static unsigned char       *g_active_owned;

void hl_ca_bundle_reset_active(void)
{
    free(g_active_owned);
    g_active_owned = NULL;
    g_active_data  = NULL;
    g_active_len   = 0;
    g_active_set   = 0;
}

void hl_ca_bundle_set_active(const unsigned char *data, size_t len, int owned)
{
    hl_ca_bundle_reset_active();
    g_active_set   = 1;
    g_active_data  = data;
    g_active_len   = data ? len : 0;
    g_active_owned = (owned && data) ? (unsigned char *)(uintptr_t)data : NULL;
}

int hl_ca_bundle_active(const unsigned char **data, size_t *len)
{
    if (!data || !len) return -1;
    if (!g_active_set) return hl_embedded_ca_bundle(data, len);
    if (!g_active_data || g_active_len == 0) {
        *data = NULL;
        *len  = 0;
        return -1;
    }
    *data = g_active_data;
    *len  = g_active_len;
    return 0;
}
