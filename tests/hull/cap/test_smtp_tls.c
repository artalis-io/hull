/*
 * test_smtp_tls.c - per-worker-thread client TLS context cache.
 *
 * Exercises the cache keying / lifecycle with a fake ctx create+destroy (no live
 * mbedTLS), all from SPAWNED worker threads so the pthread-key destructor fires
 * on thread exit (as it does at pool teardown). Covers: lazy creation once per
 * (worker, trust); two workers -> distinct contexts; no reuse across trust
 * owners; destruction via the pthread-key destructor; CA buffer + allocator
 * valid through destruction; creation failure caches no partial context; the
 * context is built from the borrowed buffer (no filesystem read).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include (compiled with -DHL_SMTP_TEST_HOOKS) to substitute the
 * ctx create/destroy seam; cap_smtp_tls.o is excluded from this test's link. */
#include "../../../src/hull/cap/smtp_tls.c"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── fake ctx create/destroy + bookkeeping (mutex-protected: multi-threaded) ── */
#define MAXCTX 64
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int   g_create, g_destroy, g_fail_next;
static const unsigned char *g_last_ca;     /* ca_buf passed to the last create */
static uintptr_t g_seq;                    /* monotonic ctx-id, never reused */
static struct { void *ctx; const unsigned char *ca; unsigned char first; }
             g_map[MAXCTX];
static int   g_map_n;
static int   g_bad_destroy;                /* destroyed a ctx whose CA looked dead */

static void fx_reset(void)
{
    pthread_mutex_lock(&g_mu);
    g_create = g_destroy = g_fail_next = 0; g_last_ca = NULL; g_seq = 0;
    g_map_n = 0; g_bad_destroy = 0;
    memset(g_map, 0, sizeof g_map);
    pthread_mutex_unlock(&g_mu);
}

/* Fake ctx is a monotonic id cast to a pointer: distinct across the whole run
 * (never freed/reused, so pointer identity is a reliable distinctness signal). */
static void *fx_create(const unsigned char *ca, size_t len, void *alloc)
{
    (void)len; (void)alloc;
    pthread_mutex_lock(&g_mu);
    g_create++;
    g_last_ca = ca;
    if (g_fail_next) { pthread_mutex_unlock(&g_mu); return NULL; }
    void *ctx = (void *)(++g_seq);
    if (g_map_n < MAXCTX) {
        g_map[g_map_n].ctx = ctx;
        g_map[g_map_n].ca = ca;
        g_map[g_map_n].first = ca ? ca[0] : 0;   /* buffer readable at create */
        g_map_n++;
    }
    pthread_mutex_unlock(&g_mu);
    return ctx;
}

static void fx_destroy(void *ctx)
{
    pthread_mutex_lock(&g_mu);
    g_destroy++;
    /* The CA buffer this ctx was built from must still be valid + unchanged at
     * destruction (the server keeps it alive through pool drain). */
    for (int i = 0; i < g_map_n; i++) {
        if (g_map[i].ctx == ctx) {
            if (!g_map[i].ca || g_map[i].ca[0] != g_map[i].first)
                g_bad_destroy = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

static void install_fakes(void)
{
    smtp_tls_test_create  = fx_create;
    smtp_tls_test_destroy = fx_destroy;
}

/* Immutable server-owned CA material (kept alive for the whole test). */
static const unsigned char CA1[] = "CATRUST-ONE-pem-bytes";
static const unsigned char CA2[] = "DATRUST-TWO-pem-bytes";
static int g_alloc1, g_alloc2;   /* stand-ins for KlAllocator* identities */
static HlSmtpTrust T1 = { CA1, sizeof CA1, &g_alloc1 };
static HlSmtpTrust T2 = { CA2, sizeof CA2, &g_alloc2 };

/* ── worker-thread scripts ──────────────────────────────────────────── */
typedef struct { void *r0, *r1; int same01; } TResult;

/* get T1 twice: proves lazy-once-per-(thread,trust). */
static void *th_get_t1_twice(void *arg)
{
    TResult *o = (TResult *)arg;
    o->r0 = hl_smtp_tls_ctx_for(&T1);
    o->r1 = hl_smtp_tls_ctx_for(&T1);
    o->same01 = (o->r0 == o->r1);
    return NULL;   /* thread exit -> pthread-key destructor */
}
/* get T1 once (for the two-workers test). */
static void *th_get_t1(void *arg) { *(void **)arg = hl_smtp_tls_ctx_for(&T1); return NULL; }
/* get T1 then T2: proves no reuse across trust owners on one thread. */
static void *th_get_t1_t2(void *arg)
{
    TResult *o = (TResult *)arg;
    o->r0 = hl_smtp_tls_ctx_for(&T1);
    o->r1 = hl_smtp_tls_ctx_for(&T2);
    return NULL;
}
/* fail the first create, then succeed: proves no partial cache on failure. */
static void *th_fail_then_ok(void *arg)
{
    TResult *o = (TResult *)arg;
    pthread_mutex_lock(&g_mu); g_fail_next = 1; pthread_mutex_unlock(&g_mu);
    o->r0 = hl_smtp_tls_ctx_for(&T1);     /* fails -> NULL, caches nothing */
    pthread_mutex_lock(&g_mu); g_fail_next = 0; pthread_mutex_unlock(&g_mu);
    o->r1 = hl_smtp_tls_ctx_for(&T1);     /* retries -> fresh create */
    return NULL;
}

static void run(void *(*fn)(void *), void *arg)
{
    pthread_t t; pthread_create(&t, NULL, fn, arg); pthread_join(t, NULL);
}

/* ── tests ──────────────────────────────────────────────────────────── */

UTEST(smtp_tls, lazy_once_per_worker_and_from_buffer)
{
    fx_reset(); install_fakes();
    TResult r; memset(&r, 0, sizeof r);
    run(th_get_t1_twice, &r);

    ASSERT_EQ(g_create, 1);              /* created once, then cached */
    ASSERT_EQ(r.same01, 1);              /* same context both calls */
    ASSERT_TRUE(g_last_ca == CA1);       /* built from the borrowed buffer (no path) */
    ASSERT_EQ(g_destroy, 1);             /* destroyed by the pthread-key destructor */
    ASSERT_EQ(g_bad_destroy, 0);         /* CA buffer valid at destruction */
}

UTEST(smtp_tls, two_workers_get_distinct_contexts)
{
    fx_reset(); install_fakes();
    void *c1 = NULL, *c2 = NULL;
    pthread_t t1, t2;
    pthread_create(&t1, NULL, th_get_t1, &c1);
    pthread_create(&t2, NULL, th_get_t1, &c2);
    pthread_join(t1, NULL); pthread_join(t2, NULL);

    ASSERT_EQ(g_create, 2);              /* one per worker thread */
    ASSERT_TRUE(c1 != NULL && c2 != NULL);
    ASSERT_TRUE(c1 != c2);               /* distinct per worker */
    ASSERT_EQ(g_destroy, 2);             /* each destroyed at its thread exit */
    ASSERT_EQ(g_bad_destroy, 0);
}

UTEST(smtp_tls, no_reuse_across_trust_owners)
{
    fx_reset(); install_fakes();
    TResult r; memset(&r, 0, sizeof r);
    run(th_get_t1_t2, &r);

    ASSERT_EQ(g_create, 2);              /* one per distinct trust owner */
    ASSERT_TRUE(r.r0 != NULL && r.r1 != NULL);
    ASSERT_TRUE(r.r0 != r.r1);           /* T1 and T2 never share a context */
    ASSERT_EQ(g_destroy, 2);
    ASSERT_EQ(g_bad_destroy, 0);
}

UTEST(smtp_tls, creation_failure_caches_no_partial)
{
    fx_reset(); install_fakes();
    TResult r; memset(&r, 0, sizeof r);
    run(th_fail_then_ok, &r);

    ASSERT_TRUE(r.r0 == NULL);           /* failure returns NULL */
    ASSERT_TRUE(r.r1 != NULL);           /* retry succeeds */
    ASSERT_EQ(g_create, 2);              /* failure did not cache -> real re-create */
    ASSERT_EQ(g_destroy, 1);             /* only the successful ctx destroyed */
    ASSERT_EQ(g_bad_destroy, 0);
}

UTEST_MAIN();
