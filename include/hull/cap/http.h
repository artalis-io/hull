/*
 * cap/http.h — HTTP client capability with host allowlist
 *
 * Provides synchronous outbound HTTP requests with configurable
 * host allowlists, timeouts, response size limits, and optional
 * TLS support via Keel's KlTls vtable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_H
#define HL_CAP_HTTP_H

#include <stddef.h>

/* Forward declaration — avoid pulling in keel/tls.h */
typedef struct {
    void    *ctx;
    void    *factory;
    void   (*ctx_destroy)(void *ctx);
} HlTlsBridge;

/**
 * @brief Single HTTP header (name: value).
 */
typedef struct HlHttpHeader {
    const char *name;
    const char *value;
} HlHttpHeader;

/**
 * @brief HTTP client configuration.
 */
typedef struct HlHttpConfig {
    const char     **allowed_hosts;    /**< Host allowlist (exact match) */
    int              count;            /**< Number of allowed hosts */
    int              timeout_ms;       /**< Connect/send/recv timeout (default: 30000) */
    size_t           max_response_size;/**< Max response body bytes (default: 4 MB) */
    void            *tls;             /**< KlTlsConfig* for HTTPS — NULL = no HTTPS */
} HlHttpConfig;

/**
 * @brief HTTP response (owned — caller must call hl_cap_http_free).
 */
typedef struct HlHttpResponse {
    int           status;              /**< HTTP status code */
    char         *body;                /**< Response body (heap-allocated, NUL-terminated) */
    size_t        body_len;            /**< Body length in bytes */
    HlHttpHeader *headers;             /**< Response headers (heap-allocated array) */
    int           num_headers;         /**< Number of response headers */
} HlHttpResponse;

/**
 * @brief Perform a synchronous HTTP request.
 *
 * Blocks until the response is received, an error occurs, or timeout.
 *
 * @param cfg      HTTP client configuration (host allowlist, timeouts, TLS).
 * @param method   HTTP method ("GET", "POST", etc.).
 * @param url      Full URL ("http://host/path" or "https://host/path").
 * @param headers  Request headers (may be NULL).
 * @param num_headers Number of request headers.
 * @param body     Request body (may be NULL).
 * @param body_len Request body length.
 * @param resp     Output: populated on success. Caller must call hl_cap_http_free().
 * @return 0 on success, -1 on error.
 */
int hl_cap_http_request(const HlHttpConfig *cfg,
                        const char *method, const char *url,
                        const HlHttpHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        HlHttpResponse *resp);

/**
 * @brief Free response resources.
 */
void hl_cap_http_free(HlHttpResponse *resp);

/* ── Internal helpers (exposed for unit testing) ─────────────────── */

/**
 * @brief Parsed URL components (points into original URL string).
 */
typedef struct {
    int         is_https;
    const char *host;
    size_t      host_len;
    int         port;
    const char *path;      /**< Includes leading '/' and query string */
    size_t      path_len;
} HlParsedUrl;

/**
 * @brief Parse a URL into components.
 * @return 0 on success, -1 on error.
 */
int hl_http_parse_url(const char *url, HlParsedUrl *out);

/**
 * @brief Check if a hostname is in the allowlist.
 * @return 0 if allowed, -1 if denied.
 */
int hl_http_check_host(const HlHttpConfig *cfg,
                        const char *host, size_t host_len);

#endif /* HL_CAP_HTTP_H */
