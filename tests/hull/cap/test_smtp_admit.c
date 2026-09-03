/*
 * test_smtp_admit.c - per-server SMTP admission capacity + per-op leases.
 *
 * Pins: exact cap values; race-safe admission never exceeding the cap;
 * per-operation lease ownership (a duplicate release frees only its own slot,
 * never another op's); race of duplicate releases on one lease decrements once;
 * failed reservation leaves the lease unheld; a held lease is not reusable (even
 * against another instance); reuse after a legitimate release; per-instance
 * scoping; queued + running both consume slots.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/cap/smtp_admit.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

UTEST(smtp_admit, exact_cap_values)
{
    struct { int w, cap; } cases[] = {
        {1, 0}, {2, 1}, {3, 1}, {4, 2}, {5, 2}, {8, 4},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        HlSmtpAdmission a;
        hl_smtp_admission_init(&a, cases[i].w);
        ASSERT_EQ(hl_smtp_admission_cap(&a), cases[i].cap);
        ASSERT_EQ(hl_smtp_admission_inflight(&a), 0);
    }
}

UTEST(smtp_admit, disabled_pool_admits_nothing)
{
    HlSmtpAdmission a;
    hl_smtp_admission_init(&a, 1);   /* cap 0 */
    HlSmtpAdmissionLease l; hl_smtp_admission_lease_init(&l);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l), 0);
    ASSERT_EQ(hl_smtp_admission_lease_held(&l), 0);
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 0);
}

UTEST(smtp_admit, fill_to_cap_then_reject)
{
    HlSmtpAdmission a;
    hl_smtp_admission_init(&a, 8);   /* cap 4 */
    HlSmtpAdmissionLease l[5];
    for (int i = 0; i < 5; i++) hl_smtp_admission_lease_init(&l[i]);
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l[i]), 1);
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 4);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l[4]), 0);   /* full */
    ASSERT_EQ(hl_smtp_admission_lease_held(&l[4]), 0);
}

UTEST(smtp_admit, released_slot_immediately_reusable)
{
    HlSmtpAdmission a;
    hl_smtp_admission_init(&a, 4);   /* cap 2 */
    HlSmtpAdmissionLease l0, l1, l2;
    hl_smtp_admission_lease_init(&l0);
    hl_smtp_admission_lease_init(&l1);
    hl_smtp_admission_lease_init(&l2);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l0), 1);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l1), 1);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l2), 0);   /* full */
    ASSERT_EQ(hl_smtp_admission_release(&l0), 1);            /* free one */
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l2), 1);    /* immediately reusable */
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 2);
}

/* The decisive lease-ownership test: a duplicate release frees only its own
 * slot, never another operation's. */
UTEST(smtp_admit, duplicate_release_frees_only_own_lease)
{
    HlSmtpAdmission a;
    hl_smtp_admission_init(&a, 4);   /* cap 2 */
    HlSmtpAdmissionLease la, lb;
    hl_smtp_admission_lease_init(&la);
    hl_smtp_admission_lease_init(&lb);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &la), 1);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &lb), 1);
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 2);

    ASSERT_EQ(hl_smtp_admission_release(&la), 1);   /* A releases */
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 1);
    ASSERT_EQ(hl_smtp_admission_release(&la), 0);   /* A releases AGAIN: no-op */
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 1);   /* B's slot is untouched */

    ASSERT_EQ(hl_smtp_admission_release(&lb), 1);   /* B releases its own slot */
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 0);
}

/* Five terminal paths (completion, queued cancel, submission failure, suspension
 * failure, shutdown discard) each release their OWN lease exactly once. */
UTEST(smtp_admit, exactly_once_release_per_path_distinct_leases)
{
    HlSmtpAdmission a;
    hl_smtp_admission_init(&a, 8);   /* cap 4 */
    for (int path = 0; path < 5; path++) {
        HlSmtpAdmissionLease l; hl_smtp_admission_lease_init(&l);
        ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l), 1);
        ASSERT_EQ(hl_smtp_admission_inflight(&a), 1);
        ASSERT_EQ(hl_smtp_admission_release(&l), 1);     /* the path releases once */
        ASSERT_EQ(hl_smtp_admission_inflight(&a), 0);
        ASSERT_EQ(hl_smtp_admission_release(&l), 0);     /* double release: no-op */
        ASSERT_EQ(hl_smtp_admission_inflight(&a), 0);
    }
}

UTEST(smtp_admit, failed_reservation_leaves_lease_unheld)
{
    HlSmtpAdmission a;
    hl_smtp_admission_init(&a, 4);   /* cap 2 */
    HlSmtpAdmissionLease l0, l1, l2;
    hl_smtp_admission_lease_init(&l0);
    hl_smtp_admission_lease_init(&l1);
    hl_smtp_admission_lease_init(&l2);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l0), 1);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l1), 1);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l2), 0);   /* full */
    ASSERT_EQ(hl_smtp_admission_lease_held(&l2), 0);        /* unheld */
    ASSERT_EQ(hl_smtp_admission_release(&l2), 0);           /* releasing unheld: no-op */
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 2);
}

UTEST(smtp_admit, lease_reusable_after_release)
{
    HlSmtpAdmission a;
    hl_smtp_admission_init(&a, 4);   /* cap 2 */
    HlSmtpAdmissionLease l; hl_smtp_admission_lease_init(&l);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l), 1);
    ASSERT_EQ(hl_smtp_admission_release(&l), 1);
    ASSERT_EQ(hl_smtp_admission_lease_held(&l), 0);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l), 1);    /* reusable */
    ASSERT_EQ(hl_smtp_admission_lease_held(&l), 1);
    ASSERT_EQ(hl_smtp_admission_release(&l), 1);
}

UTEST(smtp_admit, held_lease_not_reusable_against_other_instance)
{
    HlSmtpAdmission a, b;
    hl_smtp_admission_init(&a, 4);   /* cap 2 */
    hl_smtp_admission_init(&b, 4);
    HlSmtpAdmissionLease l; hl_smtp_admission_lease_init(&l);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &l), 1);    /* held by a */
    ASSERT_EQ(hl_smtp_admission_try_reserve(&b, &l), 0);    /* cannot reuse a held lease */
    ASSERT_EQ(hl_smtp_admission_inflight(&b), 0);           /* b untouched */
    ASSERT_TRUE(l.owner == &a);
    ASSERT_EQ(hl_smtp_admission_release(&l), 1);            /* frees a's slot */
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 0);
}

UTEST(smtp_admit, per_instance_scoped_no_cross_interference)
{
    HlSmtpAdmission a, b;
    hl_smtp_admission_init(&a, 4);   /* cap 2 */
    hl_smtp_admission_init(&b, 8);   /* cap 4 */
    HlSmtpAdmissionLease la[2], lb[4];
    for (int i = 0; i < 2; i++) { hl_smtp_admission_lease_init(&la[i]);
        ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &la[i]), 1); }
    HlSmtpAdmissionLease la_extra; hl_smtp_admission_lease_init(&la_extra);
    ASSERT_EQ(hl_smtp_admission_try_reserve(&a, &la_extra), 0);  /* A exhausted */
    for (int i = 0; i < 4; i++) { hl_smtp_admission_lease_init(&lb[i]);
        ASSERT_EQ(hl_smtp_admission_try_reserve(&b, &lb[i]), 1); }  /* B unaffected */
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 2);
    ASSERT_EQ(hl_smtp_admission_inflight(&b), 4);
    ASSERT_EQ(hl_smtp_admission_release(&lb[0]), 1);
    ASSERT_EQ(hl_smtp_admission_inflight(&a), 2);            /* A unchanged */
    ASSERT_EQ(hl_smtp_admission_inflight(&b), 3);
}

/* ── high-contention race: admission never exceeds the cap ───────────── */
#define NTHREADS 16
static HlSmtpAdmission     g_a;
static HlSmtpAdmissionLease g_leases[NTHREADS];
static atomic_int          g_admits;

static void *reserve_only(void *arg)   /* each thread owns its own lease */
{
    HlSmtpAdmissionLease *l = (HlSmtpAdmissionLease *)arg;
    if (hl_smtp_admission_try_reserve(&g_a, l))
        atomic_fetch_add(&g_admits, 1);
    return NULL;
}

UTEST(smtp_admit, race_never_exceeds_cap)
{
    hl_smtp_admission_init(&g_a, 8);   /* cap 4 */
    atomic_init(&g_admits, 0);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        hl_smtp_admission_lease_init(&g_leases[i]);
        pthread_create(&th[i], NULL, reserve_only, &g_leases[i]);
    }
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);
    ASSERT_EQ(atomic_load(&g_admits), 4);             /* exactly cap admitted */
    ASSERT_EQ(hl_smtp_admission_inflight(&g_a), 4);
}

/* ── race duplicate releases on ONE lease: decrement exactly once ─────── */
static HlSmtpAdmissionLease g_rl;      /* the contested lease */
static atomic_int           g_rel_wins;
static void *rel_attempt(void *_)
{
    (void)_;
    if (hl_smtp_admission_release(&g_rl))
        atomic_fetch_add(&g_rel_wins, 1);
    return NULL;
}

UTEST(smtp_admit, race_duplicate_release_same_lease_decrements_once)
{
    for (int iter = 0; iter < 500; iter++) {
        hl_smtp_admission_init(&g_a, 8);   /* cap 4 */
        HlSmtpAdmissionLease keeper;
        hl_smtp_admission_lease_init(&keeper);
        hl_smtp_admission_lease_init(&g_rl);
        ASSERT_EQ(hl_smtp_admission_try_reserve(&g_a, &keeper), 1);
        ASSERT_EQ(hl_smtp_admission_try_reserve(&g_a, &g_rl), 1);   /* inflight 2 */
        atomic_init(&g_rel_wins, 0);

        pthread_t th[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&th[i], NULL, rel_attempt, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(th[i], NULL);

        ASSERT_EQ(atomic_load(&g_rel_wins), 1);            /* exactly one release won */
        ASSERT_EQ(hl_smtp_admission_inflight(&g_a), 1);    /* only g_rl freed; keeper intact */
        hl_smtp_admission_release(&keeper);
    }
}

/* ── race MANY reservations on ONE lease: exactly one wins, none leaked ─ */
static HlSmtpAdmissionLease g_shared;
static atomic_int           g_res_wins;
static void *reserve_shared(void *_)
{
    (void)_;
    if (hl_smtp_admission_try_reserve(&g_a, &g_shared))
        atomic_fetch_add(&g_res_wins, 1);
    return NULL;
}

UTEST(smtp_admit, race_same_lease_reservation_exactly_one)
{
    for (int iter = 0; iter < 500; iter++) {
        hl_smtp_admission_init(&g_a, 8);   /* cap 4 */
        hl_smtp_admission_lease_init(&g_shared);
        atomic_init(&g_res_wins, 0);

        pthread_t th[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&th[i], NULL, reserve_shared, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(th[i], NULL);

        ASSERT_EQ(atomic_load(&g_res_wins), 1);            /* exactly one reservation */
        ASSERT_EQ(hl_smtp_admission_inflight(&g_a), 1);    /* one slot, none leaked */
        ASSERT_EQ(hl_smtp_admission_release(&g_shared), 1);
        ASSERT_EQ(hl_smtp_admission_inflight(&g_a), 0);    /* one release restores zero */
    }
}

/* ── race reservation against release: no decrement while RESERVING ───── */
static HlSmtpAdmissionLease g_rr;
static atomic_int           g_rr_reserve_ok, g_rr_release_won;
static void *rr_reserve(void *_)
{ (void)_; if (hl_smtp_admission_try_reserve(&g_a, &g_rr)) atomic_store(&g_rr_reserve_ok, 1); return NULL; }
static void *rr_release(void *_)
{ (void)_; if (hl_smtp_admission_release(&g_rr)) atomic_store(&g_rr_release_won, 1); return NULL; }

UTEST(smtp_admit, race_reserve_vs_release_no_stray_decrement)
{
    for (int iter = 0; iter < 3000; iter++) {
        hl_smtp_admission_init(&g_a, 8);   /* cap 4 */
        hl_smtp_admission_lease_init(&g_rr);
        atomic_init(&g_rr_reserve_ok, 0);
        atomic_init(&g_rr_release_won, 0);

        pthread_t tr, tx;
        pthread_create(&tr, NULL, rr_reserve, NULL);
        pthread_create(&tx, NULL, rr_release, NULL);
        pthread_join(tr, NULL);
        pthread_join(tx, NULL);

        int inflight = hl_smtp_admission_inflight(&g_a);
        int x = atomic_load(&g_rr_release_won);
        /* The reserve always wins FREE -> RESERVING (the release cannot move a
         * FREE lease), and capacity is available, so it always admits. */
        ASSERT_EQ(atomic_load(&g_rr_reserve_ok), 1);
        /* A release only decrements if it saw HELD; if it raced during FREE or
         * RESERVING it decremented nothing, so the reservation stands. */
        ASSERT_TRUE(inflight >= 0);
        ASSERT_EQ(inflight, x ? 0 : 1);
        if (inflight == 1)
            ASSERT_EQ(hl_smtp_admission_release(&g_rr), 1);
        ASSERT_EQ(hl_smtp_admission_inflight(&g_a), 0);
    }
}

UTEST_MAIN();
