/*
 * http_echo_server.c — Minimal Keel HTTP server that echoes request details
 *
 * Used as a test fixture for E2E HTTP client tests.
 * Echoes method, path, headers, and body back as JSON.
 *
 * Usage: ./http_echo_server <port>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Build a JSON echo response containing method, path, selected headers,
 * and body (if any).
 */
static void echo_handler(KlRequest *req, KlResponse *res, void *ctx)
{
    (void)ctx;

    /* Get body from buffer reader (if present) */
    const char *body_data = NULL;
    size_t body_len = 0;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (br && br->len > 0) {
        body_data = br->data;
        body_len = br->len;
    }

    /* Build JSON response manually.
     * We use a static buffer — safe because Keel is single-threaded
     * and the buffer outlives writev. */
    static char json[8192];
    int off = 0;

    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    "{\"method\":\"%.*s\",\"path\":\"%.*s\"",
                    (int)req->method_len, req->method,
                    (int)req->path_len, req->path);

    /* Echo selected headers as "headers" object */
    off += snprintf(json + off, sizeof(json) - (size_t)off, ",\"headers\":{");
    int first = 1;
    for (int i = 0; i < req->num_headers; i++) {
        /* Skip internal headers (Host, Connection, Content-Length) for cleaner output */
        if (req->headers[i].name_len == 4 &&
            strncasecmp(req->headers[i].name, "Host", 4) == 0)
            continue;
        if (req->headers[i].name_len == 10 &&
            strncasecmp(req->headers[i].name, "Connection", 10) == 0)
            continue;
        if (req->headers[i].name_len == 14 &&
            strncasecmp(req->headers[i].name, "Content-Length", 14) == 0)
            continue;

        if (!first)
            off += snprintf(json + off, sizeof(json) - (size_t)off, ",");
        first = 0;

        off += snprintf(json + off, sizeof(json) - (size_t)off,
                        "\"%.*s\":\"%.*s\"",
                        (int)req->headers[i].name_len, req->headers[i].name,
                        (int)req->headers[i].value_len, req->headers[i].value);
    }
    off += snprintf(json + off, sizeof(json) - (size_t)off, "}");

    /* Echo body if present */
    if (body_data && body_len > 0) {
        off += snprintf(json + off, sizeof(json) - (size_t)off,
                        ",\"body\":\"%.*s\"", (int)body_len, body_data);
    }

    off += snprintf(json + off, sizeof(json) - (size_t)off, "}");

    if (off < 0 || (size_t)off >= sizeof(json))
        off = 0;

    kl_response_json(res, 200, json, (size_t)off);
}

/* HEAD handler — same as echo but Keel handles HEAD automatically
 * (sends headers but no body). We just set the status. */
static void head_handler(KlRequest *req, KlResponse *res, void *ctx)
{
    (void)req; (void)ctx;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/json");
    kl_response_header(res, "X-Echo-Method", "HEAD");
    kl_response_body(res, "{}", 2);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    char *end;
    long port = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }

    KlServer s;
    KlConfig cfg = {.port = (int)port, .bind_addr = "127.0.0.1"};
    if (kl_server_init(&s, &cfg) < 0)
        return 1;

    /* Register echo handlers for all standard HTTP methods */
    kl_server_route(&s, "GET",     "/echo", echo_handler, NULL, NULL);
    kl_server_route(&s, "POST",    "/echo", echo_handler, NULL, kl_body_reader_buffer);
    kl_server_route(&s, "PUT",     "/echo", echo_handler, NULL, kl_body_reader_buffer);
    kl_server_route(&s, "PATCH",   "/echo", echo_handler, NULL, kl_body_reader_buffer);
    kl_server_route(&s, "DELETE",  "/echo", echo_handler, NULL, NULL);
    kl_server_route(&s, "HEAD",    "/echo", head_handler, NULL, NULL);
    kl_server_route(&s, "OPTIONS", "/echo", echo_handler, NULL, NULL);

    /* Health check for readiness probe */
    kl_server_route(&s, "GET", "/health", echo_handler, NULL, NULL);

    /* Print "READY" to stderr so the test script knows we're listening */
    fprintf(stderr, "echo server listening on 127.0.0.1:%ld\n", port);
    fflush(stderr);

    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
