/*
 * shared/tls_client.h: blocking TLS client helper over Keel's KlTls
 *
 * The transport mechanics shared by protocols that upgrade a plaintext socket
 * to TLS on demand (SMTP STARTTLS, PostgreSQL SSLRequest): a blocking
 * handshake over the embedded CA bundle plus read/write that tunnel through
 * KlTls. Each protocol keeps its own negotiation (STARTTLS vs SSLRequest);
 * only the handshake + io are shared. Compiled when HL_LINK_TLS is on
 * (an HTTP half or PostgreSQL enabled).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_TLS_CLIENT_H
#define HL_SHARED_TLS_CLIENT_H

#include <stddef.h>
#include <sys/types.h>

/* Opaque owner of the per-connection KlTlsCtx + KlTls session. */
typedef struct HlTlsClient HlTlsClient;

/*
 * Run a blocking TLS client handshake on an already-connected socket @p fd.
 * @p host is used for SNI and, when @p verify is non-zero, certificate
 * hostname verification against the embedded CA bundle (sslmode=verify-full).
 * When @p verify is zero the server certificate is accepted without
 * verification (sslmode=require/prefer). @p timeout_ms bounds the handshake
 * (<= 0 waits indefinitely). Returns a session on success (free with
 * hl_tls_client_free) or NULL on failure.
 */
HlTlsClient *hl_tls_client_handshake(int fd, const char *host,
                                     int verify, int timeout_ms);

/* Blocking plaintext read/write tunnelled through the TLS session. Return
 * bytes transferred (> 0), or <= 0 on error / close. */
ssize_t hl_tls_client_read(int fd, HlTlsClient *c, void *buf, size_t len);
ssize_t hl_tls_client_write(int fd, HlTlsClient *c, const void *buf, size_t len);

/* Tear down the TLS session and its context. Idempotent; NULL-safe. */
void hl_tls_client_free(HlTlsClient *c);

#endif /* HL_SHARED_TLS_CLIENT_H */
