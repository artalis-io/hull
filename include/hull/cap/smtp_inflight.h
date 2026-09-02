/**
 * @file cap/smtp_inflight.h
 * @brief Per-server registry of in-flight async SMTP ops (the shutdown sweep).
 *
 * A model-2 SMTP op that has been submitted retains a runtime-side reference
 * (the submit ctx) until its terminal is consumed. On the normal path the runtime
 * continuation consumes the terminal and unlinks the op; but a completion callback
 * can be DROPPED at pool shutdown (poll backend), and a still-parked continuation
 * is not auto-cancelled by pool/backend free (docs/smtp_keel_slice2c_plan.md
 * section 10). So the server keeps every in-flight op in this registry and, AFTER
 * pool_free() has joined the workers (every terminal published, every worker ref
 * dropped), SWEEPS the remainder: it releases each op's retained runtime ref and
 * tears the op down exactly once.
 *
 * The registry is touched ONLY on the event-loop thread (submit adds, the resume
 * path removes, the shutdown sweep drains), so it needs no lock. Nodes are
 * INTRUSIVE (embedded in the binding's async-op struct) so tracking an op costs
 * no extra allocation; each node carries the op's own teardown as a callback, so
 * the registry stays runtime-agnostic (it never sees a coroutine, Promise, or
 * KlAsyncOp).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_INFLIGHT_H
#define HL_CAP_SMTP_INFLIGHT_H

/* Full teardown of one swept op (release the retained runtime ref + free the
 * op's continuation/binding storage). Runs exactly once per node, from the sweep.
 * @p owner is the value passed to hl_smtp_inflight_add. */
typedef void (*HlSmtpInflightRelease)(void *owner);

/* Intrusive registry node: embed one in the binding's async-op struct. */
typedef struct HlSmtpInflightNode {
    struct HlSmtpInflightNode *prev, *next;
    struct HlSmtpInflight     *reg;      /* owning registry while linked */
    void                      *owner;    /* the async op (passed to release) */
    HlSmtpInflightRelease      release;  /* full teardown, called by the sweep */
    int                        linked;   /* 1 while in a registry */
} HlSmtpInflightNode;

/* Per-server registry (circular list with a sentinel head). */
typedef struct HlSmtpInflight {
    HlSmtpInflightNode head;   /* sentinel: head.next/prev ring the live nodes */
    int                count;
} HlSmtpInflight;

/* Initialise an empty registry. */
void hl_smtp_inflight_init(HlSmtpInflight *r);

/* Track an op: link @p n at the head, recording @p owner + @p release for the
 * sweep. Called when an op is submitted (suspended or suspend-failed-retained). */
void hl_smtp_inflight_add(HlSmtpInflight *r, HlSmtpInflightNode *n,
                          void *owner, HlSmtpInflightRelease release);

/* Untrack an op (the normal resume path consumed its terminal and is tearing it
 * down itself). O(1); a no-op if @p n is not currently linked. Does NOT call the
 * release callback - the caller owns the teardown on this path. */
void hl_smtp_inflight_remove(HlSmtpInflightNode *n);

/* Number of currently-tracked ops. */
int hl_smtp_inflight_count(const HlSmtpInflight *r);

/* Drain the registry: unlink every node and invoke its release callback exactly
 * once (each node is unlinked BEFORE its release runs, so a release that frees the
 * node's storage is safe). Returns the number swept. Call AFTER pool_free(). */
int hl_smtp_inflight_sweep(HlSmtpInflight *r);

#endif /* HL_CAP_SMTP_INFLIGHT_H */
