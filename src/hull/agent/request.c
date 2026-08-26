/*
 * agent/request.c - `hull agent request`, `hull agent status`,
 *                   `hull agent errors` - dev-server-adjacent ops.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include <sh_json.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

int hl_agent_request(const char *method, const char *path, int port,
                     const char *body, const char **headers, int nhdrs,
                     ShJsonBuf *out)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return hl_agent_write_error(out, "cannot create socket");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct timeval start, end;
    gettimeofday(&start, NULL);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        char err[128];
        snprintf(err, sizeof(err), "cannot connect to 127.0.0.1:%d", port);
        return hl_agent_write_error(out, err);
    }

    /* Build HTTP request.
     *
     * Each `snprintf` returns the would-be length even on truncation, so
     * naively accumulating `req_len += snprintf(...)` past `sizeof(req_buf)`
     * causes `sizeof(req_buf) - req_len` to wrap to a huge size_t on the
     * next call, writing past the end of `req_buf`. Check truncation after
     * every call and bail with a clear error (M-1). */
    size_t body_len = body ? strlen(body) : 0;
    char req_buf[8192];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Connection: close\r\n",
        method, path, port);
    if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
        close(sock);
        return hl_agent_write_error(out, "request too large");
    }

    #define APPEND_FMT(...) do { \
        int _n = snprintf(req_buf + req_len, \
                          sizeof(req_buf) - (size_t)req_len, __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= sizeof(req_buf) - (size_t)req_len) { \
            close(sock); \
            return hl_agent_write_error(out, "request too large"); \
        } \
        req_len += _n; \
    } while (0)

    if (body_len > 0)
        APPEND_FMT("Content-Length: %zu\r\n", body_len);

    for (int i = 0; i < nhdrs; i++)
        APPEND_FMT("%s\r\n", headers[i]);

    APPEND_FMT("\r\n");

    #undef APPEND_FMT

    ssize_t sent = send(sock, req_buf, (size_t)req_len, 0);
    if (sent < 0) {
        close(sock);
        return hl_agent_write_error(out, "send failed");
    }

    if (body && body_len > 0) {
        sent = send(sock, body, body_len, 0);
        if (sent < 0) {
            close(sock);
            return hl_agent_write_error(out, "send body failed");
        }
    }

    /* Read response */
    char *resp_buf = malloc(1024 * 1024);
    if (!resp_buf) {
        close(sock);
        return hl_agent_write_error(out, "allocation failed");
    }

    size_t resp_len = 0;
    for (;;) {
        ssize_t n = recv(sock, resp_buf + resp_len,
                         1024 * 1024 - resp_len - 1, 0);
        if (n <= 0) break;
        resp_len += (size_t)n;
        if (resp_len >= 1024 * 1024 - 1) break;
    }
    resp_buf[resp_len] = '\0';
    close(sock);

    gettimeofday(&end, NULL);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_usec - start.tv_usec) / 1000;

    /* Parse status line */
    int status = 0;
    const char *header_end = strstr(resp_buf, "\r\n\r\n");
    if (!header_end) {
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, out);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "error", "malformed response");
        sh_json_write_kv_string(&w, "raw", resp_buf);
        sh_json_write_object_end(&w);
        free(resp_buf);
        return -1;
    }

    if (resp_len > 12) {
        char *sp = memchr(resp_buf, ' ', 12);
        if (sp) status = (int)strtol(sp + 1, NULL, 10);
    }

    const char *resp_body = header_end + 4;
    size_t resp_body_len = resp_len - (size_t)(resp_body - resp_buf);

    /* Write JSON output */
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "status", status);
    sh_json_write_kv_int(&w, "elapsed_ms", elapsed_ms);

    /* Response headers */
    sh_json_write_key(&w, "headers");
    sh_json_write_object_start(&w);

    const char *line = resp_buf;
    const char *first_crlf = strstr(line, "\r\n");
    if (first_crlf) line = first_crlf + 2;

    while (line < header_end) {
        const char *next = strstr(line, "\r\n");
        if (!next || next == line) break;

        const char *colon = memchr(line, ':', (size_t)(next - line));
        if (colon) {
            size_t key_len = (size_t)(colon - line);
            char key_buf[256];
            if (key_len >= sizeof(key_buf)) key_len = sizeof(key_buf) - 1;
            memcpy(key_buf, line, key_len);
            key_buf[key_len] = '\0';

            const char *val = colon + 1;
            while (val < next && *val == ' ') val++;
            size_t val_len = (size_t)(next - val);
            char val_buf[4096];
            if (val_len >= sizeof(val_buf)) val_len = sizeof(val_buf) - 1;
            memcpy(val_buf, val, val_len);
            val_buf[val_len] = '\0';

            sh_json_write_kv_string(&w, key_buf, val_buf);
        }
        line = next + 2;
    }

    sh_json_write_object_end(&w);

    /* Body */
    sh_json_write_kv_string(&w, "body", resp_body);
    (void)resp_body_len;

    sh_json_write_object_end(&w);
    free(resp_buf);
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

    /* Probe with socket connect */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int connected = 0;
    if (sock >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        connected = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        close(sock);
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "running", connected != 0);
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
