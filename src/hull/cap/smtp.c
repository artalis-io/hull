/*
 * smtp.c - SMTP email capability implementation
 *
 * Synchronous SMTP client with STARTTLS, AUTH PLAIN, and host allowlist
 * enforcement. Hull owns SMTP policy and protocol here: host authorization,
 * message validation, the reply-driven conversation, EHLO / STARTTLS /
 * AUTH PLAIN / envelope commands, message formatting + dot-stuffing, the stable
 * error tokens, and audit records.
 *
 * The BYTE TRANSPORT (name resolution, connect, ordered reads/writes, TLS
 * attachment, close) is owned by cap/smtp_transport.c, which composes Keel v3's
 * public primitives (KlConnectOp + KlStream + KlTls) on a private, operation-
 * local event context that this synchronous entry point pumps to a terminal
 * state (docs/smtp_keel_client_design.md; Slice 2b). No raw socket, fcntl,
 * poll, getaddrinfo, read, write, or descriptor-close logic lives in this file.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp.h"
#include "hull/cap/smtp_transport.h"
#include "hull/cap/audit.h"
#include "hull/host_match.h"
#include "hull/limits/core.h"

#include <keel/allocator.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "log.h"

/* ── CRLF injection guard ────────────────────────────────────────── */

static int has_crlf(const char *s)
{
    if (!s) return 0;
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n')
            return 1;
    }
    return 0;
}

/* ── Base64 encoding ─────────────────────────────────────────────── */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hl_smtp_base64_encode(const unsigned char *src, int src_len,
                          char *dst, int dst_len)
{
    if (!src || !dst || src_len < 0 || dst_len < 0)
        return -1;

    int needed = 4 * ((src_len + 2) / 3) + 1;
    if (dst_len < needed)
        return -1;

    int out = 0;
    int i;
    for (i = 0; i + 2 < src_len; i += 3) {
        unsigned int v = ((unsigned int)src[i] << 16) |
                         ((unsigned int)src[i+1] << 8) |
                         (unsigned int)src[i+2];
        dst[out++] = b64_table[(v >> 18) & 0x3F];
        dst[out++] = b64_table[(v >> 12) & 0x3F];
        dst[out++] = b64_table[(v >> 6) & 0x3F];
        dst[out++] = b64_table[v & 0x3F];
    }

    if (i < src_len) {
        unsigned int v = (unsigned int)src[i] << 16;
        if (i + 1 < src_len)
            v |= (unsigned int)src[i+1] << 8;

        dst[out++] = b64_table[(v >> 18) & 0x3F];
        dst[out++] = b64_table[(v >> 12) & 0x3F];

        if (i + 1 < src_len)
            dst[out++] = b64_table[(v >> 6) & 0x3F];
        else
            dst[out++] = '=';
        dst[out++] = '=';
    }

    dst[out] = '\0';
    return out;
}

/* ── Host allowlist check ────────────────────────────────────────── */

int hl_smtp_check_host(const HlSmtpConfig *cfg, const char *host)
{
    if (!cfg || !host)
        return -1;
    /* Same manifest.hosts matcher as http.fetch: exact + glob + CIDR + $VAR. */
    return hl_host_match_any_env(cfg->allowed_hosts, cfg->host_count, host)
           ? 0 : -1;
}

/* ── SMTP reply-code parser (also used by the incremental parser in the
 * transport, cap/smtp_transport.c) ───────────────────────────────── */

int hl_smtp_parse_response(const char *line, int len)
{
    if (!line || len < 3)
        return -1;

    /* First 3 chars must be digits */
    for (int i = 0; i < 3; i++) {
        if (line[i] < '0' || line[i] > '9')
            return -1;
    }

    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

/* ── Command / response helpers over the Keel-primitive transport ─── */

/**
 * Write @p cmd, then read one reply and check @p expected_code.
 * Returns the response code, or -1 on write / read / mismatch (mirrors the
 * former smtp_send_command semantics exactly).
 */
static int smtp_command(HlSmtpTransport *t, const char *cmd,
                        int expected_code, int timeout_ms)
{
    if (hl_smtp_transport_write(t, cmd, strlen(cmd), timeout_ms) != 0)
        return -1;

    char resp[HL_SMTP_RECV_BUF_SIZE];
    int code = hl_smtp_transport_read_reply(t, resp, (int)sizeof(resp), timeout_ms);

    if (expected_code > 0 && code != expected_code) {
        log_warn("smtp: expected %d, got %d for command '%.20s'",
                 expected_code, code, cmd);
        return -1;
    }
    return code;
}

/* ── RFC 5322 message formatting ─────────────────────────────────── */

int hl_smtp_format_message(const HlSmtpMessage *msg, char *buf, int size)
{
    if (!msg || !buf || size <= 0)
        return -1;

    /* Format Date header per RFC 5322 */
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm);

    /* Build headers */
    int off = snprintf(buf, (size_t)size,
                       "Date: %s\r\n"
                       "From: %s\r\n"
                       "To: %s\r\n",
                       date, msg->from, msg->to);

    if (off < 0 || off >= size)
        return -1;

    /* CC header */
    if (msg->cc && msg->cc_count > 0) {
        int n = snprintf(buf + off, (size_t)(size - off), "Cc: ");
        if (n < 0 || off + n >= size) return -1;
        off += n;

        for (int i = 0; i < msg->cc_count; i++) {
            if (i > 0) {
                n = snprintf(buf + off, (size_t)(size - off), ", ");
                if (n < 0 || off + n >= size) return -1;
                off += n;
            }
            n = snprintf(buf + off, (size_t)(size - off), "%s", msg->cc[i]);
            if (n < 0 || off + n >= size) return -1;
            off += n;
        }
        n = snprintf(buf + off, (size_t)(size - off), "\r\n");
        if (n < 0 || off + n >= size) return -1;
        off += n;
    }

    /* Reply-To header */
    if (msg->reply_to) {
        int n = snprintf(buf + off, (size_t)(size - off),
                         "Reply-To: %s\r\n", msg->reply_to);
        if (n < 0 || off + n >= size) return -1;
        off += n;
    }

    /* Subject + MIME headers */
    const char *ct = msg->content_type ? msg->content_type : "text/plain";
    int n = snprintf(buf + off, (size_t)(size - off),
                     "Subject: %s\r\n"
                     "MIME-Version: 1.0\r\n"
                     "Content-Type: %s; charset=utf-8\r\n"
                     "\r\n",
                     msg->subject, ct);
    if (n < 0 || off + n >= size) return -1;
    off += n;

    /* Body with dot-stuffing:
     * Any line starting with '.' must be doubled per RFC 5321 §4.5.2 */
    const char *p = msg->body;
    int at_line_start = 1;

    while (*p) {
        if (at_line_start && *p == '.') {
            if (off + 1 >= size) return -1;
            buf[off++] = '.';
        }

        if (off + 1 >= size) return -1;
        buf[off++] = *p;

        at_line_start = (*p == '\n');
        p++;
    }

    /* Ensure body ends with \r\n */
    if (off >= 2 && (buf[off-2] != '\r' || buf[off-1] != '\n')) {
        if (off + 2 >= size) return -1;
        buf[off++] = '\r';
        buf[off++] = '\n';
    } else if (off < 2) {
        if (off + 2 >= size) return -1;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    buf[off] = '\0';
    return off;
}

/* ── Validate message fields ─────────────────────────────────────── */

static int smtp_validate_message(const HlSmtpMessage *msg)
{
    if (!msg->host || !msg->from || !msg->to ||
        !msg->subject || !msg->body)
        return -1;

    /* CRLF injection guard on all header-injectable fields */
    if (has_crlf(msg->host) || has_crlf(msg->from) ||
        has_crlf(msg->to) || has_crlf(msg->subject) ||
        has_crlf(msg->reply_to))
        return -1;

    /* Check CC recipients for CRLF */
    if (msg->cc) {
        for (int i = 0; i < msg->cc_count; i++) {
            if (has_crlf(msg->cc[i]))
                return -1;
        }
    }

    /* Check port is valid */
    if (msg->port < 1 || msg->port > 65535)
        return -1;

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

int hl_cap_smtp_send(const HlSmtpConfig *cfg, const HlSmtpMessage *msg,
                     const char **err_msg)
{
    if (!cfg || !msg) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }

    /* Validate message fields */
    if (smtp_validate_message(msg) != 0) {
        if (err_msg) *err_msg = "validation_failed";
        return -1;
    }

    /* Check host allowlist */
    if (hl_smtp_check_host(cfg, msg->host) != 0) {
        log_warn("smtp: host '%s' not in allowlist", msg->host);
        if (err_msg) *err_msg = "host_not_allowed";
        ShJsonWriter w = hl_audit_begin("smtp.send");
        sh_json_write_kv_string(&w, "host", msg->host);
        sh_json_write_kv_string(&w, "from", msg->from);
        sh_json_write_kv_string(&w, "to", msg->to);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return -1;
    }

    int timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms
                                         : HL_SMTP_DEFAULT_TIMEOUT_MS;

    /* TLS config (for STARTTLS or implicit TLS) */
    void *tls_cfg = cfg->tls;   /* opaque KlTlsConfig *, passed to the helper */

    /* Connect to SMTP server (resolve + Happy-Eyeballs connect over the
     * Keel-primitive transport; NULL == resolve / connect / deadline failure). */
    HlSmtpTransport *t = hl_smtp_transport_connect(msg->host, msg->port, timeout_ms);
    if (!t) {
        log_warn("smtp: connect to %s:%d failed", msg->host, msg->port);
        if (err_msg) *err_msg = "connect_failed";
        return -1;
    }

    int ret = -1;
    char resp[HL_SMTP_RECV_BUF_SIZE];
    char cmd[HL_SMTP_SEND_BUF_SIZE];

    /* Implicit TLS (port 465) - handshake before any SMTP commands */
    if (msg->use_tls == 2) {
        if (!tls_cfg) {
            log_warn("smtp: implicit TLS requested but no TLS config");
            if (err_msg) *err_msg = "tls_config_missing";
            goto cleanup;
        }
        if (hl_smtp_transport_implicit_tls(t, msg->host, tls_cfg, timeout_ms) != 0) {
            log_warn("smtp: implicit TLS handshake failed");
            if (err_msg) *err_msg = "tls_handshake_failed";
            goto cleanup;
        }
    }

    /* Read 220 greeting */
    int code = hl_smtp_transport_read_reply(t, resp, (int)sizeof(resp), timeout_ms);
    if (code != 220) {
        log_warn("smtp: expected 220 greeting, got %d", code);
        if (err_msg) *err_msg = "greeting_failed";
        goto cleanup;
    }

    /* EHLO */
    snprintf(cmd, sizeof(cmd), "EHLO localhost\r\n");
    code = smtp_command(t, cmd, 250, timeout_ms);
    if (code < 0) {
        if (err_msg) *err_msg = "ehlo_failed";
        goto cleanup;
    }

    /* STARTTLS (if requested and not already implicit TLS) */
    if (msg->use_tls == 1 && !hl_smtp_transport_tls_active(t)) {
        if (!tls_cfg) {
            log_warn("smtp: STARTTLS requested but no TLS config");
            if (err_msg) *err_msg = "tls_config_missing";
            goto cleanup;
        }

        code = smtp_command(t, "STARTTLS\r\n", 220, timeout_ms);
        if (code < 0) {
            log_warn("smtp: STARTTLS rejected");
            if (err_msg) *err_msg = "starttls_rejected";
            goto cleanup;
        }

        /* In-place upgrade. NO plaintext fallback on failure. */
        if (hl_smtp_transport_starttls(t, msg->host, tls_cfg, timeout_ms) != 0) {
            log_warn("smtp: STARTTLS handshake failed");
            if (err_msg) *err_msg = "tls_handshake_failed";
            goto cleanup;
        }

        /* Re-EHLO after TLS upgrade */
        snprintf(cmd, sizeof(cmd), "EHLO localhost\r\n");
        code = smtp_command(t, cmd, 250, timeout_ms);
        if (code < 0) {
            if (err_msg) *err_msg = "ehlo_failed";
            goto cleanup;
        }
    }

    /* AUTH PLAIN (if credentials provided) */
    if (msg->username && msg->password) {
        /* Refuse to send credentials over a plaintext connection */
        if (!hl_smtp_transport_tls_active(t)) {
            log_warn("smtp: AUTH PLAIN requires TLS - refusing to send "
                     "credentials in plaintext (set use_tls=1 or 2)");
            if (err_msg) *err_msg = "auth_requires_tls";
            goto cleanup;
        }

        /* AUTH PLAIN: base64(\0username\0password) */
        size_t ulen = strlen(msg->username);
        size_t plen = strlen(msg->password);
        size_t plain_len = 1 + ulen + 1 + plen;

        if (plain_len > 1024) {
            log_warn("smtp: AUTH PLAIN credentials too long");
            if (err_msg) *err_msg = "auth_credentials_too_long";
            goto cleanup;
        }

        unsigned char plain[1026];
        plain[0] = '\0';
        memcpy(plain + 1, msg->username, ulen);
        plain[1 + ulen] = '\0';
        memcpy(plain + 2 + ulen, msg->password, plen);

        char b64[1400];
        int b64_len = hl_smtp_base64_encode(plain, (int)plain_len,
                                            b64, (int)sizeof(b64));
        if (b64_len < 0) {
            log_warn("smtp: AUTH PLAIN base64 encode failed");
            if (err_msg) *err_msg = "auth_encode_failed";
            goto cleanup;
        }

        int n = snprintf(cmd, sizeof(cmd), "AUTH PLAIN %s\r\n", b64);
        if (n < 0 || (size_t)n >= sizeof(cmd)) {
            log_warn("smtp: AUTH PLAIN credentials too large for send buffer");
            if (err_msg) *err_msg = "auth_encode_failed";
            goto cleanup;
        }
        code = smtp_command(t, cmd, 235, timeout_ms);
        if (code < 0) {
            log_warn("smtp: AUTH PLAIN failed");
            if (err_msg) *err_msg = "auth_failed";
            goto cleanup;
        }
    }

    /* MAIL FROM */
    snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", msg->from);
    code = smtp_command(t, cmd, 250, timeout_ms);
    if (code < 0) {
        if (err_msg) *err_msg = "mail_from_failed";
        goto cleanup;
    }

    /* RCPT TO - primary recipient */
    snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", msg->to);
    code = smtp_command(t, cmd, 250, timeout_ms);
    if (code < 0) {
        if (err_msg) *err_msg = "rcpt_to_failed";
        goto cleanup;
    }

    /* RCPT TO - CC recipients */
    if (msg->cc) {
        for (int i = 0; i < msg->cc_count; i++) {
            snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", msg->cc[i]);
            code = smtp_command(t, cmd, 250, timeout_ms);
            if (code < 0) {
                if (err_msg) *err_msg = "rcpt_to_failed";
                goto cleanup;
            }
        }
    }

    /* DATA */
    code = smtp_command(t, "DATA\r\n", 354, timeout_ms);
    if (code < 0) {
        if (err_msg) *err_msg = "data_failed";
        goto cleanup;
    }

    /* Format and send the message */
    {
        /* Allocate message buffer (body + headers overhead) */
        size_t body_len = strlen(msg->body);
        size_t msg_size = body_len + 4096;  /* headers + dot-stuffing headroom */
        if (msg_size > (size_t)HL_SMTP_MAX_MSG_SIZE)
            msg_size = (size_t)HL_SMTP_MAX_MSG_SIZE;

        KlAllocator alloc = kl_allocator_default();
        char *msg_buf = kl_malloc(&alloc, msg_size);
        if (!msg_buf) {
            log_warn("smtp: message buffer allocation failed");
            if (err_msg) *err_msg = "alloc_failed";
            goto cleanup;
        }

        int msg_len = hl_smtp_format_message(msg, msg_buf, (int)msg_size);
        if (msg_len < 0) {
            kl_free(&alloc, msg_buf, msg_size);
            log_warn("smtp: message formatting failed");
            if (err_msg) *err_msg = "format_failed";
            goto cleanup;
        }

        /* Send formatted message */
        if (hl_smtp_transport_write(t, msg_buf, (size_t)msg_len, timeout_ms) != 0) {
            kl_free(&alloc, msg_buf, msg_size);
            if (err_msg) *err_msg = "send_failed";
            goto cleanup;
        }

        kl_free(&alloc, msg_buf, msg_size);
    }

    /* End DATA with \r\n.\r\n */
    code = smtp_command(t, ".\r\n", 250, timeout_ms);
    if (code < 0) {
        if (err_msg) *err_msg = "data_end_failed";
        goto cleanup;
    }

    /* QUIT */
    smtp_command(t, "QUIT\r\n", 221, timeout_ms);

    ret = 0;

cleanup:
    hl_smtp_transport_shutdown(t);   /* best-effort close_notify + graceful close */
    hl_smtp_transport_free(t);

    {
        ShJsonWriter w = hl_audit_begin("smtp.send");
        sh_json_write_kv_string(&w, "host", msg->host);
        sh_json_write_kv_string(&w, "from", msg->from);
        sh_json_write_kv_string(&w, "to", msg->to);
        sh_json_write_kv_string(&w, "subject", msg->subject);
        sh_json_write_kv_int(&w, "result", ret);
        hl_audit_end(&w);
    }
    return ret;
}
