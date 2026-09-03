/**
 * @file cap/pg_transport.h
 * @brief PostgreSQL byte transport over Keel v3 public primitives.
 *
 * Hull-local adapter that composes Keel's public transport surface -
 * KlConnectOp (resolve + Happy-Eyeballs racing connect), a private KlEventCtx to
 * drive it, and a KlSocketProvider's raw ops - into the blocking byte transport
 * the synchronous PostgreSQL client (cap/pg_conn.c) drives.
 *
 * Ownership split (docs/pg_keel_transport_slice3.md): pg_conn.c owns the wire
 * protocol (framing, SCRAM auth, the $n rewrite, typed decode, migrations, the
 * SSLRequest / sslmode negotiation, and the stable error tokens). This transport
 * owns only the bytes: name resolution, the connection race, blocking
 * reads/writes, an optional attached TLS session, and close.
 *
 * Heap-allocated + opaque (Amendment 1, refined at Checkpoint 2 review): the
 * transport is a HEAP allocation reached through an opaque handle. pg_conn.c holds
 * a PgTransport* in HlPgConn, never an embedded value. This is what lets the
 * exceptional non-detachment path (a connect op that will not confirm detachment)
 * leak the WHOLE allocation intentionally and safely - a live op keeps
 * referencing its own storage, so the storage must never be freed under it, and
 * only a separately-owned heap block can be dropped whole. Mirrors the SMTP
 * transport.
 *
 * Scheduling model (design D1): KlConnectOp is asynchronous, so the transport
 * owns a PRIVATE, operation-local KlEventCtx used ONLY during connect
 * establishment. The transport pumps that loop to a terminal state, receives the
 * winning descriptor, and then set_blocking()s it. After that point there is no
 * event loop: startup, SCRAM auth, the TLS handshake, and every query are plain
 * blocking recv / send on the winning fd. Unlike the SMTP transport this uses no
 * KlStream and no post-connect event loop.
 *
 * TLS (design D2): TLS stays the shared hl_tls_client_* helpers layered over the
 * provider-created blocking descriptor; the transport merely retains the optional
 * HlTlsClient and routes send/recv through it once attached. The transport itself
 * runs no handshake (pg_conn.c owns the SSLRequest negotiation and hands the
 * resulting session here via hl_pg_transport_attach_tls).
 *
 * Ownership (design Amendment 1 / Amendment 4 / frozen rules): the transport
 * retains a BORROWED immutable KlSocketProvider reference (the default POSIX
 * provider, or a test-supplied one) whose lifetime exceeds the connection, and it
 * OWNS the private KlEventCtx, the embedded KlConnectOp and its timers, the
 * resolved address set (at most KL_CONNECT_MAX_ADDRS, valid through confirmed
 * detachment), the winning descriptor, and the optional attached HlTlsClient.
 * Every descriptor - winner, loser, or failure-path - is disposed through the
 * provider, never a bare close(2).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_PG_TRANSPORT_H
#define HL_CAP_PG_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */

struct KlSocketProvider;  /* keel/socket.h; the borrowed provider (opaque here) */
struct HlTlsClient;       /* shared/tls_client.h; the optional attached TLS session */

/* Opaque, heap-allocated. pg_conn.c stores a PgTransport* and reaches every field
 * through the API below. The concrete struct is defined in pg_transport.c so the
 * exceptional non-detachment path can drop the whole heap block (see the banner).
 * The direct-include unit test sees the full definition for white-box checks. */
typedef struct PgTransport PgTransport;

/**
 * Resolve @p host / @p port, race the addresses through KlConnectOp on a private
 * event loop, and return a transport holding the winning blocking descriptor.
 *
 * An IP-literal @p host is parsed directly via kl_sockaddr_parse (no DNS,
 * matching the IP-literal-only databases.dynamic CIDR gate); a hostname is
 * resolved through a sandbox-compatible blocking getaddrinfo adapter kept here
 * (never in pg_conn.c). At most KL_CONNECT_MAX_ADDRS ordered addresses are raced.
 *
 * @p timeout_ms > 0 bounds TCP establishment only (design D3): one absolute
 * connect deadline is armed after resolution, and the Happy-Eyeballs stagger does
 * not refresh it. @p timeout_ms <= 0 is TRULY UNBOUNDED (no total deadline, no
 * hidden ceiling): the loop pumps in bounded event-loop increments until the op
 * completes, preserving pg_connect's prior select(..., NULL) contract.
 *
 * @p sp_or_NULL, when non-NULL, is a caller-supplied provider (the test seam);
 * NULL selects the default POSIX provider. The reference is borrowed and must
 * outlive the returned transport. The provider MUST supply the required op subset
 * (socket/connect/close/send/recv/get_so_error/set_nonblocking/set_blocking) and
 * advertise KL_SOCK_CAP_NATIVE_FD (the private event loop watches its handles);
 * otherwise this fails closed.
 *
 * Returns a non-NULL transport on success (ready for send/recv). On failure
 * returns NULL and writes a message to @p errbuf (which may be NULL), mirroring
 * pg_conn.c's set_err style; the allocation is either freed (op detached) or, if
 * a live op will not confirm detachment, intentionally leaked (never freed under a
 * live op).
 */
PgTransport *hl_pg_transport_connect(const char *host, const char *port,
                                     int timeout_ms,
                                     const struct KlSocketProvider *sp_or_NULL,
                                     char *errbuf, size_t errlen);

/**
 * Adopt an already-connected, blocking descriptor @p fd (design Amendment 2).
 *
 * Takes ownership of @p fd exactly once, associates it with @p sp_or_NULL (or the
 * default POSIX provider when NULL), and skips resolution, connection racing, and
 * event-context creation. Subsequent send/recv/close use the same path as a raced
 * connection. This backs the existing hl_pg_conn_start(conn, fd, dsn) test API.
 * The provider is validated (required ops + KL_SOCK_CAP_NATIVE_FD) as in connect.
 *
 * Returns a non-NULL transport on success. On failure returns NULL, and the
 * descriptor is CONSUMED (closed exactly once through the provider) on every
 * outcome where the provider can close it - i.e. an allocation failure after a
 * valid provider was resolved - so the caller never leaks it. The ONLY
 * non-consuming failures are an invalid @p fd (< 0, nothing to close) or an
 * invalid provider (@p sp_or_NULL missing a required op / capability, so there is
 * no way to close @p fd); in those two cases the caller still owns @p fd. This
 * lets hl_pg_conn_start keep its "takes ownership of fd, closed on every failure"
 * contract with a valid (default) provider.
 */
PgTransport *hl_pg_transport_adopt(int fd,
                                   const struct KlSocketProvider *sp_or_NULL,
                                   char *errbuf, size_t errlen);

/**
 * Attach a TLS session to route subsequent send/recv through (design D2). ONE-SHOT
 * and fallible: requires a live descriptor and a non-NULL @p tls, and rejects a
 * second attachment (so an owned session is never silently dropped). @p tls is
 * created and handshaked by the caller (pg_conn.c) over the transport's blocking
 * descriptor; on success ownership transfers to the transport, which frees it in
 * hl_pg_transport_close. Returns 0 on success, -1 on rejection (no descriptor,
 * NULL session, or already attached) - on -1 the caller still owns @p tls.
 */
int hl_pg_transport_attach_tls(PgTransport *t, struct HlTlsClient *tls);

/**
 * One blocking send / recv over the transport, EINTR-retried and TLS-aware.
 *
 * When a TLS session is attached the bytes tunnel through hl_tls_client_write /
 * _read; otherwise the provider send / recv is used with SIGPIPE suppression
 * (set_nosigpipe on the fd plus the nosignal send flag where the platform has
 * one). Interrupted-retry classification uses the provider's io_status when it
 * supplies one, falling back to hosted errno only when absent. These are SINGLE
 * operations: the provider (or TLS) may return short, so callers that need every
 * byte use hl_pg_transport_send_all. Returns the byte count (> 0), 0 on orderly
 * close, or -1 on error.
 */
ssize_t hl_pg_transport_send(PgTransport *t, const uint8_t *buf, size_t len);
ssize_t hl_pg_transport_recv(PgTransport *t, uint8_t *buf, size_t len);

/**
 * Send every byte of @p buf (all-or-error), looping over hl_pg_transport_send.
 * This is conn_send's semantics: returns 0 iff all @p len bytes were written, -1
 * on any short/failed write.
 */
int hl_pg_transport_send_all(PgTransport *t, const uint8_t *buf, size_t len);

/**
 * The raw integer descriptor (POSIX KlSocketHandle == int) for handing to the
 * hl_tls_client_* helpers, which take an int fd. Returns -1 when no descriptor is
 * held.
 */
int hl_pg_transport_fd(const PgTransport *t);

/**
 * Close and free the transport. One close path: TLS shutdown (if attached), TLS
 * free, then provider close of the winning descriptor. Drives a still-live raced
 * connect op to confirmed detachment first (frozen rule 4).
 *
 * Returns 0 on full success: the descriptor is retired and the heap allocation is
 * FREED - the caller MUST drop its pointer and never reuse it.
 *
 * Returns -1 iff a live connect op would NOT confirm detachment within the
 * teardown bound. In that case the ENTIRE allocation is PRESERVED (not freed, not
 * marked closed): the caller may retry hl_pg_transport_close later, or drop its
 * pointer to intentionally leak the block safely. Freeing storage a live op still
 * references is never done. NULL-safe (returns 0).
 */
int hl_pg_transport_close(PgTransport *t);

#endif /* HL_CAP_PG_TRANSPORT_H */
