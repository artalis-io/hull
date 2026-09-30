/**
 * @file cap/net_resolve.h
 * @brief Resolve host:port into the address list a KlConnectOp races.
 *
 * One resolver for every outbound transport Hull drives over KlConnectOp:
 * the private stream (cap/net_stream.c), SMTP (cap/smtp_transport.c) and the
 * SQL / KV wire transport (cap/db_transport.c). Each used to carry its own
 * copy of this loop, and the copies drifted - one dropped the IPv6 scope id,
 * so a link-local destination (fe80::1%eth0) could not be reached through it,
 * and only one skipped the resolver for an address literal.
 *
 * BLOCKING, and it is the system resolver on purpose: getaddrinfo honours
 * /etc/hosts, search domains and mDNS, which Keel's own nameserver-only
 * resolver does not. Call it where blocking is allowed - a pool worker, or a
 * thread that already owns the connection - never on the event loop.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_NET_RESOLVE_H
#define HULL_CAP_NET_RESOLVE_H

#include <keel/sockaddr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fill @p out with up to @p max stream addresses for @p host : @p port.
 *
 * An address literal is parsed without a lookup. Otherwise the order is the
 * resolver's (it has already applied the host's address-selection policy),
 * IPv4 and IPv6 only, each IPv6 address keeping its scope id.
 *
 * @return the number of addresses written (0 when nothing resolved, or on a
 *         NULL / empty host, a port outside 1..65535, or max < 1)
 */
int hl_net_resolve(const char *host, int port, KlSockAddr *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CAP_NET_RESOLVE_H */
