/*
 * hull/limits/http.h - outbound HTTP client timeout default + ceilings
 *
 * The timeout is ONE deadline for the whole request: DNS, connect, TLS,
 * send, receive and every redirect hop. The async path (http.fetch /
 * http.async.*) arms it as the op's deadline; the sync path (http.get /
 * post / put / patch / delete / request) hands it to Keel's sync client,
 * which since Keel v3.3.0 also treats it as one whole-request deadline.
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

#endif /* HL_LIMITS_HTTP_H */
