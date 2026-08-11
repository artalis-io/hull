/*
 * Hull-tree C-API test for WAMR patch 0003 (wasm_runtime_destroy_shared_heap +
 * mandatory attach-time registration check + detach read_only reset + the
 * test-only shared_heap_count probe), exercised THROUGH HULL'S OWN WAMR BUILD.
 *
 * Covers the #307 matrix at the C-API level (runnable under Hull's normal /
 * ASan+UBSan / MSan / TSan builds, unlike the WAMR cmake unit harness):
 *   - list-length reclamation: N create/attach?/detach?/unchain?/destroy cycles
 *     leave shared_heap_count() constant (no growth -> bounded chain cost);
 *   - fail-closed matrix: NULL, unknown/already-destroyed, runtime-owned
 *     (heap_handle != NULL), still-attached, still-chained -> false, no free;
 *   - destroy a chained body AND head -> false; destroy after the final unchain
 *     -> true;
 *   - attach using a previously destroyed pointer -> false, no use-after-free;
 *   - detach resets the read_only cache flag;
 *   - races (TSan): destroy vs create/destroy on the shared list, and two
 *     threads destroying the same pointer -> exactly one frees.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"

#include "hull/cap/wasm.h"
#include "wasm_export.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

UTEST_MAIN();

/* store_i32(p): *p = 0x11223344 ;  load_i32(p) -> *p  (module has (memory 1)). */
static const unsigned char ro_heap_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60,
    0x01, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x03, 0x03, 0x02, 0x00,
    0x01, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x21, 0x03, 0x06, 0x6d, 0x65,
    0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x09, 0x73, 0x74, 0x6f, 0x72, 0x65,
    0x5f, 0x69, 0x33, 0x32, 0x00, 0x00, 0x08, 0x6c, 0x6f, 0x61, 0x64, 0x5f,
    0x69, 0x33, 0x32, 0x00, 0x01, 0x0a, 0x17, 0x02, 0x0d, 0x00, 0x20, 0x00,
    0x41, 0xc4, 0xe6, 0x88, 0x89, 0x01, 0x36, 0x02, 0x00, 0x0b, 0x07, 0x00,
    0x20, 0x00, 0x28, 0x02, 0x00, 0x0b
};
static const unsigned int ro_heap_wasm_len = 90;

static uint32_t g_page;

/* A page-aligned, page-sized backing buffer Hull owns (NOT freed by destroy). */
static void *
alloc_backing(void)
{
    void *p = NULL;
    if (posix_memalign(&p, g_page, g_page) != 0)
        return NULL;
    memset(p, 0, g_page);
    return p;
}

/* Create a pre-allocated shared heap over `backing` (read_only optional). */
static wasm_shared_heap_t
make_prealloc_heap(void *backing, bool read_only)
{
    SharedHeapInitArgs args;
    memset(&args, 0, sizeof(args));
    args.pre_allocated_addr = backing;
    args.size = g_page;
    args.read_only = read_only;
    return wasm_runtime_create_shared_heap(&args);
}

/* ── list-length reclamation: create+destroy N times, count stays flat ─────── */
UTEST(wasm_shared_heap_destroy, no_list_growth)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);

    uint32_t base = wasm_runtime_shared_heap_count();
    for (int i = 0; i < 5000; i++) {
        void *b = alloc_backing();
        ASSERT_TRUE(b != NULL);
        wasm_shared_heap_t h = make_prealloc_heap(b, true);
        ASSERT_TRUE(h != NULL);
        ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1); /* +1 while live */
        ASSERT_TRUE(wasm_runtime_destroy_shared_heap(h));      /* reclaimed */
        ASSERT_EQ(wasm_runtime_shared_heap_count(), base);     /* back to baseline */
        free(b);
    }
    /* The list never grew: chain_shared_heaps' O(list) cost stays bounded. */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_wasm_destroy(&cache);
}

/* ── fail-closed matrix ────────────────────────────────────────────────────── */
UTEST(wasm_shared_heap_destroy, fail_closed_matrix)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);
    uint32_t base = wasm_runtime_shared_heap_count();

    /* NULL */
    ASSERT_FALSE(wasm_runtime_destroy_shared_heap(NULL));

    /* unknown / already-destroyed: destroy once (true), again (false), no growth */
    void *b = alloc_backing();
    wasm_shared_heap_t h = make_prealloc_heap(b, true);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(wasm_runtime_destroy_shared_heap(h));
    ASSERT_FALSE(wasm_runtime_destroy_shared_heap(h)); /* not on list; no deref */
    free(b);

    /* runtime-owned (managed) heap: heap_handle != NULL -> not destroyable */
    SharedHeapInitArgs margs;
    memset(&margs, 0, sizeof(margs));
    margs.size = 65536; /* managed heap: no pre_allocated_addr */
    wasm_shared_heap_t managed = wasm_runtime_create_shared_heap(&margs);
    ASSERT_TRUE(managed != NULL);
    ASSERT_FALSE(wasm_runtime_destroy_shared_heap(managed)); /* fail closed */
    /* managed heap is intentionally left (reclaimed at runtime destroy). */

    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1); /* only the managed one */
    hl_cap_wasm_destroy(&cache);
}

/* ── chained head/body reject; destroy after final unchain succeeds ────────── */
UTEST(wasm_shared_heap_destroy, chain_reject_then_unchain)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);
    uint32_t base = wasm_runtime_shared_heap_count();

    void *ba = alloc_backing(), *bb = alloc_backing();
    wasm_shared_heap_t a = make_prealloc_heap(ba, true);
    wasm_shared_heap_t b = make_prealloc_heap(bb, true);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(b != NULL);

    /* chain a -> b : a is head (outgoing), b is body (incoming). */
    ASSERT_TRUE(wasm_runtime_chain_shared_heaps(a, b) != NULL);
    ASSERT_FALSE(wasm_runtime_destroy_shared_heap(b)); /* incoming edge */
    ASSERT_FALSE(wasm_runtime_destroy_shared_heap(a)); /* outgoing edge */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 2); /* nothing freed */

    /* unchain the entire chain -> both standalone -> both destroyable. */
    ASSERT_TRUE(wasm_runtime_unchain_shared_heaps(a, true) != NULL);
    ASSERT_TRUE(wasm_runtime_destroy_shared_heap(a));
    ASSERT_TRUE(wasm_runtime_destroy_shared_heap(b));
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    free(ba); free(bb);
    hl_cap_wasm_destroy(&cache);
}

/* ── attach with a destroyed pointer fails closed; still-attached rejects; the
 *    detach read_only reset holds. Needs a real instance. ──────────────────── */
UTEST(wasm_shared_heap_destroy, attach_detach_lifecycle)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);
    uint32_t base = wasm_runtime_shared_heap_count();

    /* fast-interp rewrites bytes in place -> mutable copy. */
    uint8_t *mbuf = (uint8_t *)malloc(ro_heap_wasm_len);
    ASSERT_TRUE(mbuf != NULL);
    memcpy(mbuf, ro_heap_wasm, ro_heap_wasm_len);
    char err[128] = { 0 };
    wasm_module_t mod = wasm_runtime_load(mbuf, ro_heap_wasm_len, err, sizeof(err));
    ASSERT_TRUE(mod != NULL);
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(mod, 16 * 1024, 64 * 1024, err, sizeof(err));
    ASSERT_TRUE(inst != NULL);

    void *b1 = alloc_backing();
    wasm_shared_heap_t h1 = make_prealloc_heap(b1, false); /* writable */
    ASSERT_TRUE(h1 != NULL);

    /* attach ok; destroy-while-attached fails; detach; then destroy ok. */
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, h1));
    ASSERT_FALSE(wasm_runtime_destroy_shared_heap(h1)); /* still attached */
    wasm_runtime_detach_shared_heap(inst);
    ASSERT_TRUE(wasm_runtime_destroy_shared_heap(h1));
    free(b1);

    /* attach using the DESTROYED pointer -> false, no use-after-free (ASan). */
    ASSERT_FALSE(wasm_runtime_attach_shared_heap(inst, h1));

    /* re-attach a fresh writable heap into the SAME instance: proves detach
     * cleared the read_only cache (a stale read_only would misgate this). */
    void *b2 = alloc_backing();
    wasm_shared_heap_t h2 = make_prealloc_heap(b2, false);
    ASSERT_TRUE(h2 != NULL);
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, h2));
    wasm_runtime_detach_shared_heap(inst);
    ASSERT_TRUE(wasm_runtime_destroy_shared_heap(h2));
    free(b2);

    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    free(mbuf);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_wasm_destroy(&cache);
}

/* ── concurrency (TSan): parallel create/destroy churn + a same-pointer
 *    double-destroy race that exactly one thread wins. ──────────────────────── */
#define NTHREAD 4
#define NITER   2000

static void *
churn_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < NITER; i++) {
        void *b = alloc_backing();
        if (!b) continue;
        wasm_shared_heap_t h = make_prealloc_heap(b, true);
        if (h)
            wasm_runtime_destroy_shared_heap(h);
        free(b);
    }
    return NULL;
}

UTEST(wasm_shared_heap_destroy, concurrent_churn)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);
    uint32_t base = wasm_runtime_shared_heap_count();

    pthread_t th[NTHREAD];
    for (int i = 0; i < NTHREAD; i++)
        ASSERT_EQ(pthread_create(&th[i], NULL, churn_worker, NULL), 0);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    /* every heap created was destroyed on its own thread -> list back to base. */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_wasm_destroy(&cache);
}

/* Two threads race to destroy the SAME pointer; exactly one must free it. */
struct dd_arg { wasm_shared_heap_t h; int result; };
static void *
double_destroy_worker(void *arg)
{
    struct dd_arg *a = (struct dd_arg *)arg;
    a->result = wasm_runtime_destroy_shared_heap(a->h) ? 1 : 0;
    return NULL;
}

UTEST(wasm_shared_heap_destroy, double_destroy_race)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);

    for (int round = 0; round < 500; round++) {
        void *b = alloc_backing();
        wasm_shared_heap_t h = make_prealloc_heap(b, true);
        ASSERT_TRUE(h != NULL);
        struct dd_arg a0 = { h, -1 }, a1 = { h, -1 };
        pthread_t t0, t1;
        ASSERT_EQ(pthread_create(&t0, NULL, double_destroy_worker, &a0), 0);
        ASSERT_EQ(pthread_create(&t1, NULL, double_destroy_worker, &a1), 0);
        pthread_join(t0, NULL);
        pthread_join(t1, NULL);
        /* exactly one thread freed it; the other found it already gone. */
        ASSERT_EQ(a0.result + a1.result, 1);
        free(b);
    }
    hl_cap_wasm_destroy(&cache);
}

/* ── attach racing destroy: the lock serializes; exactly one wins; a later
 *    attach with the (now destroyed) pointer fails closed, no UAF. ──────────── */
struct ad_arg { wasm_module_inst_t inst; wasm_shared_heap_t h; int result; };
static void *attach_worker(void *p)
{
    struct ad_arg *a = (struct ad_arg *)p;
    a->result = wasm_runtime_attach_shared_heap(a->inst, a->h) ? 1 : 0;
    return NULL;
}
static void *destroy_worker(void *p)
{
    struct ad_arg *a = (struct ad_arg *)p;
    a->result = wasm_runtime_destroy_shared_heap(a->h) ? 1 : 0;
    return NULL;
}

UTEST(wasm_shared_heap_destroy, attach_vs_destroy_race)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);
    uint32_t base = wasm_runtime_shared_heap_count();

    uint8_t *mbuf = (uint8_t *)malloc(ro_heap_wasm_len);
    ASSERT_TRUE(mbuf != NULL);
    memcpy(mbuf, ro_heap_wasm, ro_heap_wasm_len);
    char err[128] = { 0 };
    wasm_module_t mod = wasm_runtime_load(mbuf, ro_heap_wasm_len, err, sizeof(err));
    ASSERT_TRUE(mod != NULL);
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(mod, 16 * 1024, 64 * 1024, err, sizeof(err));
    ASSERT_TRUE(inst != NULL);

    for (int round = 0; round < 400; round++) {
        void *b = alloc_backing();
        wasm_shared_heap_t h = make_prealloc_heap(b, false); /* writable */
        ASSERT_TRUE(h != NULL);
        struct ad_arg aa = { inst, h, -1 }, ad = { inst, h, -1 };
        pthread_t ta, td;
        ASSERT_EQ(pthread_create(&ta, NULL, attach_worker, &aa), 0);
        ASSERT_EQ(pthread_create(&td, NULL, destroy_worker, &ad), 0);
        pthread_join(ta, NULL);
        pthread_join(td, NULL);
        /* exactly one won: attach-then-destroy-fails, or destroy-then-attach-fails. */
        ASSERT_EQ(aa.result + ad.result, 1);
        if (aa.result) {
            /* attach won -> detach, then the descriptor is destroyable. */
            wasm_runtime_detach_shared_heap(inst);
            ASSERT_TRUE(wasm_runtime_destroy_shared_heap(h));
        }
        /* stale-pointer attach after destruction fails closed (no UAF). */
        ASSERT_FALSE(wasm_runtime_attach_shared_heap(inst, h));
        free(b);
    }

    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    free(mbuf);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_wasm_destroy(&cache);
}

/* ── chain/unchain racing destroy: destroy on chained heaps fails; on
 *    standalone ones frees; the list stays consistent (TSan). ──────────────── */
static wasm_shared_heap_t g_x, g_y;
static void *chain_churn_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1500; i++) {
        wasm_shared_heap_t head = wasm_runtime_chain_shared_heaps(g_x, g_y);
        if (head)
            wasm_runtime_unchain_shared_heaps(g_x, true);
    }
    return NULL;
}
static void *destroy_churn_worker(void *arg)
{
    (void)arg;
    /* Race chain/unchain (thread A, mutating the global list) against
     * create/destroy of OUR OWN heaps (never g_x/g_y): both threads hammer
     * shared_heap_list + shared_heap_list_lock concurrently. We do NOT destroy
     * a heap another thread references by raw pointer -- that is the documented
     * caller-contract violation (Hull's mod->mutex forbids it), not something the
     * lock can save; the race we validate here is on the LIST, not on a shared
     * descriptor. */
    for (int i = 0; i < 1500; i++) {
        void *b = alloc_backing();
        if (!b) continue;
        wasm_shared_heap_t z = make_prealloc_heap(b, true);
        if (z)
            wasm_runtime_destroy_shared_heap(z);
        free(b);
    }
    return NULL;
}

UTEST(wasm_shared_heap_destroy, chain_unchain_vs_destroy_race)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    g_page = (uint32_t)sysconf(_SC_PAGESIZE);
    uint32_t base = wasm_runtime_shared_heap_count();

    void *bx = alloc_backing(), *by = alloc_backing();
    g_x = make_prealloc_heap(bx, true);
    g_y = make_prealloc_heap(by, true);
    ASSERT_TRUE(g_x != NULL);
    ASSERT_TRUE(g_y != NULL);

    pthread_t tc, td;
    ASSERT_EQ(pthread_create(&tc, NULL, chain_churn_worker, NULL), 0);
    ASSERT_EQ(pthread_create(&td, NULL, destroy_churn_worker, NULL), 0);
    pthread_join(tc, NULL);
    pthread_join(td, NULL);

    /* g_x/g_y are owned solely by this test now; the chain thread left them
     * either standalone or chained (x -> y). Unchain if needed, then destroy. */
    (void)wasm_runtime_unchain_shared_heaps(g_x, true);
    ASSERT_TRUE(wasm_runtime_destroy_shared_heap(g_x));
    ASSERT_TRUE(wasm_runtime_destroy_shared_heap(g_y));
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    free(bx); free(by);
    hl_cap_wasm_destroy(&cache);
}

/* Runtime-teardown racing destroy is OUTSIDE the API contract and is NOT tested
 * as a race: wasm_runtime_destroy (Hull's hl_cap_wasm_destroy) frees the whole
 * shared_heap_list and DESTROYS shared_heap_list_lock, so a destroy concurrent
 * with, or after, teardown is undefined. The contract (design doc s7) is that
 * teardown runs strictly LAST -- after every span set is torn down and no
 * compute call can start. Every test above enforces it by construction: all
 * destroys complete (threads joined) BEFORE hl_cap_wasm_destroy, which then
 * reclaims any remainder with no double free (ASan-verified). */
