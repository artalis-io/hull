/**
 * @file cap/smtp_tls.h
 * @brief Per-worker-thread client TLS context cache, keyed by trust owner.
 *
 * MBEDTLS_THREADING is off, so a single shared KlTlsCtx (RNG / ssl_config) is not
 * safe for concurrent use by SMTP workers. Each worker thread instead lazily
 * builds and caches its OWN KlTlsCtx from the server's immutable, in-memory CA
 * material (no filesystem read on the worker), destroyed by the thread's
 * pthread-key destructor at pool teardown. See docs/smtp_keel_slice2c_plan.md.
 *
 * The cache is keyed by the OWNING trust descriptor (#HlSmtpTrust identity), not
 * by a process-global "first context on this thread": distinct server / trust
 * owners never share a derived context, and a worker that serves two owners
 * holds a distinct context for each.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_TLS_H
#define HL_CAP_SMTP_TLS_H

#include <stddef.h>

/**
 * Server-owned trust configuration. The CA buffer and allocator are BORROWED
 * from immutable server-owned storage and must stay valid until the worker pool
 * is fully drained (past every pthread-key destructor); only the derived
 * per-worker KlTlsCtx is worker-cached.
 */
typedef struct HlSmtpTrust {
    const unsigned char *ca_buf;   /* borrowed immutable CA bytes (PEM/DER) */
    size_t               ca_len;
    void                *alloc;     /* KlAllocator*, borrowed, server-owned */
} HlSmtpTrust;

/**
 * This thread's client KlTlsCtx (returned opaque) for @p trust, created lazily
 * once per (thread, trust) from @p trust->ca_buf via
 * hl_tls_client_ctx_create_from_buf (no file read). Returns NULL on creation
 * failure, caching nothing (a later call retries). The context is owned by the
 * thread's pthread-key and destroyed at thread exit via hl_tls_ctx_destroy.
 */
void *hl_smtp_tls_ctx_for(const HlSmtpTrust *trust);

#endif /* HL_CAP_SMTP_TLS_H */
