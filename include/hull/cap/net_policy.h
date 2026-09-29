/**
 * @file cap/net_policy.h
 * @brief Outbound reach authorization: may this app reach this host and port.
 *
 * This header is the POLICY half of the private outbound stream, and it is
 * GENERIC: a host/port grant in, allow-or-reason out, with no protocol, no
 * socket, no resolver and no event loop in scope. Protocol grants are built on
 * top of it (cap/ssh_policy.h adds the SSH login and the relay); the transport
 * half (cap/net_stream.h) runs after one of them has said yes.
 *
 * The security invariant Hull wants here is "capability denial occurs BEFORE
 * network access", and the cheapest way to guarantee it is to make the check a
 * pure function of the manifest plus the requested destination, testable on
 * its own. A denial cannot race a DNS query that was never started.
 *
 * Fails closed. Every path that is not an explicit allow is a deny:
 *
 *   - a NULL or undeclared grant              -> HL_NET_REACH_NO_GRANT
 *   - a grant with no hosts or no ports       -> HL_NET_REACH_NO_GRANT
 *   - a NULL or empty host, or none matching  -> HL_NET_REACH_HOST
 *   - a port out of range or not in the list  -> HL_NET_REACH_PORT
 *
 * The reasons carry no wording: the grant's NAME (`ssh.connect`,
 * `ssh.tunnel`, ...) belongs to the protocol layer, which maps each reason to
 * a message naming the list that refused.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_NET_POLICY_H
#define HULL_CAP_NET_POLICY_H

#include "hull/manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of a reach check. Only HL_NET_REACH_OK permits a connection. */
typedef enum HlNetReach {
    HL_NET_REACH_OK = 0,
    HL_NET_REACH_NO_GRANT,   /* no grant, or one with an empty host or port list */
    HL_NET_REACH_HOST,       /* host matched no pattern in the grant */
    HL_NET_REACH_PORT        /* port is not in the grant's port list */
} HlNetReach;

/**
 * Authorize one outbound reach.
 *
 * @param g     the host/port grant (NULL denies)
 * @param host  hostname or IP literal (NULL or empty denies)
 * @param port  TCP port, 1..65535 (anything else denies)
 * @return HL_NET_REACH_OK, or the specific reason for refusal
 *
 * Pure: no allocation, no I/O, no globals. Safe to call before the sandbox is
 * applied and safe to call from a test with a hand-built grant.
 */
HlNetReach hl_net_check_reach(const HlManifestNetConnect *g,
                              const char *host, int port);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CAP_NET_POLICY_H */
