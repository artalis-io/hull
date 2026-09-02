/**
 * @file cap/smtp.h
 * @brief SMTP email capability - outbound send with STARTTLS / TLS.
 *
 * #HlSmtpConfig holds runtime configuration (host allowlist, timeout,
 * TLS context) - typically set once at startup from the app manifest.
 *
 * #HlSmtpMessage carries per-send parameters: connection details (host,
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
    void        *tls;             /* KlTlsConfig* - opaque to callers */
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
int hl_cap_smtp_send(const HlSmtpConfig *cfg, const HlSmtpMessage *msg,
                     const char **err_msg);

/* ── Execute-phase ───────────────────────────────────────────────── */

/**
 * Terminal result of the SMTP execute-phase. Carries only a stable outcome
 * (a static string literal token, safe to cross a worker -> event-loop
 * boundary) so the execute-phase can run on a worker thread without touching
 * audit writers or runtime objects.
 */
typedef struct HlSmtpResult {
    int         rc;               /* 0 = success, -1 = failure */
    const char *token;            /* stable error token, or NULL on success */
    int         teardown_leaked;  /* 1 if transport teardown could not confirm
                                     detachment (resources intentionally leaked) */
} HlSmtpResult;

/**
 * Run the SMTP conversation (connect -> optional TLS -> greeting/EHLO/AUTH ->
 * MAIL/RCPT/DATA -> QUIT -> confirmed teardown) to a terminal state against
 * @p msg, using the opaque @p tls_cfg (KlTlsConfig*). Emits no audit and
 * touches no runtime objects. Fills *out and returns out->rc.
 *
 * This is the phase hl_cap_smtp_send runs inline (sync / no-loop path) and the
 * SMTP worker will run on a pool thread (model 2, docs/smtp_keel_slice2c_plan.md).
 */
int hl_smtp_execute(const HlSmtpMessage *msg, void *tls_cfg, int timeout_ms,
                    int (*cancel_poll)(void *), void *cancel_user,
                    HlSmtpResult *out);

/* ── Audit helpers (single record; shared sync + model-2 async) ──────── */

/** Emit the "denied" audit record for a host-allowlist rejection (submit side). */
void hl_smtp_audit_denied(const HlSmtpMessage *msg);

/**
 * Emit the single completion audit record. @p schedule (a scheduling-failure tag)
 * and @p terminal (a cancel/deadline tag) are written only when non-NULL;
 * r->teardown_leaked adds teardown:leaked. Audited exactly once per send.
 */
void hl_smtp_audit_complete(const HlSmtpMessage *msg, const HlSmtpResult *r,
                            const char *schedule, const char *terminal);

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
