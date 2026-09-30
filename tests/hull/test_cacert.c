/*
 * test_cacert.c - Tests for the embedded CA bundle accessor.
 *
 * When HL_EMBED_CA_BUNDLE is defined at build time, the bundle should:
 *   - be non-empty (> 100KB Mozilla bundle)
 *   - be NUL-terminated (last byte == 0) for mbedtls_x509_crt_parse PEM
 *   - parse as valid PEM (multiple BEGIN CERTIFICATE markers)
 *   - have a non-"none" label
 *
 * When HL_EMBED_CA_BUNDLE is undefined, the accessor returns -1.
 *
 * The ACTIVE anchor (hl_ca_bundle_active) is what per-connection TLS clients
 * load - the DB / KV wire backends and the async SMTP workers. It is the
 * embedded bundle until hl_ca_trust_resolve publishes the one --ca-bundle or
 * the system store supplied, and "no anchor" when a named bundle fails.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cacert.h"

#include <stdlib.h>
#include <string.h>

UTEST(cacert, accessor_null_args_safe)
{
    /* NULL pointers must not crash */
    ASSERT_EQ(hl_embedded_ca_bundle(NULL, NULL), -1);
}

#ifdef HL_EMBED_CA_BUNDLE

UTEST(cacert, embedded_bundle_present)
{
    const unsigned char *data = NULL;
    size_t len = 0;
    int rc = hl_embedded_ca_bundle(&data, &len);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(data, NULL);
    /* Mozilla bundle is ~225KB - sanity-check it's at least 100KB */
    ASSERT_GT(len, (size_t)100000);
}

UTEST(cacert, embedded_bundle_nul_terminated)
{
    /* mbedtls_x509_crt_parse requires PEM input to be NUL-terminated.
     * The Makefile recipe appends a NUL byte during xxd generation. */
    const unsigned char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(hl_embedded_ca_bundle(&data, &len), 0);
    ASSERT_EQ(data[len - 1], (unsigned char)0);
}

UTEST(cacert, embedded_bundle_is_pem)
{
    const unsigned char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(hl_embedded_ca_bundle(&data, &len), 0);

    /* Count "BEGIN CERTIFICATE" markers - Mozilla bundle has 100+ certs */
    const char *needle = "-----BEGIN CERTIFICATE-----";
    size_t nlen = strlen(needle);
    int count = 0;
    for (size_t i = 0; i + nlen < len; i++) {
        if (memcmp(data + i, needle, nlen) == 0) count++;
    }
    ASSERT_GT(count, 50);
}

UTEST(cacert, label_not_none)
{
    const char *label = hl_embedded_ca_bundle_label();
    ASSERT_NE(label, NULL);
    ASSERT_STRNE(label, "none");
}

UTEST(cacert, parseable_by_mbedtls)
{
    /* The whole point: feed it to mbedTLS and confirm it parses. */
    const unsigned char *data = NULL;
    size_t len = 0;
    ASSERT_EQ(hl_embedded_ca_bundle(&data, &len), 0);

    /* mbedtls_x509_crt_parse is the actual API Keel uses. */
    extern int mbedtls_x509_crt_parse(void *chain,
                                        const unsigned char *buf, size_t buflen);
    extern void mbedtls_x509_crt_init(void *chain);
    extern void mbedtls_x509_crt_free(void *chain);

    /* mbedtls_x509_crt is large (~200 bytes); allocate a generous buffer. */
    unsigned char chain_buf[4096] = {0};
    mbedtls_x509_crt_init(chain_buf);
    int rc = mbedtls_x509_crt_parse(chain_buf, data, len);
    /* rc > 0 means "this many certs failed to parse" - Mozilla bundle is clean,
     * but some old roots may have non-fatal warnings. rc == 0 is ideal. */
    mbedtls_x509_crt_free(chain_buf);
    ASSERT_LE(rc, 5);
}

#else  /* !HL_EMBED_CA_BUNDLE */

UTEST(cacert, no_bundle_when_disabled)
{
    const unsigned char *data = (const unsigned char *)1;
    size_t len = 42;
    int rc = hl_embedded_ca_bundle(&data, &len);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(data, NULL);
    ASSERT_EQ(len, (size_t)0);
}

UTEST(cacert, label_is_none)
{
    ASSERT_STREQ(hl_embedded_ca_bundle_label(), "none");
}

#endif

/* ── the active anchor ─────────────────────────────────────────────── */

UTEST(cacert, active_is_the_embedded_bundle_until_one_is_published)
{
    hl_ca_bundle_reset_active();
    const unsigned char *a = NULL, *e = NULL;
    size_t alen = 0, elen = 0;
    int arc = hl_ca_bundle_active(&a, &alen);
    int erc = hl_embedded_ca_bundle(&e, &elen);
    ASSERT_EQ(arc, erc);
    ASSERT_TRUE(a == e);
    ASSERT_EQ(alen, elen);
    ASSERT_EQ(hl_ca_bundle_active(NULL, &alen), -1);
}

UTEST(cacert, a_published_anchor_replaces_it)
{
    static const unsigned char pem[] = "-----BEGIN CERTIFICATE-----\n";
    hl_ca_bundle_set_active(pem, sizeof pem, 0);
    const unsigned char *a = NULL;
    size_t alen = 0;
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), 0);
    ASSERT_TRUE(a == pem);
    ASSERT_EQ(alen, sizeof pem);

    /* An owned anchor is freed on replacement (ASan would report a leak or
     * a double free otherwise). */
    unsigned char *owned = malloc(4);
    ASSERT_TRUE(owned != NULL);
    memcpy(owned, "abc", 4);
    hl_ca_bundle_set_active(owned, 4, 1);
    hl_ca_bundle_set_active(pem, sizeof pem, 0);
    hl_ca_bundle_reset_active();
}

UTEST(cacert, no_usable_anchor_fails_closed)
{
    /* A named bundle that failed: verification must not quietly fall back
     * to the embedded one. */
    hl_ca_bundle_set_active(NULL, 0, 0);
    const unsigned char *a = (const unsigned char *)1;
    size_t alen = 7;
    ASSERT_EQ(hl_ca_bundle_active(&a, &alen), -1);
    ASSERT_TRUE(a == NULL);
    ASSERT_EQ(alen, (size_t)0);
    hl_ca_bundle_reset_active();
}


UTEST_MAIN()
