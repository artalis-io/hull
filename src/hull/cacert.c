/*
 * cacert.c - Embedded Mozilla CA bundle accessor
 *
 * Compiled into libhull_platform.a so hull AND apps built via `hull build`
 * both inherit the embedded bundle. When HL_EMBED_CA_BUNDLE is undefined,
 * the accessor returns -1 and clients fall back to the system CA path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdatomic.h>
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

/* The published anchor (see cacert.h): ONE immutable record behind ONE
 * atomic pointer (C audit L4a). A reader loads the pointer once and reads a
 * record nobody writes again, so it can never see a torn {set, data, len}
 * - three plain globals written at startup let a thread started early read
 * the new data with the old length. Records (and the owned bytes they name)
 * are never freed while anything may read them: a later publish retires the
 * old record, and only reset, which runs when nothing is reading, frees
 * them. Startup publishes twice at most (early for a network -d DSN, then
 * at the full resolve); past the bound a record is leaked, never freed
 * early. */
typedef struct {
    int                  set;
    const unsigned char *data;
    size_t               len;
} CaAnchor;

#define HL_CA_RETIRED_MAX 4
static _Atomic(const CaAnchor *) g_anchor;
static CaAnchor      *g_recs[HL_CA_RETIRED_MAX];     /* heap records */
static int            g_recs_n;
static unsigned char *g_owned[HL_CA_RETIRED_MAX];    /* heap bundle bytes */
static int            g_owned_n;
/* The writer's view of the current record's bytes when they are a heap
 * copy (seal copies them into the read-only arena). Startup thread only. */
static unsigned char *g_cur_owned;

/* Publishing fails closed: a record that cannot be allocated is replaced
 * by "an anchor was resolved, and it is empty", so verifying clients fail
 * rather than fall back to the embedded bundle. */
static const CaAnchor g_failed_anchor = { 1, NULL, 0 };

/* The SEALED anchor (see hl_ca_bundle_seal_active): the record - and the
 * bytes, when they were a heap copy - in a read-only arena. Once set it is
 * what g_anchor points at, and no publish changes it. */
static ShSealArena     g_seal_arena;
static const CaAnchor *g_sealed;

void hl_ca_bundle_reset_active(void)
{
    atomic_store_explicit(&g_anchor, NULL, memory_order_release);
    if (g_sealed) {
        sh_seal_arena_destroy(&g_seal_arena);
        g_sealed = NULL;
    }
    for (int i = 0; i < g_recs_n; i++) free(g_recs[i]);
    for (int i = 0; i < g_owned_n; i++) free(g_owned[i]);
    g_recs_n    = 0;
    g_owned_n   = 0;
    g_cur_owned = NULL;
}

void hl_ca_bundle_set_active(const unsigned char *data, size_t len, int owned)
{
    if (g_sealed) {
        /* Startup is over; nothing may swap the anchor now. */
        fprintf(stderr, "[hull:c] CA anchor publish refused: already sealed\n");
        if (owned) free((void *)(uintptr_t)data);
        return;
    }
    unsigned char *own = (owned && data) ? (unsigned char *)(uintptr_t)data
                                         : NULL;
    if (own) {
        if (g_owned_n < HL_CA_RETIRED_MAX) g_owned[g_owned_n++] = own;
        /* else leaked: never freed under a reader */
    }
    CaAnchor *rec = NULL;
    if (g_recs_n < HL_CA_RETIRED_MAX) rec = malloc(sizeof *rec);
    if (!rec) {
        g_cur_owned = NULL;
        atomic_store_explicit(&g_anchor, &g_failed_anchor,
                              memory_order_release);
        return;
    }
    rec->set  = 1;
    rec->data = data;
    rec->len  = data ? len : 0;
    g_recs[g_recs_n++] = rec;
    g_cur_owned = own;
    atomic_store_explicit(&g_anchor, rec, memory_order_release);
}

int hl_ca_bundle_active(const unsigned char **data, size_t *len)
{
    if (!data || !len) return -1;
    const CaAnchor *a = atomic_load_explicit(&g_anchor, memory_order_acquire);
    if (!a || !a->set) return hl_embedded_ca_bundle(data, len);
    if (!a->data || a->len == 0) {
        *data = NULL;
        *len  = 0;
        return -1;
    }
    *data = a->data;
    *len  = a->len;
    return 0;
}

int hl_ca_bundle_seal_active(void)
{
    if (g_sealed) return 0;   /* nothing can have changed since */

    const CaAnchor *cur = atomic_load_explicit(&g_anchor,
                                               memory_order_acquire);
    /* A heap anchor (--ca-bundle, the system store) is copied in; the
     * embedded one is .rodata already and is only pointed at. */
    size_t copy = (cur && g_cur_owned) ? cur->len : 0;
    if (sh_seal_arena_init(&g_seal_arena, sizeof(CaAnchor) + copy + 64,
                           "hull-ca-anchor") != 0)
        return -1;
    CaAnchor *a = sh_seal_arena_alloc(&g_seal_arena, sizeof *a,
                                      _Alignof(CaAnchor));
    unsigned char *bytes = copy
        ? sh_seal_arena_memdup(&g_seal_arena, cur->data, copy) : NULL;
    if (!a || (copy && !bytes)) {
        sh_seal_arena_destroy(&g_seal_arena);
        return -1;
    }
    a->set  = cur ? cur->set : 0;
    a->data = copy ? bytes : (cur ? cur->data : NULL);
    a->len  = cur ? cur->len : 0;
    if (sh_seal_arena_seal(&g_seal_arena) != 0) {
        sh_seal_arena_destroy(&g_seal_arena);
        return -1;
    }
    /* The heap record and copy stay allocated (retired): a connection that
     * loaded the pointer just before this may still be parsing it. Freed by
     * reset. */
    g_sealed = a;
    atomic_store_explicit(&g_anchor, a, memory_order_release);
    return 0;
}
