/*
 * cap/http_redirect_guard.h - one outbound request's redirect policy
 *
 * Kept free of Keel headers so the async context (shared/async.h) can embed
 * it. The check itself (hl_http_redirect_allowed) and the initialiser
 * (hl_http_redirect_guard_init) live in cap/http.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_REDIRECT_GUARD_H
#define HL_CAP_HTTP_REDIRECT_GUARD_H

#include <stddef.h>

struct HlHttpConfig;

/** Room for the origin's host (or AF_UNIX socket path). */
#define HL_HTTP_GUARD_ORIGIN_MAX 256

/**
 * @brief One request's redirect policy: the (sealed, process-lifetime)
 * config whose allowlist every hop must pass; whether the chain began on
 * https - a chain that did may not be redirected to http:// (audit 25: a
 * 307/308 body or an X-Api-Key header went out in cleartext; Keel strips only
 * Authorization / Cookie / Proxy-Authorization on a scheme change); and the
 * chain's first origin plus whether the request carries anything Keel would
 * re-send to another origin (a body, or a caller header outside the safe
 * list). Such a request may not be redirected to another origin, even an
 * allowed one (audit 26).
 */
typedef struct HlHttpRedirectGuard {
    const struct HlHttpConfig *cfg;
    int    origin_https;
    int    carries_secrets;  /* a body, or a header outside the safe list */
    int    origin_known;     /* origin_host holds the whole host / path */
    int    origin_unix;
    int    origin_port;
    size_t origin_len;
    char   origin_host[HL_HTTP_GUARD_ORIGIN_MAX];
} HlHttpRedirectGuard;

#endif /* HL_CAP_HTTP_REDIRECT_GUARD_H */
