/*
 * cap/http.h — HTTP client capability with host allowlist
 *
 * Thin wrapper around Keel's HTTP client (keel/client.h) that adds
 * host allowlist checking and audit logging.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_H
#define HL_CAP_HTTP_H

#include <keel/client.h>

/* Backward-compatible typedef — Hull code uses HlHttpHeader for request headers */
typedef KlClientHeader HlHttpHeader;

/**
 * @brief HTTP client configuration.
 */
typedef struct HlHttpConfig {
    const char     **allowed_hosts;    /**< Host allowlist (exact match) */
    int              count;            /**< Number of allowed hosts */
    int              timeout_ms;       /**< Connect/send/recv timeout (default: 30000) */
    size_t           max_response_size;/**< Max response body bytes (default: 4 MB) */
    KlTlsConfig     *tls;             /**< KlTlsConfig* for HTTPS — NULL = no HTTPS */
} HlHttpConfig;

/**
 * @brief Perform a synchronous HTTP request.
 *
 * Checks host allowlist, audits, then delegates to kl_client_request().
 * Blocks until the response is received, an error occurs, or timeout.
 *
 * @param cfg      HTTP client configuration (host allowlist, timeouts, TLS).
 * @param method   HTTP method ("GET", "POST", etc.).
 * @param url      Full URL ("http://host/path" or "https://host/path").
 * @param headers  Request headers (may be NULL).
 * @param num_headers Number of request headers.
 * @param body     Request body (may be NULL).
 * @param body_len Request body length.
 * @param resp     Output: populated on success. Caller must call kl_client_response_free().
 * @return 0 on success, -1 on error.
 */
int hl_cap_http_request(const HlHttpConfig *cfg,
                        const char *method, const char *url,
                        const HlHttpHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        KlClientResponse *resp);

/* ── Internal helpers (exposed for unit testing) ─────────────────── */

/**
 * @brief Check if a hostname is in the allowlist.
 * @return 0 if allowed, -1 if denied.
 */
int hl_http_check_host(const HlHttpConfig *cfg,
                        const char *host, size_t host_len);

#endif /* HL_CAP_HTTP_H */
