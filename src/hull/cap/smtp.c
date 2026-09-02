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
 * state (docs/smtp_keel_client_design.md). No raw socket, fcntl,
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

/* ── Credential scrubbing ────────────────────────────────────────────
 * hull_secure_zero is static-in-crypto.c, so keep a local volatile-memset
 * loop here for scrubbing AUTH material off the stack. `volatile` on the
 * pointer stops the compiler from eliding the write as a dead store. */
static void smtp_secure_zero(void *p, size_t n)
{
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--)
        *q++ = 0;
}

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

    /* CRLF injection guard on all header-injectable fields. content_type is
     * optional but flows into the Content-Type MIME header, so it must be
     * checked too (has_crlf treats NULL as clean). */
    if (has_crlf(msg->host) || has_crlf(msg->from) ||
        has_crlf(msg->to) || has_crlf(msg->subject) ||
        has_crlf(msg->reply_to) || has_crlf(msg->content_type))
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

#ifdef HL_SMTP_TEST_HOOKS
/* ── Test-only seam (compiled in ONLY under -DHL_SMTP_TEST_HOOKS) ─────
 * Absent and zero-overhead in production: the whole block is preprocessed out
 * of the normal build. The unit test sets these hooks to capture the built
 * AUTH PLAIN command (proving the live secret was present) and to assert the
 * scrub cleanup ran with every secret buffer zeroed on BOTH the success and
 * failure exits of smtp_do_auth_plain, without needing a live SMTP server.
 *
 * smtp_test_auth_send, when non-NULL, REPLACES the smtp_command() call inside
 * smtp_do_auth_plain (it receives the constructed command + expected code and
 * returns a reply code). smtp_test_auth_probe, when non-NULL, is called at the
 * very end of cleanup, AFTER the three smtp_secure_zero calls, with the three
 * buffers so the test can assert they are fully zeroed. */
int  (*smtp_test_auth_send)(HlSmtpTransport *t, const char *cmd,
                            int expected, int timeout);
void (*smtp_test_auth_probe)(const unsigned char *plain, size_t pn,
                             const char *b64, size_t bn,
                             const char *cmd, size_t cn);
#endif

/* ── AUTH PLAIN (credential-scrubbing helper) ────────────────────────
 * Build base64(\0user\0pass), issue "AUTH PLAIN <b64>\r\n", and check the 235
 * reply. All secret-bearing stack buffers (the raw AUTH PLAIN bytes, the base64
 * text, AND the command line) are scrubbed on EVERY exit via one cleanup path.
 * @p username / @p password are borrowed (caller-owned) and never scrubbed.
 * Returns 0 on success, -1 on any failure; on failure *err_msg is set to a
 * stable token. */
static int smtp_do_auth_plain(HlSmtpTransport *t, const char *username,
                              const char *password, int timeout_ms,
                              const char **err_msg)
{
    int ret = -1;
    unsigned char plain[1026];
    char          b64[1400];
    char          cmd[HL_SMTP_SEND_BUF_SIZE];

    /* AUTH PLAIN: base64(\0username\0password) */
    size_t ulen = strlen(username);
    size_t plen = strlen(password);
    size_t plain_len = 1 + ulen + 1 + plen;

    if (plain_len > 1024) {
        log_warn("smtp: AUTH PLAIN credentials too long");
        if (err_msg) *err_msg = "auth_credentials_too_long";
        goto cleanup;
    }

    plain[0] = '\0';
    memcpy(plain + 1, username, ulen);
    plain[1 + ulen] = '\0';
    memcpy(plain + 2 + ulen, password, plen);

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

    int code;
#ifdef HL_SMTP_TEST_HOOKS
    if (smtp_test_auth_send)
        code = smtp_test_auth_send(t, cmd, 235, timeout_ms);
    else
#endif
    code = smtp_command(t, cmd, 235, timeout_ms);
    if (code < 0) {
        log_warn("smtp: AUTH PLAIN failed");
        if (err_msg) *err_msg = "auth_failed";
        goto cleanup;
    }

    ret = 0;

cleanup:
    /* Scrub every secret-bearing buffer on success AND on each failure path. */
    smtp_secure_zero(plain, sizeof plain);
    smtp_secure_zero(b64,   sizeof b64);
    smtp_secure_zero(cmd,   sizeof cmd);
#ifdef HL_SMTP_TEST_HOOKS
    if (smtp_test_auth_probe)
        smtp_test_auth_probe(plain, sizeof plain, b64, sizeof b64,
                             cmd, sizeof cmd);
#endif
    return ret;
}

/* ── Public API ──────────────────────────────────────────────────── */

/* Execute-phase: runs the SMTP conversation to terminal and reports a stable
 * outcome in *out, emitting NO audit and touching NO runtime objects, so it is
 * safe on a worker thread. Authorization + audit + result construction are done
 * by hl_cap_smtp_send (the completion phase in model 2). */
int hl_smtp_execute(const HlSmtpMessage *msg, void *tls_cfg, int timeout_ms,
                    HlSmtpResult *out)
{
    out->rc = -1;
    out->token = NULL;
    out->teardown_leaked = 0;
    /* The interior conversation writes its stable token through `err_msg`; alias
     * it onto out->token so the body below stays byte-identical to the prior
     * in-place send. */
    const char **err_msg = &out->token;

    /* Declared before the connect + before the `cleanup` label so the
     * connect-failure path can route through the one cleanup + audit block. */
    int  ret = -1;
    int  teardown_leaked = 0;
    char resp[HL_SMTP_RECV_BUF_SIZE];
    char cmd[HL_SMTP_SEND_BUF_SIZE];
    (void)resp; (void)cmd;   /* used only past a successful connect */

    /* Connect to SMTP server (resolve + Happy-Eyeballs connect over the
     * Keel-primitive transport; NULL == resolve / connect / deadline failure).
     * out_teardown_leaked surfaces a connect-path teardown leak into the audit. */
    HlSmtpTransport *t = hl_smtp_transport_connect(msg->host, msg->port,
                                                   timeout_ms, &teardown_leaked);
    if (!t) {
        log_warn("smtp: connect to %s:%d failed", msg->host, msg->port);
        if (err_msg) *err_msg = "connect_failed";
        goto cleanup;   /* unified cleanup + audit (t is NULL, all NULL-safe) */
    }

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

        /* AUTH PLAIN construction + send + credential scrub live in one helper
         * with a single cleanup exit (fix 9): the raw AUTH bytes, the base64
         * text, and the command line are all scrubbed on every path. */
        if (smtp_do_auth_plain(t, msg->username, msg->password,
                               timeout_ms, err_msg) != 0) {
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
    /* Confirmed teardown (the fail-closed detachment discipline), on whatever
     * thread runs the execute-phase (the worker in model 2). t is NULL on the
     * connect-failure
     * path (shutdown/free are NULL-safe). Graceful close only on success; a
     * failed send goes straight to the abortive free. A -1 from free means the
     * op/stream would not detach, so the transport was intentionally leaked
     * rather than freed into a use-after-free; report it in the result. */
    if (ret == 0)
        hl_smtp_transport_shutdown(t);
    teardown_leaked = teardown_leaked || (hl_smtp_transport_free(t) != 0);
    if (teardown_leaked)
        log_error("smtp: transport teardown failed (op/stream would not "
                  "detach); leaked transport resources for host '%s'", msg->host);

    out->rc = ret;
    out->teardown_leaked = teardown_leaked;
    return ret;
}

/* Emit the single "denied" audit record for a host-allowlist rejection. Shared by
 * the sync path (hl_cap_smtp_send) and the model-2 async binding so authorization
 * is audited exactly once, on the submit side. */
void hl_smtp_audit_denied(const HlSmtpMessage *msg)
{
    ShJsonWriter w = hl_audit_begin("smtp.send");
    sh_json_write_kv_string(&w, "host", msg->host);
    sh_json_write_kv_string(&w, "from", msg->from);
    sh_json_write_kv_string(&w, "to", msg->to);
    sh_json_write_kv_string(&w, "result", "denied");
    hl_audit_end(&w);
}

/* Emit the single completion audit record. The FROZEN metadata (section 3):
 * @p schedule (a scheduling-failure tag) and @p terminal (a cancel/deadline tag)
 * are emitted ONLY when non-NULL; r->teardown_leaked adds teardown:leaked. Shared
 * by the sync path, the async scheduling-failure path, and the async completion
 * path so every send is audited exactly once. */
void hl_smtp_audit_complete(const HlSmtpMessage *msg, const HlSmtpResult *r,
                            const char *schedule, const char *terminal)
{
    ShJsonWriter w = hl_audit_begin("smtp.send");
    sh_json_write_kv_string(&w, "host", msg->host);
    sh_json_write_kv_string(&w, "from", msg->from);
    sh_json_write_kv_string(&w, "to", msg->to);
    sh_json_write_kv_string(&w, "subject", msg->subject);
    sh_json_write_kv_int(&w, "result", r->rc);
    if (schedule)
        sh_json_write_kv_string(&w, "schedule", schedule);
    if (terminal)
        sh_json_write_kv_string(&w, "terminal", terminal);
    if (r->teardown_leaked)
        sh_json_write_kv_string(&w, "teardown", "leaked");
    hl_audit_end(&w);
}

int hl_cap_smtp_send(const HlSmtpConfig *cfg, const HlSmtpMessage *msg,
                     const char **err_msg)
{
    if (!cfg || !msg) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }

    /* Phase 1 (event-loop side in model 2): validate + authorize BEFORE any
     * resolve or socket work. Authorization is against the declared hostname
     * and never crosses to a worker. */
    if (smtp_validate_message(msg) != 0) {
        if (err_msg) *err_msg = "validation_failed";
        return -1;
    }
    if (hl_smtp_check_host(cfg, msg->host) != 0) {
        log_warn("smtp: host '%s' not in allowlist", msg->host);
        if (err_msg) *err_msg = "host_not_allowed";
        hl_smtp_audit_denied(msg);
        return -1;
    }

    int timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms
                                         : HL_SMTP_DEFAULT_TIMEOUT_MS;

    /* Execute-phase: inline on the calling thread here (the sync / no-loop
     * path; the SMTP worker runs the same function in model 2). No audit. */
    HlSmtpResult r;
    hl_smtp_execute(msg, cfg->tls, timeout_ms, &r);
    if (err_msg && r.token)
        *err_msg = r.token;

    /* Completion phase (event-loop side in model 2): the single audit record. */
    hl_smtp_audit_complete(msg, &r, NULL, NULL);
    return r.rc;
}
