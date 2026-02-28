/*
 * http.c — HTTP client capability implementation
 *
 * Synchronous HTTP/1.1 client with host allowlist, timeouts, and
 * optional TLS support via Keel's KlTls vtable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http.h"
#include "hull/cap/http_parser.h"
#include "hull/limits.h"

#include <keel/allocator.h>
#include <keel/tls.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "log.h"

/* ── CRLF injection guard ────────────────────────────────────────── */

static int has_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    }
    return 0;
}

/* ── URL parsing ─────────────────────────────────────────────────── */

int hl_http_parse_url(const char *url, HlParsedUrl *out)
{
    if (!url || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    /* Scheme */
    if (strncmp(url, "https://", 8) == 0) {
        out->is_https = 1;
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->is_https = 0;
        url += 7;
    } else {
        return -1;  /* unsupported scheme */
    }

    /* Host (may include :port) */
    out->host = url;

    /* Handle IPv6 addresses in brackets: [::1]:8080 */
    if (*url == '[') {
        const char *bracket = strchr(url, ']');
        if (!bracket)
            return -1;
        out->host = url + 1;
        out->host_len = (size_t)(bracket - url - 1);
        url = bracket + 1;
    } else {
        /* Find end of host (: or / or end) */
        const char *host_start = url;
        while (*url && *url != ':' && *url != '/')
            url++;
        out->host_len = (size_t)(url - host_start);
    }

    if (out->host_len == 0)
        return -1;

    /* Reject CRLF in hostname (header injection) */
    if (has_crlf(out->host, out->host_len))
        return -1;

    /* Port (optional) */
    if (*url == ':') {
        url++;
        char *end;
        long p = strtol(url, &end, 10);
        if (end == url || p < 1 || p > 65535)
            return -1;
        out->port = (int)p;
        url = end;
    } else {
        out->port = out->is_https ? 443 : 80;
    }

    /* Path (rest of URL, or "/" if empty) — reject CRLF (header injection) */
    if (*url == '/') {
        out->path = url;
        out->path_len = strlen(url);
        if (has_crlf(out->path, out->path_len))
            return -1;
    } else if (*url == '\0') {
        out->path = "/";
        out->path_len = 1;
    } else {
        return -1;  /* unexpected character after port */
    }

    return 0;
}

/* ── Host allowlist check ────────────────────────────────────────── */

int hl_http_check_host(const HlHttpConfig *cfg,
                        const char *host, size_t host_len)
{
    if (!cfg || !host)
        return -1;

    for (int i = 0; i < cfg->count; i++) {
        const char *allowed = cfg->allowed_hosts[i];
        size_t alen = strlen(allowed);
        if (alen == host_len && strncasecmp(allowed, host, host_len) == 0)
            return 0;
    }

    return -1;  /* not allowed */
}

/* ── I/O abstraction (plain or TLS) ──────────────────────────────── */

static ssize_t io_write(int fd, KlTls *tls, const void *buf, size_t len)
{
    if (tls)
        return tls->write(tls, fd, buf, len);
    ssize_t r;
    do { r = write(fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

static ssize_t io_read(int fd, KlTls *tls, void *buf, size_t len)
{
    if (tls)
        return tls->read(tls, fd, buf, len);
    ssize_t r;
    do { r = read(fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

/* ── Connect with timeout ────────────────────────────────────────── */

static int connect_with_timeout(const char *host, size_t host_len,
                                 int port, int timeout_ms)
{
    /* NUL-terminate hostname */
    char host_buf[256];
    if (host_len >= sizeof(host_buf))
        return -1;
    memcpy(host_buf, host, host_len);
    host_buf[host_len] = '\0';

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host_buf, port_str, &hints, &res);
    if (rc != 0 || !res)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* Per-socket SIGPIPE protection (defense-in-depth; Keel already
     * sets signal(SIGPIPE, SIG_IGN) process-wide in kl_server_run) */
#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    /* Set non-blocking for connect timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (rc < 0) {
        /* Non-blocking connect — wait for completion */
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        rc = poll(&pfd, 1, timeout_ms);
        if (rc <= 0) {
            close(fd);
            return -1;
        }

        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }

    /* Restore blocking mode for subsequent I/O */
    fcntl(fd, F_SETFL, flags);

    return fd;
}

/* ── TLS handshake ───────────────────────────────────────────────── */

static KlTls *do_tls_handshake(int fd, KlTlsConfig *tls_cfg,
                                 const char *host, size_t host_len,
                                 int timeout_ms)
{
    if (!tls_cfg || !tls_cfg->factory)
        return NULL;

    /* We need a temporary allocator for the TLS session.
     * Use a simple default allocator. */
    KlAllocator alloc = kl_allocator_default();

    KlTls *tls = tls_cfg->factory(tls_cfg->ctx, &alloc);
    if (!tls)
        return NULL;

    /* Set SNI hostname (if the backend supports it — check for our mbedTLS backend).
     * We use a small trick: try to call kl_tls_mbedtls_set_hostname via the linker.
     * But to stay backend-agnostic, we don't. Instead we import it conditionally. */
#ifdef KEEL_TLS_MBEDTLS_H  /* header was included — mbedTLS backend available */
    {
        char host_buf[256];
        if (host_len < sizeof(host_buf)) {
            memcpy(host_buf, host, host_len);
            host_buf[host_len] = '\0';
            kl_tls_mbedtls_set_hostname(tls, host_buf);
        }
    }
#else
    (void)host; (void)host_len;
#endif

    /* Handshake loop with timeout */
    int elapsed = 0;
    int step = 100;  /* poll 100ms at a time */

    for (;;) {
        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK)
            return tls;
        if (r == KL_TLS_ERROR) {
            tls->destroy(tls);
            return NULL;
        }

        /* Wait for readable/writable */
        short events = (r == KL_TLS_WANT_READ) ? POLLIN : POLLOUT;
        struct pollfd pfd = { .fd = fd, .events = events };
        int pr = poll(&pfd, 1, step);
        if (pr < 0) {
            tls->destroy(tls);
            return NULL;
        }

        elapsed += step;
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            tls->destroy(tls);
            return NULL;
        }
    }
}

/* ── Send HTTP request ───────────────────────────────────────────── */

static int send_request(int fd, KlTls *tls,
                         const char *method, const HlParsedUrl *url,
                         const HlHttpHeader *headers, int num_headers,
                         const char *body, size_t body_len,
                         int timeout_ms)
{
    /* Reject CRLF in method (header injection) */
    if (has_crlf(method, strlen(method)))
        return -1;

    /* Build request line + headers into a buffer */
    char buf[HL_HTTP_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       (int)url->path_len, url->path,
                       (int)url->host_len, url->host);

    if (off < 0 || (size_t)off >= sizeof(buf)) {
        log_warn("http: request line exceeds %d-byte buffer", HL_HTTP_REQ_BUF_SIZE);
        return -1;
    }

    /* Add user headers */
    for (int i = 0; i < num_headers; i++) {
        /* Reject CRLF in header names and values (header injection) */
        if (has_crlf(headers[i].name, strlen(headers[i].name)) ||
            has_crlf(headers[i].value, strlen(headers[i].value)))
            return -1;
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "%s: %s\r\n", headers[i].name, headers[i].value);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf)) {
            log_warn("http: request headers exceed %d-byte buffer", HL_HTTP_REQ_BUF_SIZE);
            return -1;
        }
        off += n;
    }

    /* Content-Length if body present */
    if (body && body_len > 0) {
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf))
            return -1;
        off += n;
    }

    /* Connection: close */
    int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                     "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)(off + n) >= sizeof(buf)) {
        log_warn("http: request headers exceed %d-byte buffer", HL_HTTP_REQ_BUF_SIZE);
        return -1;
    }
    off += n;

    /* Send header block */
    size_t sent = 0;
    while (sent < (size_t)off) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;

        ssize_t w = io_write(fd, tls, buf + sent, (size_t)off - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }

    /* Send body */
    if (body && body_len > 0) {
        sent = 0;
        while (sent < body_len) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0)
                return -1;

            ssize_t w = io_write(fd, tls, body + sent, body_len - sent);
            if (w <= 0)
                return -1;
            sent += (size_t)w;
        }
    }

    return 0;
}

/* ── Receive + parse response ────────────────────────────────────── */

static int recv_response(int fd, KlTls *tls, HlHttpResponse *resp,
                          size_t max_response_size, int timeout_ms)
{
    KlAllocator alloc = kl_allocator_default();
    HlHttpParser *parser = hl_http_parser_llhttp(max_response_size, &alloc);
    if (!parser)
        return -1;

    char buf[HL_HTTP_RECV_BUF_SIZE];
    int ret = -1;

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            break;  /* timeout or error */

        ssize_t nread = io_read(fd, tls, buf, sizeof(buf));
        if (nread < 0)
            break;  /* error */
        if (nread == 0) {
            /* Connection closed — check if we have a complete response */
            if (resp->status > 0) {
                ret = 0;
            }
            break;
        }

        size_t consumed;
        HlHttpParseResult pr2 = parser->parse(parser, resp,
                                               buf, (size_t)nread, &consumed);
        if (pr2 == HL_HTTP_PARSE_OK) {
            ret = 0;
            break;
        }
        if (pr2 == HL_HTTP_PARSE_ERROR)
            break;

        /* HL_HTTP_PARSE_INCOMPLETE — continue reading */
    }

    /* Body is NUL-terminated by the parser's transfer_to_response */

    parser->destroy(parser);
    return ret;
}

/* ── Public API ──────────────────────────────────────────────────── */

int hl_cap_http_request(const HlHttpConfig *cfg,
                        const char *method, const char *url,
                        const HlHttpHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        HlHttpResponse *resp)
{
    if (!cfg || !method || !url || !resp)
        return -1;
    if (num_headers < 0 || num_headers > HL_HTTP_MAX_REQ_HEADERS)
        return -1;
    if (num_headers > 0 && !headers)
        return -1;

    memset(resp, 0, sizeof(*resp));

    int timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : HL_HTTP_DEFAULT_TIMEOUT_MS;
    size_t max_resp = cfg->max_response_size > 0 ? cfg->max_response_size
                                                  : (size_t)HL_HTTP_DEFAULT_MAX_RESP;

    /* Parse URL */
    HlParsedUrl parsed;
    if (hl_http_parse_url(url, &parsed) != 0)
        return -1;

    /* Check host allowlist */
    if (hl_http_check_host(cfg, parsed.host, parsed.host_len) != 0)
        return -1;

    /* HTTPS requires TLS config */
    KlTlsConfig *tls_cfg = (KlTlsConfig *)cfg->tls;
    if (parsed.is_https && !tls_cfg)
        return -1;

    /* Connect */
    int fd = connect_with_timeout(parsed.host, parsed.host_len,
                                   parsed.port, timeout_ms);
    if (fd < 0)
        return -1;

    KlTls *tls = NULL;
    int ret = -1;

    /* TLS handshake (if HTTPS) */
    if (parsed.is_https) {
        tls = do_tls_handshake(fd, tls_cfg, parsed.host, parsed.host_len,
                                timeout_ms);
        if (!tls)
            goto cleanup;
    }

    /* Send request */
    if (send_request(fd, tls, method, &parsed,
                     headers, num_headers, body, body_len, timeout_ms) != 0)
        goto cleanup;

    /* Receive response */
    if (recv_response(fd, tls, resp, max_resp, timeout_ms) != 0)
        goto cleanup;

    ret = 0;

cleanup:
    if (tls) {
        tls->shutdown(tls, fd);
        tls->destroy(tls);
    }
    close(fd);

    if (ret != 0)
        hl_cap_http_free(resp);

    return ret;
}

void hl_cap_http_free(HlHttpResponse *resp)
{
    if (!resp)
        return;

    KlAllocator alloc = kl_allocator_default();

    if (resp->body) {
        kl_free(&alloc, resp->body, resp->body_len + 1);
        resp->body = NULL;
        resp->body_len = 0;
    }

    if (resp->headers) {
        for (int i = 0; i < resp->num_headers; i++) {
            kl_free(&alloc, (char *)resp->headers[i].name,
                    strlen(resp->headers[i].name) + 1);
            kl_free(&alloc, (char *)resp->headers[i].value,
                    strlen(resp->headers[i].value) + 1);
        }
        kl_free(&alloc, resp->headers,
                (size_t)resp->num_headers * sizeof(HlHttpHeader));
        resp->headers = NULL;
    }
    resp->num_headers = 0;
    resp->status = 0;
}
