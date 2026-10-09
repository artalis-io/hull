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

#include "hull/utils/alloc.h"

#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A CA bundle larger than this is not a CA bundle. The system stores are a few
 * hundred KiB; the bound keeps a mistyped --ca-bundle (a disk image, a log)
 * from being read whole. */
#define CA_FILE_MAX (16u * 1024u * 1024u)

/* Read a bundle into memory, in the form hl_tls_client_ctx_create_from_buf
 * takes: PEM with a trailing NUL counted in the length (mbedTLS needs it to
 * recognise PEM), or DER exactly as stored. NULL on any failure. */
static unsigned char *read_bundle(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= CA_FILE_MAX) { free(buf); fclose(f); return NULL; }
            size_t ncap = cap ? cap * 2 : 64u * 1024u;
            if (ncap > CA_FILE_MAX) ncap = CA_FILE_MAX;
            unsigned char *nb = realloc(buf, ncap + 1);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
            cap = ncap;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    int err = ferror(f);
    fclose(f);
    if (err || len == 0) { free(buf); return NULL; }

    buf[len] = '\0';
    /* PEM: count the NUL. DER: leave it out - a trailing byte would make the
     * certificate's length disagree with its encoding. */
    const char *marker = "-----BEGIN ";
    int is_pem = 0;
    for (size_t i = 0; i + strlen(marker) <= len; i++) {
        if (memcmp(buf + i, marker, strlen(marker)) == 0) { is_pem = 1; break; }
    }
    *out_len = is_pem ? len + 1 : len;
    return buf;
}

/* Load one file anchor: its bytes, and a context parsed from them. Both or
 * neither. */
static KlTlsCtx *load_file(const char *path, KlAllocator *alloc,
                           unsigned char **out_bytes, size_t *out_len)
{
    size_t len = 0;
    unsigned char *bytes = read_bundle(path, &len);
    if (!bytes) return NULL;
    KlTlsCtx *ctx = hl_tls_client_ctx_create_from_buf(bytes, len, alloc);
    if (!ctx) { free(bytes); return NULL; }
    *out_bytes = bytes;
    *out_len   = len;
    return ctx;
}

/* Quiet mode logs nothing: an early publish is followed by the full resolve,
 * which reports the same choice once. */
#define LOG_INFO(...) do { if (!quiet) log_info(__VA_ARGS__); } while (0)
#define LOG_WARN(...) do { if (!quiet) log_warn(__VA_ARGS__); } while (0)

static KlTlsCtx *ladder(int skip_verify, const char *override,
                        KlAllocator *alloc, const char **out_source, int quiet)
{
    KlTlsCtx *ctx = NULL;
    const char *source = NULL;
    unsigned char *bytes = NULL;
    size_t len = 0;

    if (skip_verify) {
        LOG_WARN("[hull:c] TLS certificate verification disabled (--no-ca-bundle)");
        ctx = hl_tls_client_ctx_create_insecure(alloc);
        /* Nothing is published: a database DSN that asks for verify-full
         * asked for verification itself, so it keeps the embedded bundle
         * rather than inheriting a development switch meant for the
         * http / tunnel clients. */
    } else if (override) {
        source = override;
        LOG_INFO("[hull:c] using CA bundle (override): %s", override);
        ctx = load_file(override, alloc, &bytes, &len);
        if (ctx) {
            hl_ca_bundle_set_active(bytes, len, 1);
        } else {
            LOG_WARN("[hull:c] failed to load CA bundle from %s", override);
            /* Named, so it fails closed everywhere: no other anchor is
             * quietly substituted for the one the operator chose. */
            hl_ca_bundle_set_active(NULL, 0, 0);
        }
    } else {
        /* The system store is TRIED, not trusted to work. A path being
         * readable is not the same as its contents parsing, and both entry
         * points once logged "using CA bundle: X" and moved on without
         * looking at the result - so an unparseable store disabled outbound
         * TLS silently, with a log line claiming the opposite. Fall through
         * to the embedded bundle, which is what it is for. */
        const char *sys = hl_ca_bundle_find_system();
        if (sys) {
            LOG_INFO("[hull:c] using CA bundle: %s", sys);
            ctx = load_file(sys, alloc, &bytes, &len);
            if (ctx) {
                source = sys;
                hl_ca_bundle_set_active(bytes, len, 1);
            } else {
                LOG_WARN("[hull:c] system CA bundle %s did not load; "
                         "falling back to the embedded bundle", sys);
            }
        }
        if (!ctx) {
            const unsigned char *emb = NULL;
            size_t emb_len = 0;
            if (hl_embedded_ca_bundle(&emb, &emb_len) == 0) {
                LOG_INFO("[hull:c] using embedded CA bundle (%s)",
                         hl_embedded_ca_bundle_label());
                ctx = hl_tls_client_ctx_create_from_buf(emb, emb_len, alloc);
                /* Named even when it fails to parse, so doctor and
                 * introspection report which anchor was attempted. */
                source = "(embedded)";
                if (!ctx)
                    LOG_WARN("[hull:c] failed to parse embedded CA bundle");
                /* Published like any other anchor, so a heap anchor an
                 * EARLY publish left (the system store, which then failed
                 * here) is retired, not freed under a worker reading it. */
                hl_ca_bundle_set_active(emb, emb_len, 0);
            } else {
                LOG_WARN("[hull:c] no CA bundle found; outbound TLS disabled "
                         "(use --no-ca-bundle, --ca-bundle PATH, or build "
                         "with HL_EMBED_CA_BUNDLE=1)");
            }
        }
    }

    if (out_source) *out_source = source;
    return ctx;
}

KlTlsCtx *hl_ca_trust_resolve(int skip_verify, const char *override,
                              KlAllocator *alloc, const char **out_source)
{
    return ladder(skip_verify, override, alloc, out_source, 0);
}

void hl_ca_trust_publish(int skip_verify, const char *override)
{
    /* The context only proves the bytes parse; the clients that read the
     * anchor build their own per connection. */
    HlAllocator a;
    hl_alloc_init(&a, 0);
    KlAllocator k = hl_alloc_kl(&a);
    KlTlsCtx *ctx = ladder(skip_verify, override, &k, NULL, 1);
    if (ctx) hl_tls_ctx_destroy(ctx);
}
