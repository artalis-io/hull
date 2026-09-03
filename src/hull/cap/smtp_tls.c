/*
 * smtp_tls.c - per-worker-thread client TLS context cache, keyed by trust owner.
 * See include/hull/cap/smtp_tls.h and docs/smtp_keel_slice2c_plan.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_tls.h"
#include "hull/tls_transport.h"   /* hl_tls_client_ctx_create_from_buf, hl_tls_ctx_destroy */

#include <keel/allocator.h>       /* KlAllocator */

#include <pthread.h>
#include <stdlib.h>

/* Create/destroy seam. In production these call the real from-buffer creator and
 * destroyer directly (no writable function pointers in .data). Under
 * HL_SMTP_TEST_HOOKS a test may substitute fakes to exercise the cache keying,
 * lazy creation, destruction, and failure paths without live mbedTLS. */
#ifdef HL_SMTP_TEST_HOOKS
static void *(*smtp_tls_test_create)(const unsigned char *, size_t, void *) = 0;
static void  (*smtp_tls_test_destroy)(void *) = 0;
static void *tls_make(const HlSmtpTrust *t)
{
    if (smtp_tls_test_create)
        return smtp_tls_test_create(t->ca_buf, t->ca_len, t->alloc);
    return hl_tls_client_ctx_create_from_buf(t->ca_buf, t->ca_len,
                                             (KlAllocator *)t->alloc);
}
static void tls_kill(void *ctx)
{
    if (smtp_tls_test_destroy) { smtp_tls_test_destroy(ctx); return; }
    hl_tls_ctx_destroy((KlTlsCtx *)ctx);
}
#else
static void *tls_make(const HlSmtpTrust *t)
{
    return hl_tls_client_ctx_create_from_buf(t->ca_buf, t->ca_len,
                                             (KlAllocator *)t->alloc);
}
static void tls_kill(void *ctx) { hl_tls_ctx_destroy((KlTlsCtx *)ctx); }
#endif

/* Per-thread cache: a short list keyed by the trust-descriptor identity. One
 * entry for the common single-server case; a worker that serves two owners grows
 * it by one node each (mirrors worker_db's per-thread keyed connection list). */
typedef struct TlsNode {
    const HlSmtpTrust *trust;   /* key: descriptor identity */
    void              *ctx;     /* derived per-worker KlTlsCtx */
    struct TlsNode    *next;
} TlsNode;

static pthread_key_t  s_key;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

/* pthread-key destructor: runs on thread exit (pool teardown). Destroys every
 * cached context for this thread. The CA buffer + allocator each ctx was built
 * from must still be valid here (the server keeps them alive through pool drain). */
static void tls_thread_destructor(void *p)
{
    TlsNode *n = (TlsNode *)p;
    while (n) {
        TlsNode *next = n->next;
        tls_kill(n->ctx);
        free(n);
        n = next;
    }
}

static void tls_key_create(void) { pthread_key_create(&s_key, tls_thread_destructor); }

void *hl_smtp_tls_ctx_for(const HlSmtpTrust *trust)
{
    if (!trust)
        return NULL;
    pthread_once(&s_once, tls_key_create);

    TlsNode *head = (TlsNode *)pthread_getspecific(s_key);
    for (TlsNode *n = head; n; n = n->next)
        if (n->trust == trust)
            return n->ctx;   /* lazily created earlier, on this thread, for this owner */

    /* Miss: create from the borrowed immutable CA buffer. */
    void *ctx = tls_make(trust);
    if (!ctx)
        return NULL;   /* creation failure: cache nothing (no partial context) */

    TlsNode *node = (TlsNode *)malloc(sizeof *node);
    if (!node) {
        tls_kill(ctx);   /* cannot cache; do not leak the context */
        return NULL;
    }
    node->trust = trust;
    node->ctx   = ctx;
    node->next  = head;
    pthread_setspecific(s_key, node);
    return ctx;
}
