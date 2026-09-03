/*
 * smtp_admit.c - per-server SMTP admission capacity with per-operation leases.
 * See include/hull/cap/smtp_admit.h and docs/smtp_keel_slice2c_plan.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_admit.h"

#include <stddef.h>   /* NULL */

static int cap_for(int workers)
{
    if (workers <= 1)
        return 0;                 /* async SMTP admission disabled */
    int half = workers / 2;       /* floor */
    return half < 1 ? 1 : half;   /* max(1, floor(W / 2)) */
}

void hl_smtp_admission_init(HlSmtpAdmission *a, int workers)
{
    a->cap = cap_for(workers);
    atomic_init(&a->inflight, 0);
}

int hl_smtp_admission_cap(const HlSmtpAdmission *a)
{
    return a->cap;
}

int hl_smtp_admission_inflight(const HlSmtpAdmission *a)
{
    return atomic_load_explicit(&a->inflight, memory_order_acquire);
}

void hl_smtp_admission_lease_init(HlSmtpAdmissionLease *lease)
{
    lease->owner = NULL;
    atomic_init(&lease->state, HL_SMTP_LEASE_FREE);
}

int hl_smtp_admission_lease_held(const HlSmtpAdmissionLease *lease)
{
    return atomic_load_explicit(&lease->state, memory_order_acquire)
           == HL_SMTP_LEASE_HELD ? 1 : 0;
}

int hl_smtp_admission_try_reserve(HlSmtpAdmission *a, HlSmtpAdmissionLease *lease)
{
    /* Claim the lease exclusively for reserving: only the FREE -> RESERVING
     * winner proceeds to touch capacity, so two concurrent reservations on one
     * lease cannot both increment. A non-FREE lease (RESERVING or HELD) is
     * rejected without touching capacity. */
    int expected = HL_SMTP_LEASE_FREE;
    if (!atomic_compare_exchange_strong_explicit(
            &lease->state, &expected, HL_SMTP_LEASE_RESERVING,
            memory_order_acq_rel, memory_order_acquire))
        return 0;

    /* Reserve capacity (cap == 0 rejects always: cur >= 0). */
    int cur = atomic_load_explicit(&a->inflight, memory_order_relaxed);
    for (;;) {
        if (cur >= a->cap) {
            /* Capacity unavailable: restore the lease, no capacity change. */
            atomic_store_explicit(&lease->state, HL_SMTP_LEASE_FREE,
                                  memory_order_release);
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &a->inflight, &cur, cur + 1,
                memory_order_acq_rel, memory_order_relaxed))
            break;
    }

    /* Bind the owner BEFORE publishing HELD, so a release that observes HELD
     * (acquire) sees the owner it must decrement. */
    lease->owner = a;
    atomic_store_explicit(&lease->state, HL_SMTP_LEASE_HELD, memory_order_release);
    return 1;
}

int hl_smtp_admission_release(HlSmtpAdmissionLease *lease)
{
    /* Only the HELD -> FREE winner decrements. A release seen while the lease is
     * FREE, mid-RESERVING, or already released fails here and decrements nothing
     * (no decrement can occur while a reservation is still in RESERVING). */
    int expected = HL_SMTP_LEASE_HELD;
    if (!atomic_compare_exchange_strong_explicit(
            &lease->state, &expected, HL_SMTP_LEASE_FREE,
            memory_order_acq_rel, memory_order_acquire))
        return 0;

    atomic_fetch_sub_explicit(&lease->owner->inflight, 1, memory_order_acq_rel);
    return 1;
}
