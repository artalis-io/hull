/*
 * cap/smtp.h — SMTP email capability
 *
 * HlSmtpConfig holds runtime configuration (host allowlist, timeout, TLS
 * context) — typically set once at startup from the app manifest.
 *
 * HlSmtpMessage carries per-send parameters: connection details (host,
 * port, credentials, TLS mode) and the RFC 5322 envelope (from, to, cc,
 * reply-to, subject, body).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_H
#define HL_CAP_SMTP_H

/* ── Runtime configuration (per-app, set once) ───────────────────── */

typedef struct HlSmtpConfig {
    const char **allowed_hosts;   /* Host allowlist (case-insensitive) */
    int          host_count;      /* Number of entries in allowed_hosts */
    int          timeout_ms;      /* Connect/send/recv timeout (0 = default) */
    void        *tls;             /* KlTlsConfig* — opaque to callers */
} HlSmtpConfig;

/* ── Per-message envelope + connection params ────────────────────── */

typedef struct {
    /* Connection */
    const char  *host;            /* SMTP server hostname */
    int          port;            /* SMTP server port (25/465/587) */
    int          use_tls;         /* 0 = none, 1 = STARTTLS, 2 = implicit TLS */
    const char  *username;        /* AUTH PLAIN username (NULL = no auth) */
    const char  *password;        /* AUTH PLAIN password (NULL = no auth) */

    /* Envelope */
    const char  *from;            /* Sender address (required) */
    const char  *to;              /* Primary recipient (required) */
    const char **cc;              /* CC recipients array (NULL = none) */
    int          cc_count;        /* Number of CC recipients */
    const char  *reply_to;        /* Reply-To address (NULL = omit) */

    /* Content */
    const char  *subject;         /* Email subject (required) */
    const char  *body;            /* Email body (required) */
    const char  *content_type;    /* MIME type (NULL = "text/plain") */
} HlSmtpMessage;

/* ── Public API ──────────────────────────────────────────────────── */

/**
 * Send an email via SMTP.
 *
 * Validates fields, checks host allowlist, connects with timeout,
 * optionally upgrades to TLS, authenticates, and delivers the message.
 *
 * Returns 0 on success, -1 on failure.
 */
int hl_cap_smtp_send(const HlSmtpConfig *cfg, const HlSmtpMessage *msg);

/* ── Internal helpers (exposed for unit testing) ─────────────────── */

/** Base64-encode src into dst. Returns output length or -1 on error. */
int hl_smtp_base64_encode(const unsigned char *src, int src_len,
                          char *dst, int dst_len);

/** Check if host is in cfg->allowed_hosts. Returns 0 if allowed, -1 if not. */
int hl_smtp_check_host(const HlSmtpConfig *cfg, const char *host);

/** Parse 3-digit SMTP response code from a line. Returns code or -1. */
int hl_smtp_parse_response(const char *line, int len);

/** Format an RFC 5322 message into buf with dot-stuffing. Returns length or -1. */
int hl_smtp_format_message(const HlSmtpMessage *msg, char *buf, int size);

#endif /* HL_CAP_SMTP_H */
