/*
 * ca_trust.c - the outbound TLS trust-anchor ladder (see ca_trust.h).
 *
 * Keel-free: it reaches TLS only through the hl_tls_* transport seam, so the
 * Keel-less base (serve_cli.o) can link it and a TLS-less base gets NULL back.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/ca_trust.h"
#include "hull/cacert.h"

#include "log.h"

#include <stddef.h>

KlTlsCtx *hl_ca_trust_resolve(int skip_verify, const char *override,
                              KlAllocator *alloc, const char **out_source)
{
    KlTlsCtx *ctx = NULL;
    const char *source = NULL;

    if (skip_verify) {
        log_warn("[hull:c] TLS certificate verification disabled (--no-ca-bundle)");
        ctx = hl_tls_client_ctx_create(NULL, alloc);
    } else if (override) {
        source = override;
        log_info("[hull:c] using CA bundle (override): %s", override);
        ctx = hl_tls_client_ctx_create(override, alloc);
        if (!ctx)
            log_warn("[hull:c] failed to load CA bundle from %s", override);
    } else {
        /* The system store is TRIED, not trusted to work. A path being
         * readable is not the same as its contents parsing, and both entry
         * points once logged "using CA bundle: X" and moved on without
         * looking at the result - so an unparseable store disabled outbound
         * TLS silently, with a log line claiming the opposite. Fall through
         * to the embedded bundle, which is what it is for. */
        const char *sys = hl_ca_bundle_find_system();
        if (sys) {
            log_info("[hull:c] using CA bundle: %s", sys);
            ctx = hl_tls_client_ctx_create(sys, alloc);
            if (ctx)
                source = sys;
            else
                log_warn("[hull:c] system CA bundle %s did not load; "
                         "falling back to the embedded bundle", sys);
        }
        if (!ctx) {
            const unsigned char *emb = NULL;
            size_t emb_len = 0;
            if (hl_embedded_ca_bundle(&emb, &emb_len) == 0) {
                log_info("[hull:c] using embedded CA bundle (%s)",
                         hl_embedded_ca_bundle_label());
                ctx = hl_tls_client_ctx_create_from_buf(emb, emb_len, alloc);
                /* Named even when it fails to parse, so doctor and
                 * introspection report which anchor was attempted. */
                source = "(embedded)";
                if (!ctx)
                    log_warn("[hull:c] failed to parse embedded CA bundle");
            } else {
                log_warn("[hull:c] no CA bundle found; outbound TLS disabled "
                         "(use --no-ca-bundle, --ca-bundle PATH, or build "
                         "with HL_EMBED_CA_BUNDLE=1)");
            }
        }
    }

    if (out_source) *out_source = source;
    return ctx;
}
