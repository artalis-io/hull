/*
 * http_async.c — Non-blocking HTTP client via KlWatcher state machine
 *
 * Replaces poll() blocking with KlWatcher callbacks on the event loop.
 * The socket stays non-blocking throughout. A state machine drives:
 *   connect → TLS handshake → send request → recv response
 *
 * All I/O happens on the event loop thread (watcher callback), so no
 * locking is needed. When the response is complete, kl_async_complete
 * resumes the suspended inbound connection.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http_async.h"
#include "hull/cap/audit.h"
#include "hull/alloc.h"

#include <keel/allocator.h>
#include <keel/server.h>
#include <keel/tls.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "log.h"

/* ── CRLF injection guard (same as http.c) ───────────────────────── */

static int has_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    }
    return 0;
}

/* ── I/O abstraction (plain or TLS) ──────────────────────────────── */

static ssize_t async_io_write(int fd, KlTls *tls, const void *buf, size_t len)
{
    if (tls)
        return tls->write(tls, fd, buf, len);
    ssize_t r;
    do { r = write(fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

static ssize_t async_io_read(int fd, KlTls *tls, void *buf, size_t len)
{
    if (tls)
        return tls->read(tls, fd, buf, len);
    ssize_t r;
    do { r = read(fd, buf, len); } while (r < 0 && errno == EINTR);
    return r;
}

/* ── Forward declarations ────────────────────────────────────────── */

static void on_event(int fd, KlEventMask ready, void *user_data);
static void complete_success(HlHttpClient *c);
static void complete_error(HlHttpClient *c);
static void handle_tls_handshake(HlHttpClient *c);

/* ── Build HTTP/1.1 request into a heap buffer ───────────────────── */

static char *build_request(HlAllocator *alloc,
                            const char *method, const HlParsedUrl *url,
                            const HlHttpHeader *headers, int num_headers,
                            const char *body, size_t body_len,
                            size_t *out_len)
{
    /* Reject CRLF in method */
    if (has_crlf(method, strlen(method)))
        return NULL;

    char buf[HL_HTTP_REQ_BUF_SIZE];
    int off = snprintf(buf, sizeof(buf), "%s %.*s HTTP/1.1\r\nHost: %.*s\r\n",
                       method,
                       (int)url->path_len, url->path,
                       (int)url->host_len, url->host);

    if (off < 0 || (size_t)off >= sizeof(buf))
        return NULL;

    /* User headers */
    for (int i = 0; i < num_headers; i++) {
        if (has_crlf(headers[i].name, strlen(headers[i].name)) ||
            has_crlf(headers[i].value, strlen(headers[i].value)))
            return NULL;
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "%s: %s\r\n", headers[i].name, headers[i].value);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf))
            return NULL;
        off += n;
    }

    /* Content-Length if body present */
    if (body && body_len > 0) {
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)(off + n) >= sizeof(buf))
            return NULL;
        off += n;
    }

    /* Connection: close + blank line */
    int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                     "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)(off + n) >= sizeof(buf))
        return NULL;
    off += n;

    /* Total = headers + body */
    size_t total = (size_t)off + body_len;
    char *req = hl_alloc_malloc(alloc, total);
    if (!req)
        return NULL;

    memcpy(req, buf, (size_t)off);
    if (body && body_len > 0)
        memcpy(req + off, body, body_len);

    *out_len = total;
    return req;
}

/* ── Async cancel/deadline callbacks ─────────────────────────────── */

static void free_driver(void *driver)
{
    HlHttpClient *c = driver;
    /* Remove watcher if still registered */
    if (c->fd >= 0) {
        kl_watcher_del(c->server, c->fd);
    }
    hl_async_http_free_client(c, 1);
}

static void on_deadline_timeout(KlAsyncOp *op, void *user_data)
{
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlHttpClient *c = ctx->driver;
    (void)op;

    log_debug("[hull:c] http.fetch timeout");

    /* Remove watcher, close socket */
    if (c->fd >= 0)
        kl_watcher_del(c->server, c->fd);

    /* Set driver to NULL so resume knows it's an error */
    ctx->driver = NULL;
    hl_async_http_free_client(c, 1);

    /* Complete the op — on_resume fires with driver=NULL */
    kl_async_complete(ctx->server, op);
}

/* ── State: CONNECTING ───────────────────────────────────────────── */

static void handle_connecting(HlHttpClient *c)
{
    /* Check connect result */
    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
        log_warn("[hull:c] http.fetch connect failed: %s", strerror(err));
        complete_error(c);
        return;
    }

    /* Connect succeeded */
    if (c->tls_cfg && c->tls_cfg->factory) {
        /* Start TLS handshake */
        KlAllocator alloc = kl_allocator_default();
        c->tls = c->tls_cfg->factory(c->tls_cfg->ctx, &alloc);
        if (!c->tls) {
            log_warn("[hull:c] http.fetch TLS create failed");
            complete_error(c);
            return;
        }

#ifdef KEEL_TLS_MBEDTLS_H
        if (c->host_buf[0])
            kl_tls_mbedtls_set_hostname(c->tls, c->host_buf);
#endif

        c->state = HL_HCLIENT_TLS_HANDSHAKE;
        /* Try handshake immediately — it will mod the watcher as needed */
        handle_tls_handshake(c);
    } else {
        /* No TLS — start sending */
        c->state = HL_HCLIENT_SENDING;
        kl_watcher_mod(c->server, c->fd, KL_EVENT_WRITE);
    }
}

/* ── State: TLS_HANDSHAKE ────────────────────────────────────────── */

static void handle_tls_handshake(HlHttpClient *c)
{
    KlTlsResult r = c->tls->handshake(c->tls, c->fd);
    if (r == KL_TLS_OK) {
        c->state = HL_HCLIENT_SENDING;
        kl_watcher_mod(c->server, c->fd, KL_EVENT_WRITE);
        return;
    }
    if (r == KL_TLS_WANT_READ) {
        kl_watcher_mod(c->server, c->fd, KL_EVENT_READ);
        return;
    }
    if (r == KL_TLS_WANT_WRITE) {
        kl_watcher_mod(c->server, c->fd, KL_EVENT_WRITE);
        return;
    }
    /* KL_TLS_ERROR */
    log_warn("[hull:c] http.fetch TLS handshake failed");
    complete_error(c);
}

/* ── State: SENDING ──────────────────────────────────────────────── */

static void handle_sending(HlHttpClient *c)
{
    while (c->request_sent < c->request_len) {
        ssize_t w = async_io_write(c->fd, c->tls,
                                    c->request_buf + c->request_sent,
                                    c->request_len - c->request_sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Re-arm watcher for WRITE */
                kl_watcher_rearm(c->server, c->fd);
                return;
            }
            log_warn("[hull:c] http.fetch send error: %s", strerror(errno));
            complete_error(c);
            return;
        }
        if (w == 0) {
            complete_error(c);
            return;
        }
        c->request_sent += (size_t)w;
    }

    /* All sent — switch to receiving */
    c->state = HL_HCLIENT_RECEIVING;
    kl_watcher_mod(c->server, c->fd, KL_EVENT_READ);
}

/* ── State: RECEIVING ────────────────────────────────────────────── */

static void handle_receiving(HlHttpClient *c)
{
    char buf[HL_HTTP_RECV_BUF_SIZE];

    for (;;) {
        ssize_t nread = async_io_read(c->fd, c->tls, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Re-arm watcher for READ */
                kl_watcher_rearm(c->server, c->fd);
                return;
            }
            complete_error(c);
            return;
        }
        if (nread == 0) {
            /* Connection closed — check if we have a response */
            if (c->resp.status > 0) {
                complete_success(c);
            } else {
                complete_error(c);
            }
            return;
        }

        size_t consumed;
        HlHttpParseResult pr = c->parser->parse(c->parser, &c->resp,
                                                  buf, (size_t)nread, &consumed);
        if (pr == HL_HTTP_PARSE_OK) {
            complete_success(c);
            return;
        }
        if (pr == HL_HTTP_PARSE_ERROR) {
            complete_error(c);
            return;
        }
        /* HL_HTTP_PARSE_INCOMPLETE — try to read more without waiting
         * (socket is non-blocking, will get EAGAIN if no data) */
    }
}

/* ── KlWatcher callback — drives the state machine ───────────────── */

static void on_event(int fd, KlEventMask ready, void *user_data)
{
    HlHttpClient *c = user_data;
    (void)fd;
    (void)ready;

    switch (c->state) {
    case HL_HCLIENT_CONNECTING:
        handle_connecting(c);
        break;
    case HL_HCLIENT_TLS_HANDSHAKE:
        handle_tls_handshake(c);
        break;
    case HL_HCLIENT_SENDING:
        handle_sending(c);
        break;
    case HL_HCLIENT_RECEIVING:
        handle_receiving(c);
        break;
    }
}

/* ── Completion helpers ──────────────────────────────────────────── */

static void complete_success(HlHttpClient *c)
{
    /* Remove watcher */
    kl_watcher_del(c->server, c->fd);

    /* Close socket + TLS */
    if (c->tls) {
        c->tls->shutdown(c->tls, c->fd);
        c->tls->destroy(c->tls);
        c->tls = NULL;
    }
    close(c->fd);
    c->fd = -1;

    /* Free request buf + parser (no longer needed) */
    if (c->request_buf) {
        hl_alloc_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }
    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    /* Audit */
    {
        ShJsonWriter w = hl_audit_begin("http.fetch");
        sh_json_write_kv_int(&w, "status", c->resp.status);
        sh_json_write_kv_int(&w, "result", 0);
        hl_audit_end(&w);
    }

    /* Complete the async op — on_resume fires with driver = client */
    kl_async_complete(c->server, &c->async_ctx->op);
}

static void complete_error(HlHttpClient *c)
{
    /* Remove watcher */
    if (c->fd >= 0)
        kl_watcher_del(c->server, c->fd);

    /* Set driver to NULL so resume knows it's an error */
    HlAsyncCtx *ctx = c->async_ctx;
    ctx->driver = NULL;

    /* Clean up the client entirely */
    hl_async_http_free_client(c, 1);

    /* Audit */
    {
        ShJsonWriter w = hl_audit_begin("http.fetch");
        sh_json_write_kv_int(&w, "result", -1);
        hl_audit_end(&w);
    }

    /* Complete — on_resume with driver=NULL indicates error */
    kl_async_complete(ctx->server, &ctx->op);
}

/* ── Public API ──────────────────────────────────────────────────── */

HlHttpClient *hl_async_http_start(KlServer *server, KlConn *conn,
                                   HlAllocator *alloc,
                                   HlHttpConfig *http_cfg,
                                   const char *method, const char *url,
                                   const HlHttpHeader *headers, int num_headers,
                                   const char *body, size_t body_len)
{
    if (!server || !conn || !http_cfg || !method || !url)
        return NULL;

    /* Parse URL */
    HlParsedUrl parsed;
    if (hl_http_parse_url(url, &parsed) != 0)
        return NULL;

    /* Host allowlist */
    if (hl_http_check_host(http_cfg, parsed.host, parsed.host_len) != 0) {
        ShJsonWriter w = hl_audit_begin("http.fetch");
        sh_json_write_kv_string(&w, "method", method);
        sh_json_write_kv_string(&w, "url", url);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return NULL;
    }

    /* HTTPS requires TLS */
    KlTlsConfig *tls_cfg = (KlTlsConfig *)http_cfg->tls;
    if (parsed.is_https && !tls_cfg)
        return NULL;

    int timeout_ms = http_cfg->timeout_ms > 0 ? http_cfg->timeout_ms
                                                : HL_HTTP_DEFAULT_TIMEOUT_MS;
    size_t max_resp = http_cfg->max_response_size > 0 ? http_cfg->max_response_size
                                                       : (size_t)HL_HTTP_DEFAULT_MAX_RESP;

    /* Build request buffer */
    size_t req_len = 0;
    char *req_buf = build_request(alloc, method, &parsed,
                                   headers, num_headers, body, body_len,
                                   &req_len);
    if (!req_buf)
        return NULL;

    /* DNS resolve (blocking — typically fast, OS-cached) */
    char host_buf[HL_HTTP_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        hl_alloc_free(alloc, req_buf, req_len);
        return NULL;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", parsed.port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host_buf, port_str, &hints, &res);
    if (rc != 0 || !res) {
        hl_alloc_free(alloc, req_buf, req_len);
        return NULL;
    }

    /* Create non-blocking socket */
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        hl_alloc_free(alloc, req_buf, req_len);
        return NULL;
    }

#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        freeaddrinfo(res);
        hl_alloc_free(alloc, req_buf, req_len);
        return NULL;
    }

    /* Non-blocking connect */
    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        hl_alloc_free(alloc, req_buf, req_len);
        return NULL;
    }

    /* Allocate client */
    HlHttpClient *c = hl_alloc_malloc(alloc, sizeof(HlHttpClient));
    if (!c) {
        close(fd);
        hl_alloc_free(alloc, req_buf, req_len);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->fd = fd;
    c->state = (rc == 0) ? HL_HCLIENT_SENDING : HL_HCLIENT_CONNECTING;
    c->server = server;
    c->http_cfg = http_cfg;
    c->alloc = alloc;
    c->tls_cfg = tls_cfg;

    c->request_buf = req_buf;
    c->request_len = req_len;
    c->request_sent = 0;

    /* Copy hostname for TLS SNI */
    memcpy(c->host_buf, host_buf, parsed.host_len + 1);

    /* Create response parser */
    KlAllocator kl_alloc = kl_allocator_default();
    c->parser = hl_http_parser_llhttp(max_resp, &kl_alloc);
    if (!c->parser) {
        close(fd);
        hl_alloc_free(alloc, req_buf, req_len);
        hl_alloc_free(alloc, c, sizeof(HlHttpClient));
        return NULL;
    }

    /* Create async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(server, alloc);
    if (!ctx) {
        c->parser->destroy(c->parser);
        close(fd);
        hl_alloc_free(alloc, req_buf, req_len);
        hl_alloc_free(alloc, c, sizeof(HlHttpClient));
        return NULL;
    }
    ctx->driver = c;
    ctx->free_driver = free_driver;
    ctx->op.deadline_ms = kl_monotonic_ms() + (uint64_t)timeout_ms;
    ctx->op.on_deadline = on_deadline_timeout;
    c->async_ctx = ctx;

    /* Register watcher — WRITE for connect completion (or immediate send) */
    KlEventMask mask = (c->state == HL_HCLIENT_CONNECTING) ? KL_EVENT_WRITE
                                                            : KL_EVENT_WRITE;
    if (kl_watcher_add(server, fd, mask, on_event, c) != 0) {
        c->fd = -1; /* prevent double-close in free_client */
        hl_async_ctx_free(ctx);
        c->parser->destroy(c->parser);
        close(fd);
        hl_alloc_free(alloc, req_buf, req_len);
        hl_alloc_free(alloc, c, sizeof(HlHttpClient));
        return NULL;
    }

    /* Suspend the inbound connection — cont will be set by caller */
    if (kl_async_suspend(server, conn, &ctx->op) < 0) {
        kl_watcher_del(server, fd);
        c->fd = -1;
        hl_async_ctx_free(ctx);
        c->parser->destroy(c->parser);
        close(fd);
        hl_alloc_free(alloc, req_buf, req_len);
        hl_alloc_free(alloc, c, sizeof(HlHttpClient));
        return NULL;
    }

    return c;
}

void hl_async_http_free_client(HlHttpClient *c, int free_resp)
{
    if (!c) return;

    if (c->tls) {
        if (c->fd >= 0)
            c->tls->shutdown(c->tls, c->fd);
        c->tls->destroy(c->tls);
        c->tls = NULL;
    }

    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }

    if (c->request_buf) {
        hl_alloc_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }

    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    if (free_resp)
        hl_cap_http_free(&c->resp);

    hl_alloc_free(c->alloc, c, sizeof(HlHttpClient));
}
