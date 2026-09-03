/**
 * @file cap/smtp_admit.h
 * @brief Per-server SMTP admission capacity with per-operation leases.
 *
 * Bounds the number of concurrent SMTP worker jobs on the shared async pool so a
 * burst of slow SMTP sends cannot starve db / compute work. A slot is held from
 * queue-time reservation through the terminal release, so queued AND running
 * jobs both consume a slot.
 *
 * Capacity is reserved/released through a per-operation #HlSmtpAdmissionLease
 * (may be embedded in HlSmtpOp). The lease's atomic `held` flag gives each op
 * ownership of exactly its own slot: a duplicate or racing release on a lease
 * decrements nothing, so it can never free a different operation's slot. The
 * primitive owns CAPACITY ONLY: it never references the operation, the
 * transport, or the runtime continuation. Scoped per server instance, so
 * instances do not interfere. See docs/smtp_keel_slice2c_plan.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_ADMIT_H
#define HL_CAP_SMTP_ADMIT_H

#include <stdatomic.h>
#include <stdbool.h>

typedef struct HlSmtpAdmission {
    int         cap;        /* max concurrent SMTP jobs; 0 = async admission off */
    _Atomic int inflight;   /* held leases (queued + running jobs) */
} HlSmtpAdmission;

/* Lease lifecycle. The three states make BOTH reservation and release exclusive:
 * only the FREE -> RESERVING winner may touch capacity, and only the HELD -> FREE
 * winner may decrement. So two concurrent reservations on one lease cannot both
 * increment, and a duplicate/racing release cannot double-decrement. */
enum {
    HL_SMTP_LEASE_FREE = 0,
    HL_SMTP_LEASE_RESERVING,
    HL_SMTP_LEASE_HELD,
};

typedef struct HlSmtpAdmissionLease {
    HlSmtpAdmission *owner;   /* borrowed; written by the RESERVING winner before HELD */
    _Atomic int      state;   /* HL_SMTP_LEASE_* */
} HlSmtpAdmissionLease;

/**
 * Compute the cap from the pool worker count @p workers and reset inflight to 0:
 *   W <= 1 -> 0  (async SMTP admission disabled; no false headroom claim)
 *   W >= 2 -> max(1, floor(W / 2))  (>= 1 worker always left for db / compute)
 */
void hl_smtp_admission_init(HlSmtpAdmission *a, int workers);

int  hl_smtp_admission_cap(const HlSmtpAdmission *a);
int  hl_smtp_admission_inflight(const HlSmtpAdmission *a);

/** Reset a lease to unheld / unowned. (A zero-initialized lease is also valid.) */
void hl_smtp_admission_lease_init(HlSmtpAdmissionLease *lease);

/** Whether @p lease currently holds a slot. */
int  hl_smtp_admission_lease_held(const HlSmtpAdmissionLease *lease);

/**
 * Reserve a slot into @p lease. CASes the lease FREE -> RESERVING (only that
 * winner touches capacity), reserves capacity, then publishes HELD. Returns 1 if
 * admitted (inflight incremented, lease bound to @p a), 0 if rejected: the pool
 * is full (RESERVING is restored to FREE, no capacity change) or the lease is
 * not FREE (a concurrent/duplicate reservation, or a held lease that cannot be
 * reused even against a different admission instance).
 */
int  hl_smtp_admission_try_reserve(HlSmtpAdmission *a, HlSmtpAdmissionLease *lease);

/**
 * Release the slot @p lease holds. Atomically CASes HELD -> FREE; only that
 * winner decrements the owning admission's inflight. Returns 1 if this call
 * released the slot, 0 if the lease was not HELD (FREE, mid-RESERVING, or already
 * released) - which decrements nothing. After a successful release the lease is
 * reusable.
 */
int  hl_smtp_admission_release(HlSmtpAdmissionLease *lease);

#endif /* HL_CAP_SMTP_ADMIT_H */
