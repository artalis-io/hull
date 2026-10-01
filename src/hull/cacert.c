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
#include <sh_seal_arena.h>

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

/* The published anchor (see cacert.h). Plain statics: written at startup,
 * read-only afterwards. */
static int                  g_active_set;
static const unsigned char *g_active_data;
static size_t               g_active_len;
static unsigned char       *g_active_owned;

/* Owned buffers a later publish replaced. Startup can publish twice (early
 * for a network -d DSN, then at the full resolve), and the worker pool
 * already exists by the second, so the first buffer is kept rather than
 * freed under a reader. Freed only by reset, which runs when nothing is
 * reading. Two publishes happen at most; past the bound a buffer is
 * leaked, never freed early. */
#define HL_CA_RETIRED_MAX 4
static unsigned char *g_retired[HL_CA_RETIRED_MAX];
static int            g_retired_n;

/* The SEALED anchor (see hl_ca_bundle_seal_active): the descriptor - and the
 * bytes, when they were a heap copy - in a read-only arena. Once set, it is
 * all hl_ca_bundle_active reads, and no publish changes it. */
typedef struct {
    int                  set;
    const unsigned char *data;
    size_t               len;
} CaAnchor;

static ShSealArena     g_seal_arena;
static const CaAnchor *g_sealed;

void hl_ca_bundle_reset_active(void)
{
    if (g_sealed) {
        sh_seal_arena_destroy(&g_seal_arena);
        g_sealed = NULL;
    }
    free(g_active_owned);
    for (int i = 0; i < g_retired_n; i++) free(g_retired[i]);
    g_retired_n    = 0;
    g_active_owned = NULL;
    g_active_data  = NULL;
    g_active_len   = 0;
    g_active_set   = 0;
}

void hl_ca_bundle_set_active(const unsigned char *data, size_t len, int owned)
{
    if (g_sealed) {
        /* Startup is over; nothing may swap the anchor now. */
        fprintf(stderr, "[hull:c] CA anchor publish refused: already sealed\n");
        if (owned) free((void *)(uintptr_t)data);
        return;
    }
    if (g_active_owned && g_retired_n < HL_CA_RETIRED_MAX)
        g_retired[g_retired_n++] = g_active_owned;
    g_active_owned = NULL;
    g_active_set   = 1;
    g_active_data  = data;
    g_active_len   = data ? len : 0;
    g_active_owned = (owned && data) ? (unsigned char *)(uintptr_t)data : NULL;
}

int hl_ca_bundle_active(const unsigned char **data, size_t *len)
{
    if (!data || !len) return -1;
    int                  set = g_sealed ? g_sealed->set  : g_active_set;
    const unsigned char *d   = g_sealed ? g_sealed->data : g_active_data;
    size_t               n   = g_sealed ? g_sealed->len  : g_active_len;
    if (!set) return hl_embedded_ca_bundle(data, len);
    if (!d || n == 0) {
        *data = NULL;
        *len  = 0;
        return -1;
    }
    *data = d;
    *len  = n;
    return 0;
}

int hl_ca_bundle_seal_active(void)
{
    if (g_sealed) return 0;   /* nothing can have changed since */

    /* A heap anchor (--ca-bundle, the system store) is copied in; the
     * embedded one is .rodata already and is only pointed at. */
    size_t copy = g_active_owned ? g_active_len : 0;
    if (sh_seal_arena_init(&g_seal_arena, sizeof(CaAnchor) + copy + 64,
                           "hull-ca-anchor") != 0)
        return -1;
    CaAnchor *a = sh_seal_arena_alloc(&g_seal_arena, sizeof *a,
                                      _Alignof(CaAnchor));
    unsigned char *bytes = copy
        ? sh_seal_arena_memdup(&g_seal_arena, g_active_data, copy) : NULL;
    if (!a || (copy && !bytes)) {
        sh_seal_arena_destroy(&g_seal_arena);
        return -1;
    }
    a->set  = g_active_set;
    a->data = copy ? bytes : g_active_data;
    a->len  = g_active_len;
    if (sh_seal_arena_seal(&g_seal_arena) != 0) {
        sh_seal_arena_destroy(&g_seal_arena);
        return -1;
    }
    /* The heap copy stays allocated (retired): a connection that read the
     * pointer just before this may still be parsing it. Freed by reset. */
    g_sealed = a;
    return 0;
}
