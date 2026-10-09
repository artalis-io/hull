/*
 * cap/http.h - HTTP client capability with host allowlist
 *
 * Thin wrapper around Keel's HTTP client (keel/http_client.h) that adds
 * host allowlist checking and audit logging.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_H
#define HL_CAP_HTTP_H

#include <keel/http_client.h>

#include "hull/limits/http.h"

/* Forward declarations for optional modules */
typedef struct KlHttpClientPool KlHttpClientPool;

/* Backward-compatible typedef - Hull code uses HlHttpHeader for request headers */
typedef KlHttpClientHeader HlHttpHeader;

/**
 * @brief HTTP client configuration.
 */
typedef struct HlHttpConfig {
    const char     **allowed_hosts;    /**< Host allowlist (exact match) */
    int              count;            /**< Number of allowed hosts */
    int              timeout_ms;       /**< App-wide whole-request timeout in ms
                                            *   (DNS, connect, TLS, send, receive and
                                            *   every redirect hop), from manifest
                                            *   http.timeout_ms; <= 0 = the 30 s
                                            *   default. A per-call option overrides
                                            *   it; both are clamped to the path's
                                            *   ceiling (hull/limits/http.h). */
    size_t           max_response_size;/**< Max response body bytes (default: 4 MB) */
    KlTlsConfig     *tls;             /**< KlTlsConfig* for HTTPS - NULL = no HTTPS */
    KlHttpClientPool    *pool;             /**< Connection pool (NULL = no pooling) */
    int              follow_redirects; /**< 1 = follow 3xx redirects (default) */
    int              max_redirects;    /**< Max redirect hops (0 = Keel default 10) */
    KlDecompressConfig *decompress;    /**< Response decompression (NULL = disabled) */
} HlHttpConfig;

/**
 * @brief Perform a synchronous HTTP request.
 *
 * Checks host allowlist, audits, then delegates to kl_http_client_request().
 * Blocks until the response is received, an error occurs, or the deadline
 * passes: ONE deadline for the whole request (Keel >= 3.3.0).
 *
 * @param cfg      HTTP client configuration (host allowlist, timeouts, TLS).
 * @param timeout_ms Per-call whole-request timeout in ms; <= 0 = the app's
 *                 default (cfg->timeout_ms, else 30 s). Clamped to
 *                 HL_HTTP_SYNC_MAX_TIMEOUT_MS: the call blocks the loop.
 * @param method   HTTP method ("GET", "POST", etc.).
 * @param url      Full URL ("http://host/path" or "https://host/path").
 * @param headers  Request headers (may be NULL).
 * @param num_headers Number of request headers.
 * @param body     Request body (may be NULL).
 * @param body_len Request body length.
 * @param resp     Output: populated on success. Caller must call kl_http_client_response_free().
 * @return 0 on success, -1 on error.
 */
int hl_cap_http_request(const HlHttpConfig *cfg, int timeout_ms,
                        const char *method, const char *url,
                        const HlHttpHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        KlHttpClientResponse *resp);

/* ── Internal helpers (exposed for unit testing) ─────────────────── */

/**
 * @brief Per-hop redirect check for Keel (KlHttpRedirectConfig.on_redirect):
 * the next URL's host must pass the same allowlist as the first one.
 * @p data is the HlHttpConfig. 0 to follow, -1 to refuse (audited).
 */
int hl_http_redirect_allowed(const char *next_url, void *data);

/**
 * @brief Check if a hostname is in the allowlist.
 * @return 0 if allowed, -1 if denied.
 */
int hl_http_check_host(const HlHttpConfig *cfg,
                        const char *host, size_t host_len);

#endif /* HL_CAP_HTTP_H */
