/*
 * agent/request.c - `hull agent request`, `hull agent status`,
 *                   `hull agent errors` - dev-server-adjacent ops.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include <sh_json.h>

#include <errno.h>       /* EINPROGRESS classification in the connect probe */
#include <limits.h>      /* PATH_MAX */
#include <poll.h>        /* poll(): writability wait in the connect probe */
#include <stdint.h>      /* uint8_t / uint16_t (KlSockAddr construction) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>  /* AF_INET / SOCK_STREAM ints for the provider socket() op */

#include <keel/allocator.h>    /* kl_allocator_default */
#include <keel/clock.h>        /* kl_monotonic_ms: request elapsed timing */
#include <keel/handle.h>       /* KlSocketHandle, kl_handle_valid */
#include <keel/http_client.h>  /* kl_http_client_request (sync HTTP client) */
#include <keel/sockaddr.h>     /* kl_sockaddr_from_ipv4 */
#include <keel/socket.h>       /* kl_socket_provider_posix + ops table */

int hl_agent_request(const char *method, const char *path, int port,
                     const char *body, const char **headers, int nhdrs,
                     ShJsonBuf *out)
{
    /* Each caller header is a full "Name: Value" line; Keel's sync client
     * wants split {name, value} pairs. The caller's lines are const and not
     * NUL-terminated at the colon, so copy the split fields into one bounded
     * scratch buffer and point the pair array into it. */
    if (nhdrs < 0)
        nhdrs = 0;
    if (nhdrs > KL_HTTP_CLIENT_MAX_REQ_HEADERS)
        return hl_agent_write_error(out, "too many request headers");
    KlHttpClientHeader kh[KL_HTTP_CLIENT_MAX_REQ_HEADERS];
    char hbuf[8192];
    size_t hoff = 0;
    for (int i = 0; i < nhdrs; i++) {
        const char *hline = headers[i] ? headers[i] : "";
        const char *colon = strchr(hline, ':');
        if (!colon)
            return hl_agent_write_error(out, "malformed request header");
        size_t nlen = (size_t)(colon - hline);
        const char *val = colon + 1;
        while (*val == ' ')
            val++;
        size_t vlen = strlen(val);
        if (hoff + nlen + 1 + vlen + 1 > sizeof(hbuf))
            return hl_agent_write_error(out, "request headers too large");
        char *nm = hbuf + hoff;
        memcpy(nm, hline, nlen);
        nm[nlen] = '\0';
        hoff += nlen + 1;
        char *vl = hbuf + hoff;
        memcpy(vl, val, vlen);
        vl[vlen] = '\0';
        hoff += vlen + 1;
        kh[i].name  = nm;
        kh[i].value = vl;
    }

    char url[PATH_MAX + 64];
    int un = snprintf(url, sizeof(url), "http://127.0.0.1:%d%s",
                      port, path ? path : "/");
    if (un < 0 || (size_t)un >= sizeof(url))
        return hl_agent_write_error(out, "request path too long");

    /* Blocking HTTP/1.1 exchange via Keel's sync client: it owns connect,
     * send, recv, and the status-line + header parse. timeout_ms/max_response
     * preserve the former 5s socket timeout + 1 MB response cap. */
    KlHttpClientConfig cfg = {
        .timeout_ms        = 5000,
        .max_response_size = 1024 * 1024,
    };
    KlAllocator alloc = kl_allocator_default();
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof resp);

    size_t body_len = body ? strlen(body) : 0;
    uint64_t t0 = kl_monotonic_ms();
    int rc = kl_http_client_request(&alloc, &cfg, method, url,
                                    kh, nhdrs, body, body_len, &resp);
    long elapsed_ms = (long)(kl_monotonic_ms() - t0);
    if (rc != 0) {
        char err[160];
        snprintf(err, sizeof(err),
                 "request to 127.0.0.1:%d failed (error %d)",
                 port, (int)resp.error);
        kl_http_client_response_free(&resp);
        return hl_agent_write_error(out, err);
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "status", resp.status);
    sh_json_write_kv_int(&w, "elapsed_ms", (int)elapsed_ms);

    /* Response headers (Keel already parsed them; the status line is excluded). */
    sh_json_write_key(&w, "headers");
    sh_json_write_object_start(&w);
    for (int i = 0; i < resp.num_headers; i++) {
        if (resp.headers[i].name)
            sh_json_write_kv_string(&w, resp.headers[i].name,
                                    resp.headers[i].value ? resp.headers[i].value : "");
    }
    sh_json_write_object_end(&w);

    /* Body (length-aware: binary-safe, matches the former NUL-terminated
     * write for every real text/JSON response). */
    sh_json_write_key(&w, "body");
    sh_json_write_string_n(&w, resp.body ? resp.body : "", resp.body_len);

    sh_json_write_object_end(&w);
    kl_http_client_response_free(&resp);
    return 0;
}

/* ── hl_agent_status ───────────────────────────────────────────────── */

int hl_agent_status(const char *app_dir, int port, ShJsonBuf *out)
{
    /* Try to read .hull/dev.json for port info */
    char dev_json_path[PATH_MAX];
    snprintf(dev_json_path, sizeof(dev_json_path), "%s/.hull/dev.json", app_dir);

    FILE *f = fopen(dev_json_path, "r");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        const char *port_key = strstr(buf, "\"port\":");
        if (port_key) {
            int p = (int)strtol(port_key + 7, NULL, 10);
            if (p > 0) port = p;
        }
    }

    /* Pure TCP-connect liveness probe through Keel's socket provider (no app
     * route or middleware invoked): a nonblocking connect + one writability
     * wait + SO_ERROR, mirroring cap/db_transport.c's connect attempt. */
    int running = 0;
    const KlSocketProvider *sp = kl_socket_provider_posix();
    if (sp && sp->ops && sp->ops->socket && sp->ops->connect &&
        sp->ops->close && sp->ops->set_nonblocking && sp->ops->get_so_error) {
        const KlSocketOps *ops = sp->ops;
        void *sctx = sp->context;
        KlSockAddr addr;
        const uint8_t loopback[4] = { 127, 0, 0, 1 };
        if (kl_sockaddr_from_ipv4(&addr, loopback, (uint16_t)port) == 0) {
            KlSocketHandle fd = ops->socket(sctx, AF_INET, SOCK_STREAM, 0);
            if (kl_handle_valid(fd)) {
                if (ops->set_nonblocking(sctx, fd) == 0) {
                    int cr = ops->connect(sctx, fd, &addr);
                    if (cr == 0) {
                        running = 1;                  /* loopback often connects at once */
                    } else if (errno == EINPROGRESS) {
                        struct pollfd pfd = { .fd = (int)fd, .events = POLLOUT, .revents = 0 };
                        if (poll(&pfd, 1, 1000) > 0) {
                            int soerr = 0;
                            if (ops->get_so_error(sctx, fd, &soerr) == 0 && soerr == 0)
                                running = 1;
                        }
                    }
                }
                ops->close(sctx, fd);
            }
        }
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "running", running != 0);
    sh_json_write_kv_int(&w, "port", port);
    sh_json_write_object_end(&w);
    return 0;
}

/* ── hl_agent_errors ───────────────────────────────────────────────── */

int hl_agent_errors(const char *app_dir, ShJsonBuf *out)
{
    char err_path[PATH_MAX];
    snprintf(err_path, sizeof(err_path), "%s/.hull/last_error.json", app_dir);

    FILE *f = fopen(err_path, "r");
    if (!f) {
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, out);
        sh_json_write_object_start(&w);
        sh_json_write_key(&w, "errors");
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
        sh_json_write_object_end(&w);
        return 0;
    }

    /* Read and pass through the JSON file */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long flen = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    if (flen <= 0 || flen > 1024 * 1024) {
        fclose(f);
        return -1;
    }
    char *buf = malloc((size_t)flen + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)flen, f);
    /* L3: capture read error before fclose. */
    int read_err = ferror(f);
    fclose(f);
    if (read_err || n != (size_t)flen) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    /* Pass through raw JSON - write directly to buffer */
    sh_json_buf_write(out, buf, n);
    free(buf);
    return 0;
}
