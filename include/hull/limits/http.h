/*
 * hull/limits/http.h - outbound HTTP client timeout default + ceilings
 *
 * The timeout is ONE deadline for the whole request: connect, TLS, send,
 * receive and every redirect hop.
 *
 *   - async (http.fetch / http.async.*): Hull arms it as the op's deadline,
 *     on the attached path (a request handler) and the detached one (timers,
 *     WebSocket callbacks, app.main) alike. It covers the whole redirect
 *     chain: Keel restarts its own per-hop timer on every hop, Hull's does
 *     not.
 *   - sync (http.get / post / put / patch / delete / request): Keel's sync
 *     client bounds one hop by timeout_ms (Keel >= 3.3.0); Hull computes the
 *     absolute deadline up front and, before each redirect hop, hands Keel
 *     only what is left of it, refusing the hop once nothing is left.
 *
 * DNS is NOT interruptible by the deadline. Both paths resolve with a
 * blocking getaddrinfo (the sync client always; the async client because
 * Hull sets system_dns = 1, which runs it on the event-loop thread). The
 * time a lookup takes counts against the deadline, but a stalled resolver
 * blocks until the system resolver's own timeout gives up.
 *
 * Redirects: at most HL_HTTP_MAX_REDIRECTS hops (unless HlHttpConfig sets
 * its own max_redirects), each checked against manifest.hosts.
 *
 * Resolution: a per-call option overrides the manifest's `http.timeout_ms`
 * (JS `http.timeoutMs`), which overrides HL_HTTP_DEFAULT_TIMEOUT_MS; the
 * result is clamped to the path's ceiling. The sync ceiling is lower
 * because a sync call blocks the event loop for its whole duration.
 *
 * Header-only: used by the manifest parsers (base), the http cap and the
 * runtime bindings (the composed http feature), and the unit tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_HTTP_H
#define HL_LIMITS_HTTP_H

#define HL_HTTP_DEFAULT_TIMEOUT_MS    30000    /* 30 s */
#define HL_HTTP_FETCH_MAX_TIMEOUT_MS  600000   /* 10 min: async http.fetch */
#define HL_HTTP_SYNC_MAX_TIMEOUT_MS   60000    /* 60 s: sync calls block the loop */
#define HL_HTTP_MAX_REDIRECTS         5        /* hops followed (Keel's default is 10) */

/* The effective whole-request timeout. @p per_call and @p app_default are
 * 0 when not given (anything <= 0 counts as not given); @p ceiling is
 * HL_HTTP_FETCH_MAX_TIMEOUT_MS or HL_HTTP_SYNC_MAX_TIMEOUT_MS. */
static inline int hl_http_timeout_resolve(long long per_call,
                                          long long app_default,
                                          int ceiling)
{
    long long t = per_call > 0    ? per_call
                : app_default > 0 ? app_default
                :                   HL_HTTP_DEFAULT_TIMEOUT_MS;
    if (t > ceiling) t = ceiling;
    return (int)t;
}

/* The redirect hop limit handed to Keel: @p configured when positive, else
 * HL_HTTP_MAX_REDIRECTS (never Keel's own default of 10). */
static inline int hl_http_max_redirects(int configured)
{
    return configured > 0 ? configured : HL_HTTP_MAX_REDIRECTS;
}

#endif /* HL_LIMITS_HTTP_H */
