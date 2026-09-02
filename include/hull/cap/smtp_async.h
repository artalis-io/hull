/**
 * @file cap/smtp_async.h
 * @brief Model-2 async SMTP orchestration: the shared C ownership + lifecycle.
 *
 * This is the ONE place the model-2 ownership logic lives; the per-runtime
 * bindings (Lua yield / JS Promise) only extract args, build the deep-copied
 * #HlSmtpOp, create the #HlAsyncCtx continuation, call hl_smtp_async_submit, and
 * read the terminal via hl_smtp_async_finish. It composes the submit layer
 * (reserve -> submit -> suspend), the admission cap, the per-worker TLS cache, and
 * the in-flight registry, and owns the FROZEN two-pass shutdown
 * (docs/smtp_keel_slice2c_plan.md sections 9, 10, 10a).
 *
 * Ownership (single retained runtime ref): the async op is the #HlAsyncCtx driver;
 * its free_driver drops the submit ctx exactly once, on the resume path OR the
 * shutdown sweep, never both (the sweep unlink-before-releases). A cancelled op's
 * resume is suppressed (non-resumable) and left for the sweep, so a shutdown never
 * resumes a coroutine/Promise mid-teardown.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_ASYNC_H
#define HL_CAP_SMTP_ASYNC_H

#include <stddef.h>

#include "hull/cap/smtp.h"          /* HlSmtpMessage, HlSmtpResult */
#include "hull/cap/smtp_op.h"       /* HlSmtpOp */
#include "hull/cap/smtp_admit.h"    /* HlSmtpAdmission */
#include "hull/cap/smtp_inflight.h" /* HlSmtpInflight */
#include "hull/cap/smtp_tls.h"      /* HlSmtpTrust */

/* Forward-declared to avoid pulling the async/net headers into every includer. */
typedef struct HlAsyncCtx          HlAsyncCtx;
typedef struct HlAsyncBackendPool  HlAsyncBackendPool;
typedef struct HlNetBackendCtx     HlNetBackendCtx;

/* Server-owned async-SMTP context, built once at serve wiring and reachable from
 * the runtime. Holds the admission cap (from the pool worker count W), the CA
 * trust descriptor for per-worker TLS, and the in-flight registry. */
typedef struct HlSmtpServerCtx {
    HlSmtpAdmission admission;      /* cap = max(1, floor(W/2)); 0 if W<=1 */
    HlSmtpInflight  inflight;       /* registry of suspended/retained ops */
    HlSmtpTrust     trust;          /* immutable CA bytes for per-worker TLS */
    int             shutting_down;  /* set by pass 1; blocks new submissions */
} HlSmtpServerCtx;

/* Initialise the server ctx: admission cap from @p workers, trust from the
 * immutable CA buffer (@p ca_buf/@p ca_len may be NULL/0 for a plaintext-only or
 * no-CA build), registry empty, not shutting down. @p alloc is the borrowed
 * server allocator (KlAllocator*) for the per-worker TLS contexts. */
void hl_smtp_server_ctx_init(HlSmtpServerCtx *s, int workers,
                             const unsigned char *ca_buf, size_t ca_len,
                             void *alloc);

/* Whether async admission is enabled at all (cap > 0). A W<=1 pool disables it,
 * so the binding must fall back (there is no headroom to admit an SMTP job). */
int hl_smtp_server_async_enabled(const HlSmtpServerCtx *s);

/* The async op: the #HlAsyncCtx driver + the registry node. Opaque to callers. */
typedef struct HlSmtpAsyncOp HlSmtpAsyncOp;

/* Submit request, filled by the binding. */
typedef struct {
    HlSmtpServerCtx    *server;     /* server ctx (admission + registry + trust) */
    HlAsyncBackendPool *pool;       /* async pool (seam A) */
    HlNetBackendCtx    *net_ctx;    /* net backend (seam B: suspend/complete/cancel) */
    HlAsyncCtx         *actx;       /* continuation vehicle (binding-created, cont wired) */
    void               *req_handle; /* active connection (HlReqHandle*); NULL => detached */
    int                 detached;   /* 1 = no connection (timer callback) */
    HlSmtpOp           *inputs;     /* deep-copied message; ownership moves IN */
} HlSmtpAsyncReq;

typedef enum {
    HL_SMTP_ASYNC_SUSPENDED = 0,  /* parked; binding yields (Lua) / returns Promise (JS) */
    HL_SMTP_ASYNC_RESOLVED,       /* immediate failure; result is final now */
} HlSmtpAsyncDisposition;

typedef struct {
    HlSmtpAsyncDisposition disposition;
    HlSmtpResult           result;    /* valid iff RESOLVED */
    const char            *schedule;  /* audit tag iff RESOLVED scheduling failure */
    HlSmtpAsyncOp         *op;         /* the driver: the binding sets actx->driver etc.
                                          via the return; SUSPENDED wires it, RESOLVED
                                          has already torn it down (op == NULL). */
} HlSmtpAsyncOutcome;

/* Event-loop-thread submit. Composes admission + submit-layer + registry per the
 * frozen ordering, wiring the injected pool/suspend/resume/cancel seams onto
 * @p req->actx. On SUSPENDED the op is registered and the actx parked; the binding
 * yields. On RESOLVED the immediate scheduling failure has ALREADY been audited
 * once here and the unparked actx torn down (suspend-failed excepted: it stays
 * registered for the sweep, audited once). Takes ownership of req->inputs. */
void hl_smtp_async_submit(const HlSmtpAsyncReq *req, HlSmtpAsyncOutcome *out);

/* Called from the binding's push_result (on the resume path): read the published
 * terminal and emit the single completion audit (exactly once). Fills *out with
 * the terminal so the binding can build its {ok,error} value. */
void hl_smtp_async_finish(HlSmtpAsyncOp *op, HlSmtpResult *out);

/* ── FROZEN two-pass shutdown (docs section 10a) ─────────────────────── */

/* PASS 1, BEFORE pool_free(): stop new submissions and request cancellation of
 * every in-flight op (mark non-resumable + flip the worker to CANCEL_REQUESTED)
 * so a running transport observes the shutdown and terminates promptly.
 * Registry-preserving + idempotent. */
void hl_smtp_server_request_cancel_all(HlSmtpServerCtx *s);

/* PASS 2, AFTER pool_free(): unlink and release every remaining op exactly once
 * (cancel the parked continuation, drop the retained runtime ref). Returns the
 * number swept. */
int  hl_smtp_server_sweep(HlSmtpServerCtx *s);

#endif /* HL_CAP_SMTP_ASYNC_H */
