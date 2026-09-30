/*
 * test_ca_trust.c - the outbound trust-anchor ladder publishes what it chose.
 *
 * hl_ca_trust_resolve builds the context http.fetch and the SSH tunnel use,
 * and publishes the same anchor for the clients that build a TLS context per
 * connection (the DB / KV wire backends, the async SMTP workers). These check
 * that the anchor those clients read back is the one the ladder chose.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cacert.h"
#include "hull/ca_trust.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HL_EMBED_CA_BUNDLE

/* Write the embedded bundle to a file, standing in for a private CA the
 * operator names with --ca-bundle. Returns the path, or NULL. */
static const char *write_bundle_file(char *path, size_t cap)
{
    const unsigned char *data = NULL;
    size_t len = 0;
    if (hl_embedded_ca_bundle(&data, &len) != 0) return NULL;
    const char *tmp = getenv("TMPDIR");
    snprintf(path, cap, "%s/hull_test_ca_%d.pem", tmp && *tmp ? tmp : "/tmp",
             (int)(len & 0xffff));
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    size_t body = len - 1;            /* the embedded length counts a NUL */
    int ok = fwrite(data, 1, body, f) == body;
    fclose(f);
    return ok ? path : NULL;
}

UTEST(ca_trust, a_named_bundle_is_what_every_client_trusts)
{
    char path[512];
    ASSERT_TRUE(write_bundle_file(path, sizeof path) != NULL);

    KlAllocator alloc = kl_allocator_default();
    const char *source = NULL;
    KlTlsCtx *ctx = hl_ca_trust_resolve(0, path, &alloc, &source);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_STREQ(source, path);

    /* The per-connection clients now load the file's bytes, not the embedded
     * copy: same content here, a different buffer. */
    const unsigned char *a = NULL, *e = NULL;
    size_t alen = 0, elen = 0;
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), 0);
    ASSERT_EQ(hl_embedded_ca_bundle(&e, &elen), 0);
    ASSERT_TRUE(a != e);
    ASSERT_EQ(alen, elen);               /* file + the NUL a PEM gets */
    ASSERT_EQ(memcmp(a, e, elen), 0);

    hl_tls_ctx_destroy(ctx);
    hl_ca_bundle_reset_active();
    remove(path);
}

UTEST(ca_trust, an_early_publish_needs_no_context)
{
    /* What an entry point does for a network -d DSN before the manifest is
     * loaded: the bytes are published, no context is kept. */
    char path[512];
    ASSERT_TRUE(write_bundle_file(path, sizeof path) != NULL);
    hl_ca_bundle_reset_active();
    hl_ca_trust_publish(0, path);
    const unsigned char *a = NULL, *e = NULL;
    size_t alen = 0, elen = 0;
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), 0);
    ASSERT_EQ(hl_embedded_ca_bundle(&e, &elen), 0);
    ASSERT_TRUE(a != e);
    ASSERT_EQ(memcmp(a, e, elen), 0);

    hl_ca_trust_publish(0, "/nonexistent/hull-ca.pem");
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), -1);
    hl_ca_bundle_reset_active();
    remove(path);
}

UTEST(ca_trust, a_named_bundle_that_fails_leaves_no_anchor)
{
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *ctx = hl_ca_trust_resolve(0, "/nonexistent/hull-ca.pem", &alloc, NULL);
    ASSERT_TRUE(ctx == NULL);
    const unsigned char *a = NULL;
    size_t alen = 0;
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), -1);
    hl_ca_bundle_reset_active();
}

UTEST(ca_trust, no_verify_publishes_nothing)
{
    /* --no-ca-bundle is for the http / tunnel clients; a DSN that asks for
     * verify-full keeps the embedded bundle. */
    hl_ca_bundle_reset_active();
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *ctx = hl_ca_trust_resolve(1, NULL, &alloc, NULL);
    const unsigned char *a = NULL, *e = NULL;
    size_t alen = 0, elen = 0;
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), 0);
    ASSERT_EQ(hl_embedded_ca_bundle(&e, &elen), 0);
    ASSERT_TRUE(a == e);
    if (ctx) hl_tls_ctx_destroy(ctx);
}

#endif /* HL_EMBED_CA_BUNDLE */

UTEST(ca_trust, the_file_bytes_survive_a_second_resolve)
{
    /* Resolving again (a test harness, a re-wire) frees the previous owned
     * anchor and publishes the new one; nothing dangles. */
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *c1 = hl_ca_trust_resolve(0, NULL, &alloc, NULL);
    KlTlsCtx *c2 = hl_ca_trust_resolve(0, NULL, &alloc, NULL);
    const unsigned char *a = NULL;
    size_t alen = 0;
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), 0);
    ASSERT_GT(alen, (size_t)0);
    if (c1) hl_tls_ctx_destroy(c1);
    if (c2) hl_tls_ctx_destroy(c2);
    hl_ca_bundle_reset_active();
}

UTEST_MAIN()
