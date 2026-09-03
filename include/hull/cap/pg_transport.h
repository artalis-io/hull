/**
 * @file cap/pg_transport.h
 * @brief PostgreSQL byte transport over Keel v3 public primitives.
 *
 * Hull-local adapter that composes Keel's public transport surface -
 * KlConnectOp (resolve + Happy-Eyeballs racing connect), a private KlEventCtx to
 * drive it, and the POSIX socket provider's raw ops - into the blocking byte
 * transport the synchronous PostgreSQL client (cap/pg_conn.c) drives.
 *
 * Ownership split (docs/pg_keel_transport_slice3.md): pg_conn.c owns the wire
 * protocol (framing, SCRAM auth, the $n rewrite, typed decode, migrations, the
 * SSLRequest / sslmode negotiation, and the stable error tokens). This transport
 * owns only the bytes: name resolution, the connection race, blocking
 * reads/writes, an optional attached TLS session, and close.
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

#include <keel/connect_op.h>          /* KL_CONNECT_MAX_ADDRS, KlConnectResult */
#include <keel/connect_op_detail.h>   /* opt-in layout: embed a KlConnectOp (storage only) */
#include <keel/event_ctx.h>           /* KlEventCtx (embedded, connect-time only) */
#include <keel/sockaddr.h>            /* KlSockAddr */
#include <keel/socket.h>              /* KlSocketProvider, KlSocketHandle */
#include <keel/allocator.h>           /* KlAllocator (event-ctx lifetime) */

struct HlTlsClient;   /* shared/tls_client.h; the optional attached TLS session */

/* One connection's byte transport. Standalone storage: the future pg_conn.c
 * embeds or owns one of these. Fields are documented as owned vs borrowed in the
 * header banner above; the struct is exposed (not opaque) so pg_conn.c can embed
 * it by value in HlPgConn, exactly as the design's Amendment 1 anticipates. */
typedef struct PgTransport {
    /* Borrowed, immutable, outlives the connection (Amendment 4). */
    const KlSocketProvider *sp;

    /* Owned: the private connect-time event context. Created only when a race is
     * needed (hl_pg_transport_connect); the adopt path leaves it uninitialized. */
    KlEventCtx  ev;
    int         ev_ready;      /* 1 once kl_event_ctx_init succeeded            */
    KlAllocator alloc;         /* event-ctx allocator (event-loop lifetime)     */

    /* Owned: the embedded KlConnectOp and its resolved address set. */
    KlConnectOp connect_op;       /* embedded (connect_op_detail.h: storage only) */
    int         connect_started;  /* 1 once kl_connect_op_start succeeded        */
    KlSockAddr  addrs[KL_CONNECT_MAX_ADDRS];
    int         naddrs;
    KlSocketHandle attempt_fd[KL_CONNECT_MAX_ADDRS]; /* in-flight racing fds     */
    int         attempt_armed[KL_CONNECT_MAX_ADDRS]; /* 1 if a watcher is armed  */

    /* Connect outcome. */
    int             connect_done;      /* on_done fired                          */
    int             connect_detached;  /* on_detach fired                        */
    KlConnectResult connect_result;
    int             connect_error;

    /* Overall connect deadline (TCP establishment only, design D3). */
    int64_t   deadline_timer;   /* Keel timer id, or -1                          */
    uint64_t  deadline_ms;      /* absolute monotonic instant, or 0 = unbounded  */
    int       deadline_fired;

    /* RFC 8305 Connection Attempt Delay timer (the Happy-Eyeballs stagger). */
    int64_t   delay_timer;      /* Keel timer id, or -1                          */

    /* The winning connected descriptor (blocking after connect). */
    KlSocketHandle fd;

    /* Owned: optional attached TLS session. When non-NULL, send/recv tunnel
     * through it (design D2); the transport frees it in close. */
    struct HlTlsClient *tls;

    int closed;   /* 1 once hl_pg_transport_close has run (idempotency guard)    */
} PgTransport;

/**
 * Resolve @p host / @p port, race the addresses through KlConnectOp on a private
 * event loop, and leave @p t holding the winning blocking descriptor.
 *
 * An IP-literal @p host is parsed directly via kl_sockaddr_parse (no DNS,
 * matching the IP-literal-only databases.dynamic CIDR gate); a hostname is
 * resolved through a sandbox-compatible blocking getaddrinfo adapter kept here
 * (never in pg_conn.c). At most KL_CONNECT_MAX_ADDRS ordered addresses are raced.
 *
 * @p timeout_ms > 0 bounds TCP establishment only (design D3): one absolute
 * connect deadline is armed after resolution, and the Happy-Eyeballs stagger does
 * not refresh it. @p timeout_ms <= 0 arms no deadline (unbounded connect,
 * preserving pg_connect's prior behavior).
 *
 * @p sp_or_NULL, when non-NULL, is a caller-supplied provider (the test seam);
 * NULL selects the default POSIX provider. The reference is borrowed and must
 * outlive @p t.
 *
 * On failure the transport cancels the op and pumps to confirmed detachment
 * before releasing embedded storage. Returns 0 on success (with @p t ready for
 * send/recv), or -1 with a message written to @p errbuf (which may be NULL),
 * mirroring pg_conn.c's set_err style. On failure @p t holds no live descriptor.
 */
int hl_pg_transport_connect(PgTransport *t, const char *host, const char *port,
                            int timeout_ms, const KlSocketProvider *sp_or_NULL,
                            char *errbuf, size_t errlen);

/**
 * Adopt an already-connected, blocking descriptor @p fd (design Amendment 2).
 *
 * Takes ownership of @p fd exactly once, associates it with @p sp_or_NULL (or the
 * default POSIX provider when NULL), and skips resolution, connection racing, and
 * event-context creation. Subsequent send/recv/close use the same path as a raced
 * connection. This backs the existing hl_pg_conn_start(conn, fd, dsn) test API.
 *
 * Returns 0 on success, -1 on a bad argument.
 */
int hl_pg_transport_adopt(PgTransport *t, int fd,
                          const KlSocketProvider *sp_or_NULL);

/**
 * Attach an optional TLS session to route subsequent send/recv through (design
 * D2). @p tls is created and handshaked by the caller (pg_conn.c) over the
 * transport's blocking descriptor. Ownership transfers to the transport, which
 * frees it in hl_pg_transport_close. Passing NULL detaches (rarely used).
 */
void hl_pg_transport_attach_tls(PgTransport *t, struct HlTlsClient *tls);

/**
 * One blocking send / recv over the transport, EINTR-retried and TLS-aware.
 *
 * When a TLS session is attached the bytes tunnel through hl_tls_client_write /
 * _read; otherwise the provider send / recv is used with SIGPIPE suppression
 * (set_nosigpipe on the fd plus the nosignal send flag where the platform has
 * one). These are SINGLE operations: the provider (or TLS) may return short, so
 * callers that need every byte use hl_pg_transport_send_all. Returns the byte
 * count (> 0), 0 on orderly close, or -1 on error.
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
 * Close the transport exactly once: TLS shutdown (if attached), TLS free, then
 * provider close of the winning descriptor. Idempotent and NULL-safe. Does NOT
 * free @p t itself (the storage is caller-owned / embedded); it only retires the
 * descriptor and TLS session. On a still-live (never-detached) connect op it
 * cancels and pumps to confirmed detachment before retiring storage.
 */
void hl_pg_transport_close(PgTransport *t);

#endif /* HL_CAP_PG_TRANSPORT_H */
