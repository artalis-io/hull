/**
 * @file cap/smtp_transport.h
 * @brief SMTP byte transport over Keel v3 public primitives.
 *
 * Hull-local adapter that composes Keel's public transport surface -
 * KlConnectOp (resolve + Happy-Eyeballs connect), KlStream (queued writes,
 * strict read pause/resume, graceful close), KlTls (implicit + in-place
 * STARTTLS upgrade), timers, and the POSIX socket provider - into the byte
 * transport the synchronous SMTP capability drives.
 *
 * Ownership split (docs/smtp_keel_client_design.md sections 1-2): Hull's SMTP
 * capability (cap/smtp.c) owns policy and protocol - host authorization,
 * validation, the conversation, message formatting, AUTH, and the stable error
 * tokens. This transport owns only the bytes: name resolution, connection,
 * ordered reads/writes, TLS attachment, and close.
 *
 * Scheduling model (design section 6, model 1): the compatibility wrapper drives
 * a PRIVATE, operation-local KlEventCtx that the synchronous entry points pump to
 * a terminal state. No server event context is entered recursively; the worker
 * boundary is Slice 2c and out of scope here.
 *
 * The incremental SMTP reply parser lives here, co-located with the byte source:
 * the KlStream deliver callback does NOT map 1:1 to SMTP lines, so partial bytes
 * are retained across reads, multiple complete lines in one read are accepted,
 * continuation lines (NNN-) are gathered until the terminating line (NNN ), and a
 * single line and the whole multiline response are bounded. The 3-digit code is
 * checked via hl_smtp_parse_response (cap/smtp.h), the one code parser.
 *
 * These entry points block the caller (pumping the private event loop) until the
 * requested step reaches a terminal state or the bounded deadline elapses; they
 * are synchronous exactly as cap/smtp.c requires.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_SMTP_TRANSPORT_H
#define HL_CAP_SMTP_TRANSPORT_H

#include <stddef.h>

/** Opaque SMTP transport operation (one connection). */
typedef struct HlSmtpTransport HlSmtpTransport;

/**
 * Resolve @p host and connect to @p port, bounded by @p timeout_ms.
 *
 * Runs a sandbox-compatible blocking system resolve (getaddrinfo - /etc/hosts,
 * search domains, the OS resolver the kernel-sandbox network grant permits) then
 * races the resolved address list through KlConnectOp and brings a KlStream up on
 * the winning socket. Resolution is against @p host exactly (the connected
 * address never widens authority: authorization is the caller's, already done
 * against the declared hostname before this is called).
 *
 * Returns a transport handle on success, or NULL on resolve / connect / deadline
 * failure (the caller maps NULL to "connect_failed").
 */
HlSmtpTransport *hl_smtp_transport_connect(const char *host, int port,
                                           int timeout_ms);

/**
 * Implicit-TLS (SMTPS) handshake BEFORE any application bytes are read.
 *
 * @p tls_cfg is the opaque KlTlsConfig* from HlSmtpConfig.tls (its factory + ctx
 * carry the CA bundle and verification policy). @p host is the SNI / verification
 * hostname. Must be called immediately after connect, before reading the
 * greeting.
 *
 * Returns 0 on a verified handshake, -1 on failure (the caller maps -1 to
 * "tls_handshake_failed"). On failure there is NO plaintext fallback: the caller
 * treats it as terminal.
 */
int hl_smtp_transport_implicit_tls(HlSmtpTransport *t, const char *host,
                                    void *tls_cfg, int timeout_ms);

/**
 * In-place STARTTLS upgrade on the live plaintext connection.
 *
 * Preconditions the caller guarantees: it has sent "STARTTLS\r\n", read the 220
 * reply, and holds no buffered plaintext past that reply. This pauses the
 * plaintext read side and hands the socket to the TLS session BEFORE any further
 * plaintext read can consume ClientHello bytes (Slice 2b invariant 2), then
 * drives the handshake to completion.
 *
 * @p tls_cfg / @p host as in hl_smtp_transport_implicit_tls.
 *
 * Returns 0 on a verified handshake, -1 on failure (the caller maps -1 to
 * "tls_handshake_failed"). NO plaintext fallback on failure.
 */
int hl_smtp_transport_starttls(HlSmtpTransport *t, const char *host,
                               void *tls_cfg, int timeout_ms);

/** 1 if a verified TLS session is active on the transport, else 0. */
int hl_smtp_transport_tls_active(const HlSmtpTransport *t);

/**
 * Write @p len bytes, all-or-none, draining the write queue under backpressure.
 *
 * Pumps the private event loop until every byte has been sent (queued writes
 * flushed on write-readiness) or the deadline elapses. Routes through the TLS
 * session when one is active. Returns 0 on success, -1 on error / timeout (the
 * caller maps -1 to "send_failed" or the per-command token at its call site).
 */
int hl_smtp_transport_write(HlSmtpTransport *t, const void *data, size_t len,
                            int timeout_ms);

/**
 * Read one complete SMTP reply (possibly multiline), bounded by @p timeout_ms.
 *
 * Pumps the private event loop, running the incremental parser over delivered
 * bytes until a terminating reply line is assembled. Copies up to @p size-1 bytes
 * of the raw reply text into @p buf (NUL-terminated) and returns the 3-digit
 * code, or -1 on parse error / oversize / EOF / timeout.
 */
int hl_smtp_transport_read_reply(HlSmtpTransport *t, char *buf, int size,
                                 int timeout_ms);

/**
 * Best-effort graceful close: TLS close_notify (if active), graceful KlStream
 * close, and drive the private loop to confirmed detachment. Idempotent.
 */
void hl_smtp_transport_shutdown(HlSmtpTransport *t);

/**
 * Free the transport. Retires every socket, timer, watcher, TLS object, stream,
 * and the private event context exactly once (safe after or without shutdown).
 */
void hl_smtp_transport_free(HlSmtpTransport *t);

#endif /* HL_CAP_SMTP_TRANSPORT_H */
